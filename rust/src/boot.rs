// SPDX-License-Identifier: MIT
//! Stage 1: start-up decisions and the menu (docs/SPEC.md §7 and §9).
//!
//! Its data is a set of globals, as in C, and for one reason: MIDI mode calls back in (the `mu_` hooks below) while
//! the menu that started it is still running. So nothing here keeps a reference to a global across a call that can
//! reach a hook: each use takes it afresh.
use crate::board::{KEY_NEXT, KEY_PREV, KEY_SELECT, board_hold, board_init, board_keys, board_restore, board_wait};
use crate::formats::{self, App, AppState, Config, Guard, Header, Nor, Request, Updater, Work, crc16, le16};
use crate::display::{GREEN, GREY, RED, WHITE, YELLOW};
use crate::hw::{Reg, Text, irq_off, retained, sync_cpu};
use crate::platform::{
    flash_capacity, flash_erase, flash_init, flash_jedec, flash_program, flash_read, flash_read_width, flash_release, hal_delay_us, hal_efuse, hal_fault_clear,
    hal_reset, hal_xip,
};
use crate::sdboot::{sd_probe, sd_stage2};
use crate::stage0::rom_download;
use crate::{Aligned, Global, contract, display, text};

const SMK: bool = cfg!(smk);
const BOARD: u32 = if SMK { 2 } else { 1 };
const TITLE: Text = if SMK { text!("SMK-37 M-BOOTLOADER") } else { text!("FM-1 M-BOOTLOADER") };
const CONTROLS: Text = if SMK { text!("< > MOVE SEQ PLAY OK") } else { text!("OCT-/+ MOVE PLAY OK") };
const HOLD: Text = if SMK { text!("HOLD SEQ PLAY 3 S") } else { text!("HOLD PLAY 3 S") };
const RELEASE: Text = if SMK { text!("RELEASE SEQ PLAY") } else { text!("RELEASE PLAY") };
const NONE: Text = text!("");
/// The version, in stage 0's header.
const VERSION: Text = unsafe { Text::at(0x01c0_2050) };
const STATES: [Text; 4] = [text!("APP NOT FOUND"), text!("APP OK"), text!("APP BAD CRC"), text!("APP INCOMPLETE")];
/// What survives a reset: docs/SPEC.md §6.1.
const RETAINED: usize = 0x01c7_fd80;
/// What the app is handed.
const APP_CONTEXT: usize = 0x01c7_fe08;

// M-VAVE's updater reads what it's given in words, and crashes if these aren't aligned (§6.4).
static WORK: Global<Aligned<{ formats::WORK_SIZE }>> = Global::new(Aligned([0; formats::WORK_SIZE]));
// These four keep their C names, by which tests/updater_context_test.py finds them in the linked image.
#[cfg_attr(target_arch = "pi32v2", unsafe(export_name = "request"))]
static REQUEST: Global<Aligned<80>> = Global::new(Aligned([0; 80]));
#[cfg_attr(target_arch = "pi32v2", unsafe(export_name = "clock_meta"))]
static CLOCK_META: Global<Aligned<52>> = Global::new(Aligned([0; 52]));
#[cfg_attr(target_arch = "pi32v2", unsafe(export_name = "memory_meta"))]
static MEMORY_META: Global<Aligned<60>> = Global::new(Aligned([0; 60]));
#[cfg_attr(target_arch = "pi32v2", unsafe(export_name = "update_meta"))]
static UPDATE_META: Global<Aligned<24>> = Global::new(Aligned([0; 24]));
static APP: Global<App> = Global::new(App::EMPTY);
static HEADER: Global<Header> = Global::new(Header::EMPTY);
static NOR: Global<Nor> = Global::new(Nor::new(flash_read, flash_program, flash_erase));
static JEDEC: Global<u32> = Global::new(0);
static UNIT_KEY: Global<u16> = Global::new(0);
static TRIM: Global<u16> = Global::new(0);
static MAC: Global<[u8; 8]> = Global::new([0; 8]);
static EFFECTIVE_WIDTH: Global<u32> = Global::new(0);
static MOUNTED: Global<bool> = Global::new(false);
static XIP_ATTEMPTED: Global<bool> = Global::new(false);
static SCREEN_READY: Global<bool> = Global::new(false);
static REASON: Global<Option<Text>> = Global::new(None);

