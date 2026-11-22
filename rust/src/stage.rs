// SPDX-License-Identifier: MIT
//! MIDI mode as a program in RAM, for units without m-bootloader: a host can back up the flash and install firmware
//! (docs/SPEC.md §11.7). M-UPGRADE's route stages it like an updater, and the installed bootloader, stock or ours,
//! runs it (`stage_ota`). From download mode, M-Flasher's run-ram starts it, which leaves the flash alone.
use crate::display::{self, GREEN, RED, WHITE, YELLOW};
use crate::hw::{Text, irq_off, retained, sync_cpu};
use crate::midi_update::{self, Host, Outcome, Unit, mu_messages};
use crate::platform::{
    self, flash_capacity, flash_erase, flash_init, flash_jedec, flash_release, flash_uid, hal_delay_us, hal_early, hal_efuse, hal_reset,
};
use crate::usb_midi::{self, usb_midi_poll, usb_midi_start, usb_midi_stop};
use crate::{Aligned, Global, text};

const BOARD: u32 = if cfg!(smk) { 2 } else { 1 };
const TITLE: Text = if cfg!(smk) { text!("SMK-37 M-FLASHER") } else { text!("FM-1 M-FLASHER") };
/// The LSB clock: from the bootloader that started us, or from hal_clock_b().
const LSB_MHZ: u32 = if cfg!(stage_ota) { 60 } else { 48 };
/// After this long with no message from a host, the unit goes back to its firmware.
const NO_HOST_MS: u32 = 300_000;
const STAGE: u8 = 2;
const NONE: Text = text!("");

static SECTOR: Global<Aligned<{ midi_update::SECTOR }>> = Global::new(Aligned([0; midi_update::SECTOR]));
static SCREEN: Global<bool> = Global::new(false);
static WAITED: Global<u32> = Global::new(0);
static SEEN: Global<u32> = Global::new(0);
/// An install stopped partway: restarting now would leave no firmware, and stock has no way back.
static STAY: Global<bool> = Global::new(false);

/// The hooks of `midi_update`: what it needs of the unit.
struct Stage;

impl Host for Stage {
    fn send(message: &[u8]) {
        usb_midi::send(message)
    }

    fn idle() {
        mu_idle()
    }

    fn cancel() -> bool {
        mu_cancel()
    }

    fn read(address: u32, out: &mut [u8]) -> bool {
        platform::read(address, out)
    }

    fn erase(address: u32) -> bool {
        flash_erase(core::ptr::null_mut(), address, midi_update::SECTOR as u32)
    }

    fn program(address: u32, data: &[u8]) -> bool {
        platform::program(address, data)
    }

    fn progress(writing: bool, percent: u32) {
        mu_progress(writing, percent)
    }

    fn unit_info() -> Unit {
        let mut uid = [0; 8];
        flash_uid(&mut uid);
        Unit { jedec: flash_jedec(), capacity: flash_capacity(), uid, kind: STAGE, version: text!(env!("STAGE_VERSION")) }
    }