// Safety, for these: see the top of the file.
fn work() -> &'static mut Work {
    unsafe { &mut WORK.get().0 }
}

fn request() -> &'static mut Request {
    unsafe { &mut REQUEST.get().0 }
}

/// The values every known configuration has: §5.3.
const CONFIG: Config = Config::DEFAULTS;
const WIDTH: u32 = CONFIG.width as u32;
const BAUD: u32 = CONFIG.spi_divider as u32;

fn app() -> &'static mut App {
    unsafe { APP.get() }
}

fn header() -> &'static mut Header {
    unsafe { HEADER.get() }
}

fn nor() -> &'static mut Nor {
    unsafe { NOR.get() }
}

fn screen() -> bool {
    if !SCREEN_READY.read() {
        SCREEN_READY.set(display::init(BOARD));
    }
    SCREEN_READY.read()
}

fn clear_rows(first: u32, last: u32) {
    for row in first..=last {
        display::text(row, WHITE, NONE);
    }
}

/// A row of the screen, being put together.
struct Line {
    text: [u8; 20],
    len: usize,
}

impl Line {
    fn new(start: Text) -> Line {
        let mut line = Line { text: [0; 20], len: 0 };
        line.append(start);
        line
    }

    fn append(&mut self, text: Text) {
        for &b in text.as_bytes() {
            self.push(b);
        }
    }

    fn push(&mut self, b: u8) {
        if let Some(slot) = self.text.get_mut(self.len) {
            *slot = b;
            self.len += 1;
        }
    }

    fn number(&mut self, mut v: u32, base: u32, digits: usize) {
        let mut temp = [b'0'; 10];
        let mut n = 0;
        loop {
            temp[n] = b"0123456789ABCDEF"[(v % base) as usize];
            n += 1;
            v /= base;
            if v == 0 || n == temp.len() {
                break;
            }
        }
        for &b in temp[..n.max(digits).min(temp.len())].iter().rev() {
            self.push(b);
        }
    }

    fn show(&self, row: u32, color: u16) {
        display::line(row, color, &self.text[..self.len]);
    }
}

#[inline(never)]
fn identify() {
    JEDEC.set(flash_jedec());
    let capacity = flash_capacity();
    nor().flash.capacity = capacity;
    let mut mac = [255; 8];
    UNIT_KEY.set((hal_efuse(1, 3) >> 8) as u16);
    let (r2, r3) = (hal_efuse(0, 2), hal_efuse(0, 3));
    TRIM.set((r2 | r3 & 0xfff2_38fc) as u16);
    let (page, _) = work().split_first_chunk_mut::<256>().unwrap();
    if capacity >= 256 && nor().flash.read(capacity - 256, page) {
        mac = *page[64..].first_chunk().unwrap();
        let nonzero = page[..8].iter().any(|&b| b != 0);
        if UNIT_KEY.read() == 0xffff && nonzero && crc16(0, &page[..8]) == le16(page, 8) {
            UNIT_KEY.set(le16(page, 5));
        }
    }
    MAC.set(mac);
}

fn download_screen() -> ! {
    clear_rows(3, 14);
    display::text(3, GREEN, text!("DOWNLOAD MODE"));
    display::text(5, WHITE, text!("CONNECT USB TO PC"));
    display::text(6, WHITE, text!("TO WRITE FLASH."));
    display::text(8, WHITE, text!("POWER OFF AND ON"));
    display::text(9, WHITE, text!("TO EXIT."));
    rom_download()
}

fn release_select() {
    while board_keys() & KEY_SELECT != 0 {
        board_wait(20, true);
    }
}

/// MIDI mode: the hooks of `midi_update`, and the menu's item.
#[cfg(any(not(boot_sim), midi_mode))]
mod midi {
    use super::*;
    use crate::board::board_keys;
    use crate::midi_update::{self, Outcome, Unit, mu_run};
    use crate::platform::flash_uid;
    use crate::usb_midi::{usb_midi_poll, usb_midi_send, usb_midi_start, usb_midi_stop};
    use core::ffi::c_void;

    const BACK: Text = if SMK { text!("SEQ PLAY: BACK") } else { text!("PLAY: BACK") };
    const MENU_KEYS: Text = if SMK { text!("< > SEQ PLAY SEQ REC") } else { text!("OCT- OCT+ PLAY REC") };
    const BOOTLOADER: u8 = 1;
    // The menu came up by itself (no PLAY held at power-on: the app did not boot): it opens in MIDI mode, so a computer
    // can install or restart the unit with nobody at it (ALONE, for as long as it lasts). Only a key leaves it, any of
    // the four the menu reads, back to the whole menu with every action as before. STUCK: the keys down when it opened,
    // which count once they have come up.
    pub static AUTO: Global<bool> = Global::new(false);
    pub static ALONE: Global<bool> = Global::new(false);
    pub static STUCK: Global<u32> = Global::new(0);

    /// # Safety
    /// `data` holds `size` bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn mu_send(data: *const u8, size: u32) {
        unsafe { usb_midi_send(data, size) }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_idle() {
        for _ in 0..2 {
            usb_midi_poll();
            hal_delay_us(500);
        }
    }

    /// Called about once a millisecond: one odd scan mustn't stop an install. A key that was down when MIDI mode opened
    /// by itself counts once it has read as up for 50 scans in a row: past a release's bounce, and more than twice the
    /// 20 that leave, so a stuck key that reads up now and then doesn't make MIDI mode leave by itself.
    #[unsafe(no_mangle)]
    pub extern "C" fn mu_cancel() -> bool {
        static HELD: Global<u32> = Global::new(0);
        static UP: Global<[u8; 4]> = Global::new([0; 4]);
        let keys = board_keys();
        let mut up = UP.read();
        for (k, up) in up.iter_mut().enumerate() {
            *up = if keys & 1 << k != 0 { 0 } else { (*up + 1).min(50) };
            if *up == 50 {
                STUCK.set(STUCK.read() & !(1 << k));
            }
        }
        UP.set(up);
        let counted = if ALONE.read() { !STUCK.read() } else { KEY_SELECT };
        let held = if keys & counted != 0 { HELD.read() + 1 } else { 0 };
        HELD.set(held);
        held >= 20
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_unit_info(unit: &mut Unit) {
        unit.jedec = JEDEC.read();
        unit.capacity = flash_capacity();
        flash_uid(&mut unit.uid);
        unit.kind = BOOTLOADER;
        unit.version = VERSION.as_ptr().cast();
    }

    /// # Safety
    /// `out` has room for `size` bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn mu_read(address: u32, out: *mut c_void, size: u32) -> bool {
        unsafe { flash_read(core::ptr::null_mut(), address, out, size) }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_erase(address: u32) -> bool {
        flash_erase(core::ptr::null_mut(), address, midi_update::SECTOR as u32)
    }

    /// # Safety
    /// `data` holds `size` bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn mu_program(address: u32, data: *const c_void, size: u32) -> bool {
        unsafe { flash_program(core::ptr::null_mut(), address, data, size) }
    }