    fn start_app() {
        mu_start_app()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mu_idle() {
    for _ in 0..2 {
        usb_midi_poll();
        hal_delay_us(500);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mu_cancel() -> bool {
    if mu_messages.read() != SEEN.read() {
        SEEN.set(mu_messages.read());
        WAITED.set(0);
    } else {
        WAITED.set(WAITED.read() + 1);
    }
    !STAY.read() && WAITED.read() >= NO_HOST_MS
}

fn show(row: u32, color: u16, text: Text) {
    if SCREEN.read() {
        display::text(row, color, text);
    }
}

fn clear_rows(first: u32) {
    for row in first..15 {
        show(row, WHITE, NONE);
    }
}

fn show_way_out() {
    show(8, WHITE, text!("SWITCH OFF: BACK TO"));
    show(9, WHITE, text!("THE FIRMWARE"));
}

#[unsafe(no_mangle)]
pub extern "C" fn mu_progress(writing: bool, percent: u32) {
    static SHOWN: Global<u32> = Global::new(0);
    if !SCREEN.read() {
        return;
    }
    // A pass's start draws the screen for it: the bar covers rows 9 and 10.
    if percent == 0 {
        clear_rows(3);
        show(3, YELLOW, if writing { text!("WRITING") } else { text!("CHECKING") });
        show(5, if writing { YELLOW } else { WHITE }, if writing { text!("DO NOT SWITCH OFF") } else { text!("NOTHING ERASED YET") });
        display::progress(0, GREEN);
        SHOWN.set(0);
    }
    if percent != SHOWN.read() || percent == 100 {
        display::progress(percent, GREEN);
    }
    SHOWN.set(percent);
}

fn show_waiting() {
    clear_rows(0);
    show(1, GREEN, TITLE);
    show(3, WHITE, text!("CONNECT USB AND"));
    show(4, WHITE, text!("OPEN THE WEB PAGE."));
    show_way_out();
}

#[unsafe(no_mangle)]
pub extern "C" fn mu_start_app() {
    hal_delay_us(200_000);
    usb_midi_stop();
    sync_cpu();
    // Safety: mu_run() has loaded and checked an app there, which takes the LSB clock in MHz.
    unsafe { core::mem::transmute::<*mut u8, extern "C" fn(u32)>(midi_update::app_ram())(LSB_MHZ) };
    flash_init(1, 1);
    SCREEN.set(display::init(BOARD));
    show_waiting();
    usb_midi_start();
}

/// From the entry (src/stage_start.S), as the bootloader or the ROM called it.
#[unsafe(no_mangle)]
pub extern "C" fn stage_main(_context: *mut u8, _stream_end: u32) -> ! {
    unsafe extern "C" {
        static mut __bss_start: u8;
        static __bss_end: u8;
    }
    irq_off();
    // Safety: this image's own data, which nothing has used yet.
    unsafe { core::ptr::write_bytes(&raw mut __bss_start, 0, (&raw const __bss_end).offset_from_unsigned(&raw const __bss_start)) };
    hal_early();
    // The bootloader that started us has set the 60 MHz LSB clock (§3.2).
    #[cfg(stage_ota)]
    crate::platform::hal_timer_clock(LSB_MHZ);
    #[cfg(not(stage_ota))]
    crate::platform::hal_clock_b();
    flash_init(1, 1);
    // The next start must not run this again: the unit goes back to its firmware.
    #[cfg(stage_ota)]
    if flash_capacity() != 0 {
        crate::installer::retire_flash_request(_stream_end);
        *retained::<8, 2>() = [0; 2];
        sync_cpu();
    }
    let key = (hal_efuse(1, 3) >> 8) as u16;
    SCREEN.set(display::init(BOARD));
    show_waiting();
    usb_midi_start();
    loop {
        let mut written = false;
        // Safety: the one use of this buffer.
        let r = midi_update::run::<Stage>(key, unsafe { &mut SECTOR.get().0 }, &mut written);
        if matches!(r, Outcome::Download | Outcome::Done | Outcome::Restart | Outcome::Cancelled) {
            hal_delay_us(200_000);
            usb_midi_stop();
            flash_release();
            if r == Outcome::Download {
                *retained::<0, _>() = *b"usb_update_mode\0";
                sync_cpu();
            }
            hal_reset();
        }
        let stay = STAY.read() || written;
        STAY.set(stay);
        clear_rows(3);
        show(
            3,
            RED,
            match r {
                Outcome::BadPackage => text!("BAD PACKAGE"),
                Outcome::WrongKey => text!("WRONG CHIP KEY"),
                Outcome::Lost => text!("HOST STOPPED"),
                _ => text!("FLASH ERROR"),
            },
        );
        show(5, if stay { RED } else { WHITE }, if stay { text!("INSTALL AGAIN") } else { text!("OPEN THE WEB PAGE.") });
        if !stay {
            show_way_out();
        }
    }
}