    fn frame(y: u32) {
        display::rect(10, y, 220, 12, WHITE);
        display::rect(12, y + 2, 216, 8, 0);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_progress(writing: bool, percent: u32) {
        static SHOWN: Global<u32> = Global::new(0);
        // A pass's start draws the screen for it.
        if percent == 0 {
            if !writing {
                clear_rows(5, 14);
                display::text(5, WHITE, text!("CHECKING PACKAGE"));
                frame(96);
                display::text(8, WHITE, text!("WRITING FIRMWARE"));
                frame(144);
                display::text(11, WHITE, BACK);
            } else {
                display::text(11, YELLOW, text!("DO NOT UNPLUG"));
            }
            SHOWN.set(0);
        }
        if percent != SHOWN.read() || percent == 100 {
            display::rect(12, if writing { 146 } else { 98 }, 216 * percent / 100, 8, GREEN);
        }
        SHOWN.set(percent);
    }

    fn screen() {
        clear_rows(3, 14);
        display::text(3, GREEN, text!("MIDI MODE"));
        // With the reason above it, why the app didn't start stays in view (§9.1).
        let state = app().state;
        display::text(4, if state == AppState::Ok { GREEN } else { RED }, STATES[state as usize]);
        display::text(5, WHITE, text!("CONNECT USB AND"));
        display::text(6, WHITE, text!("OPEN AN INSTALLER."));
        if ALONE.read() {
            display::text(8, WHITE, text!("TO OPEN THE MENU:"));
            display::text(9, WHITE, MENU_KEYS);
        } else {
            display::text(8, WHITE, BACK);
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_start_app() {
        hal_delay_us(200_000);
        usb_midi_stop();
        sync_cpu();
        // Safety: mu_run() has loaded and checked an app there, which takes the LSB clock in MHz.
        unsafe { core::mem::transmute::<*mut u8, extern "C" fn(u32)>(midi_update::app_ram())(60) };
        flash_init(EFFECTIVE_WIDTH.read(), BAUD);
        // The app can leave the panel in any state, and drawing it clears the menu's first rows.
        SCREEN_READY.set(display::init(BOARD));
        heading();
        screen();
        board_restore();
        board_init(SMK);
        usb_midi_start();
    }

    fn failure(r: Outcome) -> Text {
        match r {
            Outcome::BadPackage => text!("BAD PACKAGE"),
            Outcome::WrongKey => text!("WRONG CHIP KEY"),
            Outcome::Lost => text!("INSTALLER STOPPED"),
            _ => text!("FLASH ERROR"),
        }
    }

    /// Only returns if nothing was written.
    pub fn mode() {
        let mut written = false;
        screen();
        // (With nobody there, PLAY may be stuck: nothing waits for it.)
        while !ALONE.read() && board_keys() & KEY_SELECT != 0 {
            board_wait(20, false);
        }
        // The menu's LEDs drive the SMK-37's high-speed PHY: give it back first.
        board_restore();
        board_init(SMK);
        usb_midi_start();
        let r = loop {
            let r = mu_run(UNIT_KEY.read(), work(), &mut written);
            if r == Outcome::Download {
                download_screen();
            }
            let quiet = matches!(r, Outcome::Cancelled | Outcome::Done | Outcome::Restart);
            if !quiet {
                display::text(13, RED, failure(r));
                if written {
                    display::text(14, RED, text!("RESTARTS: TRY AGAIN"));
                }
                hal_delay_us(2_000_000);
            }
            if r == Outcome::Done {
                hal_delay_us(200_000);
            }
            // Opened by itself, only a key leaves: after a failed install that wrote nothing (a closed tab, a cable, a
            // wrong file), it answers again, with what went wrong in view.
            if !ALONE.read() || written || quiet {
                break r;
            }
            screen();
            display::text(13, RED, failure(r));
        };
        usb_midi_stop();
        if written || r == Outcome::Done || r == Outcome::Restart {
            hal_reset();
        }
        clear_rows(3, 14);
        if !ALONE.read() {
            release_select();
        }
    }
}

/// The screen's first rows: what this is, and why it is showing.
fn heading() {
    display::text(0, YELLOW, TITLE);
    display::text(1, GREY, VERSION);
    display::text(2, RED, REASON.read().unwrap_or(NONE));
}

fn confirm() -> bool {
    clear_rows(8, 14);
    display::text(8, RED, text!("BOOT ANYWAY"));
    display::text(9, WHITE, text!("APP MAY CRASH"));
    display::text(10, WHITE, text!("OR MISBEHAVE"));
    display::text(11, WHITE, text!("CRC AT NEXT BOOT"));
    release_select();
    display::text(13, GREY, text!("OTHER KEYS: CANCEL"));
    display::text(14, YELLOW, HOLD);
    // Clear both margins of the text row before its 12-pixel progress bar.
    display::rect(0, 208, 240, 16, 0);
    let mut held = 0;
    loop {
        let keys = board_keys();
        if keys & !KEY_SELECT != 0 {
            return false;
        }
        held = if keys == KEY_SELECT { held + 20 } else { 0 };
        display::rect(0, 210, 240, 12, GREY);
        display::rect(0, 210, 240 * held.min(3000) / 3000, 12, GREEN);
        if held >= 3000 {
            display::text(14, GREEN, RELEASE);
            release_select();
            return true;
        }
        board_wait(20, true);
    }
}

/// A test build's one-shot request, so automated tests can reach menu items with nobody at the keys (docs/SPEC.md
/// §11.9). It's in RAM, not flash, so a test that dies can't leave the unit stuck on it.
#[cfg(test_requests)]
mod asked {
    use crate::Global;

    pub const SD: u8 = 4;
    pub const MIDI: u8 = 5;
    pub static REQUEST: Global<u8> = Global::new(0);
    pub static SD_RUN: Global<bool> = Global::new(false);

    pub fn consume(retained: &mut [u8; 8]) -> u8 {
        if retained[..6] != *b"MBTEST" {
            return 0;
        }
        let asked = retained[6];
        *retained = [0; 8];
        if asked == SD || asked == MIDI { asked } else { 0 }
    }
}

// The menu's actions.
const BOOT_APP: u8 = 0;
const DOWNLOAD: u8 = 1;
const BOOT_ANYWAY: u8 = 2;
const SD_CARD: u8 = 4;
#[cfg(any(not(boot_sim), midi_mode))]
const MIDI_MODE: u8 = 5;

/// Whether to start the app without checking it.
#[inline(never)]
fn menu() -> bool {
    board_init(SMK);
    if !screen() {
        rom_download();
    }
    display::text(13, YELLOW, text!("CHECKING"));
    if MOUNTED.read() {
        formats::app_check(&nor().flash, app(), work());
    }
    let mut updates = 0;
    formats::request_scan(&nor().flash, header().alignment, work().first_chunk_mut().unwrap(), Some(&mut updates));
    let card = sd_probe(60);
    let (mut selection, mut last, mut previous, mut stable) = (0, 0, 0, 0);
    let mut first_draw = true;
    loop {
        let state = app().state;
        let mut actions = [(NONE, 0); 5];
        let mut count = 0;
        let mut add = |name, action| {
            actions[count] = (name, action);
            count += 1;
        };
        if card {
            add(text!("SD CARD"), SD_CARD);
        }
        if state == AppState::Ok {
            add(text!("BOOT APP"), BOOT_APP);
        }
        add(text!("DOWNLOAD MODE"), DOWNLOAD);
        #[cfg(any(not(boot_sim), midi_mode))]
        add(text!("MIDI MODE"), MIDI_MODE);
        if EFFECTIVE_WIDTH.read() == WIDTH && app().base != 0 && matches!(state, AppState::BadCrc | AppState::Incomplete) {
            add(text!("BOOT ANYWAY"), BOOT_ANYWAY);
        }
        if selection >= count {
            selection = 0;
        }
        heading();
        display::text(3, if state == AppState::Ok { GREEN } else { RED }, STATES[state as usize]);
        let mut line = Line::new(NONE);
        if app().app_length != 0 {
            line.number(app().app_length, 10, 0);
            line.append(text!(" B"));
        }
        line.show(4, WHITE);
        let ram = formats::request_valid(retained(8));
        line = Line::new(if ram { text!("UPDATE RAM ") } else { text!("UPDATE ") });
        if updates != 0 {
            line.number(updates, 10, 0);
            line.append(text!(" FLASH"));
        } else if !ram {
            line.append(text!("NONE"));
        }
        line.show(5, if updates != 0 || ram { YELLOW } else { WHITE });
        line = Line::new(text!("FLASH "));
        line.number(JEDEC.read(), 16, 6);
        line.push(b' ');
        line.number(nor().flash.capacity / 1024, 10, 0);
        line.push(b'K');
        line.show(6, WHITE);
        display::text(7, GREY, text!("--------------------"));
        for (i, &(name, _)) in actions.iter().enumerate() {
            line = Line::new(NONE);
            if i < count {
                line.append(if i == selection { text!("> ") } else { text!("  ") });
                line.append(name);
            }
            line.show(8 + i as u32, if i == selection { YELLOW } else { WHITE });
        }
        display::text(13, YELLOW, if EFFECTIVE_WIDTH.read() < WIDTH { text!("SPI WIDTH FALLBACK") } else { NONE });
        display::text(14, GREY, CONTROLS);
        if first_draw {
            first_draw = false;
            #[cfg(any(not(boot_sim), midi_mode))]
            if midi::AUTO.read() {
                // (once a start: back in the menu, it stays the menu)
                midi::AUTO.set(false);
                midi::ALONE.set(true);
                midi::STUCK.set(board_keys());
                midi::mode();
                midi::ALONE.set(false);
                // Every key down now is taken as held, not pressed: the one that left, or a stuck one. Nothing waits
                // for them to come up, so a key that stays down can't keep the menu away.
                (last, previous, stable) = (!0, 0, 0);
                continue;
            }
            // Show status before waiting; entry-held PLAY is not a selection.
            release_select();
        }
        // Redraw only on an action; polling keeps the multiplexed LEDs alive.
        let mut edge = 0;
        // With no card there's no such item. Nobody is there to leave the menu, so download mode:
        #[cfg(test_requests)]
        if asked::REQUEST.read() != 0 {
            for (i, &(_, action)) in actions[..count].iter().enumerate() {
                if action == asked::REQUEST.read() {
                    selection = i;
                    edge = KEY_SELECT;
                }
            }
            if edge == 0 {
                rom_download();
            }
            asked::SD_RUN.set(asked::REQUEST.read() == asked::SD);
            asked::REQUEST.set(0);
        }
        while edge == 0 {
            board_wait(20, true);
            let keys = board_keys();
            if keys == previous {
                stable += 1;
            } else {
                previous = keys;
                stable = 1;
            }
            if stable >= 2 {
                edge = keys & !last;
                last = keys;
            }
        }
        if edge & KEY_PREV != 0 {
            selection = (selection + count - 1) % count;
            continue;
        }
        if edge & KEY_NEXT != 0 {
            selection = (selection + 1) % count;
            continue;
        }
        if edge & KEY_SELECT == 0 {
            continue;
        }
        let action = actions[selection].1;
        if action == DOWNLOAD {
            download_screen();
        }
        #[cfg(any(not(boot_sim), midi_mode))]
        if action == MIDI_MODE {
            midi::mode();
            (last, previous, stable) = (0, 0, 0);
            first_draw = true;
            continue;
        }
        if action == SD_CARD {
            // The menu's LEDs drive the SMK-37's high-speed PHY, and stage 2 may want USB: give it back first.
            if SMK {
                board_restore();
            }
            // Stage 2 (/MBOOT/MENU.BIN) does what it likes, and resets or returns here.
            REASON.set(sd_stage2(60));
            // And after this run, for the same reason.
            #[cfg(test_requests)]
            if asked::SD_RUN.read() {
                rom_download();
            }
            // stage 2 reprograms the flash interface
            flash_init(EFFECTIVE_WIDTH.read(), BAUD);
            if SMK {
                board_init(SMK);
            }
            // The image can leave the panel in any state, and setting it up again clears it.
            SCREEN_READY.set(display::init(BOARD));
        }
        if action == SD_CARD || (action == BOOT_ANYWAY && !confirm()) {
            release_select();
            (last, previous, stable) = (0, 0, 0);
            continue;
        }
        if XIP_ATTEMPTED.read() {
            if action == BOOT_ANYWAY {
                *retained(0) = *b"BOOTANY\0";
            }
            hal_reset();
        }
        board_restore();
        return action == BOOT_ANYWAY;
    }
}

/// The address a pointer is given as in what the updater is handed.
fn pointer<T>(p: &T) -> u32 {
    p as *const T as usize as u32
}

#[inline(never)]
fn update(ram: bool) {
    let mut updater = Updater { load: 0, size: 0, stream_end: 0, crc: 0 };
    // Safety: the updater's context, which nothing else here uses.
    let (clock, memory, meta) = unsafe { (&mut CLOCK_META.get().0, &mut MEMORY_META.get().0, &mut UPDATE_META.get().0) };
    contract::config(clock, memory, &CONFIG);
    contract::updater(meta, app(), UNIT_KEY.read(), pointer(clock), if ram { 0x01c7_fdb0 } else { pointer(memory) });
    *retained::<2>(2) = [0; 2];
    // Safety: where a staged updater is loaded and runs (§6.3), which is free until then.
    let load = unsafe { core::slice::from_raw_parts_mut(formats::UPDATE_LOAD as usize as *mut u8, formats::UPDATE_MAX as usize) };
    if formats::updater_decode(&nor().flash, request(), load, work(), &mut updater) {
        #[cfg(boot_sim)]
        {
            unsafe extern "C" {
                fn boot_sim_call_updater(entry: u32, context: *mut u8, end: u32);
            }
            unsafe { boot_sim_call_updater(updater.load, meta.as_mut_ptr(), updater.stream_end) };
        }
        // Safety: the updater that was just decoded and checked there.
        #[cfg(not(boot_sim))]
        unsafe {
            core::mem::transmute::<usize, extern "C" fn(*mut u8, u32)>(updater.load as usize)(meta.as_mut_ptr(), updater.stream_end)
        };
        REASON.set(Some(text!("UPDATE RETURNED")));
    } else {
        REASON.set(Some(text!("UPDATER REFUSED")));
    }
    if ram {
        *retained::<2>(8) = [0; 2];
    }
    sync_cpu();
    // §6.2: the updater may have changed both the SPI path and the contents.
    hal_fault_clear();
    let flash_ok = flash_init(WIDTH, BAUD);
    EFFECTIVE_WIDTH.set(flash_read_width());
    nor().flash.capacity = flash_capacity();
    *header() = Header::EMPTY;
    // (Mounting clears what it knew of the app first.)
    MOUNTED.set(false);
    if flash_ok && nor().flash.capacity == 0x10_0000 {
        MOUNTED.set(formats::app_mount(&nor().flash, UNIT_KEY.read(), app(), header()));
    } else {
        *app() = App::EMPTY;
    }
    if MOUNTED.read() {
        formats::app_check(&nor().flash, app(), work());
    }
    if screen() {
        clear_rows(2, 14);
        display::text(3, RED, REASON.read().unwrap_or(NONE));
        hal_delay_us(1_000_000);
    }
}

/// Every file's checksum, as the cache serves it.
fn xip_crc() -> bool {
    let app = app();
    app.files[..app.file_count as usize].iter().all(|r| {
        // Safety: mounting checked that the file is inside the flash, which is mapped from here.
        let file = unsafe { core::slice::from_raw_parts((0x0200_0000 + r.address as usize) as *const u8, r.length as usize) };
        crc16(0, file) == r.data_crc
    })
}

/// Stage 1's entry, from stage 0, with the ROM's boot-device descriptor.
#[unsafe(no_mangle)]
#[unsafe(link_section = ".entry")]
pub extern "C" fn boot_main(_device: *const u32) {
    let mut bypass = contract::consume_bypass(retained(0));
    #[cfg(test_requests)]
    let asked = asked::consume(retained(0));
    #[cfg(not(boot_sim))]
    {
        unsafe extern "C" {
            static mut __bss_start: u8;
            static __bss_end: u8;
        }
        // Safety: stage 1's own data, which nothing has used yet.
        unsafe { core::ptr::write_bytes(&raw mut __bss_start, 0, (&raw const __bss_end).offset_from_unsigned(&raw const __bss_start)) };
    }
    #[cfg(test_requests)]
    asked::REQUEST.set(asked);
    let want_menu = board_hold(KEY_SELECT, 1000);
    #[cfg(test_requests)]
    let want_menu = want_menu || asked != 0;
    #[cfg(any(not(boot_sim), midi_mode))]
    midi::AUTO.set(!want_menu);
    board_restore();
    let mut flash_ok = flash_init(1, 1);
    identify();
    flash_ok = flash_ok && nor().flash.capacity == 0x10_0000 && flash_init(WIDTH, BAUD);
    EFFECTIVE_WIDTH.set(flash_read_width());
    MOUNTED.set(flash_ok && formats::app_mount(&nor().flash, UNIT_KEY.read(), app(), header()));
    let guarded = formats::guard(nor(), SMK, work());
    if !flash_ok {
        REASON.set(Some(text!("FLASH ERROR")));
    } else if guarded == Guard::Error {
        REASON.set(Some(text!("TRIM ERROR")));
    }
    let mut need_menu = want_menu || !flash_ok || guarded == Guard::Error;
    let mut updates_done = false;
    loop {
        if core::mem::take(&mut need_menu) {
            bypass = menu() || bypass;
        }
        if !updates_done {
            updates_done = true;
            let choice = contract::update_choice(retained(8), MOUNTED.read());
            let mut found = choice == 1;
            if found {
                *request() = *retained(8);
            } else if choice == 2 {
                found = formats::request_scan(&nor().flash, header().alignment, request(), None);
            }
            if found {
                update(choice == 1);
                if app().state != AppState::Ok {
                    need_menu = true;
                    continue;
                }
            } else if MOUNTED.read() && formats::uart_request(retained(0)) {
                *retained::<4>(0) = [0; 4];
                hal_reset();
            }
        }
        if !MOUNTED.read() {
            need_menu = true;
            continue;
        }
        XIP_ATTEMPTED.set(true);
        let ok = hal_xip(app().base, UNIT_KEY.read(), EFFECTIVE_WIDTH.read(), BAUD);
        if ok && ((bypass && EFFECTIVE_WIDTH.read() == WIDTH) || xip_crc()) {
            // What the app is handed: §6.5.
            *retained(0xa0) = header().bytes;
            contract::app(retained(APP_CONTEXT - RETAINED), app().base, UNIT_KEY.read(), TRIM.read(), &MAC.read());
            *retained::<8>(0xd8) = [0; 8];
            const APP_FLAGS: Reg = unsafe { Reg::at(0x01c7_fe60) };
            APP_FLAGS.write(0);
            board_restore();
            display::release();
            SCREEN_READY.set(false);
            irq_off();
            sync_cpu();
            #[cfg(boot_sim)]
            {
                unsafe extern "C" {
                    fn boot_sim_call_app(entry: u32, object: *mut u8);
                }
                unsafe { boot_sim_call_app(app().entry, APP_CONTEXT as *mut u8) };
            }
            // Safety: the app that was mounted and checked, at its entry.
            #[cfg(not(boot_sim))]
            unsafe {
                core::mem::transmute::<usize, extern "C" fn(usize)>(app().entry as usize)(APP_CONTEXT)
            };
            REASON.set(Some(text!("APP EXITED")));
        } else if !ok {
            REASON.set(Some(text!("XIP ERROR")));
        }
        bypass = false;
        flash_release();
        hal_fault_clear();
        flash_init(EFFECTIVE_WIDTH.read(), BAUD);
        EFFECTIVE_WIDTH.set(flash_read_width());
        if MOUNTED.read() {
            formats::app_check(&nor().flash, app(), work());
        }
        need_menu = true;
    }
}

/// For the simulated chip's tests, which start it many times over: its data as at power-on, and what they look at.
#[cfg(boot_sim)]
pub mod test {
    use super::*;

    #[unsafe(no_mangle)]
    pub extern "C" fn boot_test_reset() {
        unsafe {
            WORK.get().0.fill(0);
            *REQUEST.get() = Aligned([0; 80]);
            *CLOCK_META.get() = Aligned([0; 52]);
            *MEMORY_META.get() = Aligned([0; 60]);
            *UPDATE_META.get() = Aligned([0; 24]);
        }
        *app() = App::EMPTY;
        *header() = Header::EMPTY;
        nor().flash.capacity = 0;
        JEDEC.set(0);
        UNIT_KEY.set(0);
        TRIM.set(0);
        MAC.set([0; 8]);
        EFFECTIVE_WIDTH.set(0);
        MOUNTED.set(false);
        XIP_ATTEMPTED.set(false);
        SCREEN_READY.set(false);
        REASON.set(None);
        #[cfg(test_requests)]
        asked::SD_RUN.set(false);
    }

    #[cfg(midi_mode)]
    #[unsafe(no_mangle)]
    pub extern "C" fn boot_test_midi_reset() {
        midi::ALONE.set(false);
        midi::STUCK.set(0);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn boot_test_clock_meta() -> *const u8 {
        unsafe { CLOCK_META.get().0.as_ptr() }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn boot_test_memory_meta() -> *const u8 {
        unsafe { MEMORY_META.get().0.as_ptr() }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn boot_test_effective_width() -> u32 {
        EFFECTIVE_WIDTH.read()
    }
}
