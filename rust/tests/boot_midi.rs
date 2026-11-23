// SPDX-License-Identifier: MIT
//! The menu's MIDI mode by default (rust/src/boot.rs, AUTO): boot.rs built with `midi_mode` on the boot simulator,
//! the USB MIDI side stubbed (no installer connects: the MIDI loop only polls its cancel). Built for the FM-1 and,
//! with `smk`, for the SMK-37 (its own binary and output line).
//! Without any fixture, on a blank flash (APP NOT FOUND, the menu comes up by itself) and on a synthetic app:
//!   - the menu that comes up by itself opens in MIDI mode, the app's state and the reason in view, the menu's keys
//!     named, the app not run;
//!   - each of the four keys goes back to the whole menu: its actions as before, the key not taken as a menu step;
//!   - a key that stays down can't hang the unit: pressed in MIDI mode and held, the menu still comes and doesn't
//!     take it as a press; down before MIDI mode opens, PLAY included, it neither keeps MIDI mode from opening nor
//!     leaves it, until it has read as up for 50 scans in a row;
//!   - an install that fails with nothing written doesn't take MIDI mode opened by itself away: it answers again,
//!     the failure in view, and only a key leaves; chosen from the menu, it goes back to the menu as before; one
//!     that fails after writing restarts the unit, never answering again over a half-written app;
//!   - a flash error (and on the SMK-37 a calibration guard error) opens MIDI mode with its reason in view, also
//!     after a RAM app has run and left the screen black;
//!   - an app that returns to the bootloader, the panel left off: MIDI mode, APP EXITED, and the menu after it;
//!   - PLAY held at power-on opens the menu itself, no MIDI mode.
//!
//! With the board's flash dump (testdata/<board>-stock.bin, 1 MiB) as an extra, its app damaged (one bit in app.bin:
//! APP BAD CRC): the same, and BOOT ANYWAY still reached. A missing dump is a SKIP of the extra only; with
//! REQUIRE_FIXTURES set in the environment it fails.
//! Not here: USB and the clocks. The simulator has no USB controller, and rust/src/usb_midi.rs isn't built for it,
//! so what an exiting app leaves in them is for hardware (docs/SPEC.md §12.5).
use super::boot::{
    END_APP, END_RESET, END_TIMEOUT, KEY_NEXT, KEY_PREV, KEY_RECOVERY, KEY_SELECT, SMK, SPI1_CON,
    check, t, t_damage_app, t_fixture_cache, t_fm1_matrix_enable, t_go, t_key, t_map, t_open_menu,
    t_own_fixture, t_prep, t_row_is, t_usb_reset,
};
use super::sim::*;
use crate::Global;
use crate::board::{board_init, board_restore};
use crate::boot::midi::{ALONE, STUCK, mu_cancel, mu_idle, mu_start_app};
use crate::boot::test::boot_test_midi_reset;
use crate::midi_update::{Outcome, SECTOR};
use crate::platform::hal_early;
use crate::stage0::apply_config;
use std::fs::File;
use std::io::{ErrorKind, Read};

const MENU_KEYS: &str = if SMK {
    "< > SEQ PLAY SEQ REC"
} else {
    "OCT- OCT+ PLAY REC"
};

/// What the stand-ins for USB MIDI and its protocol count, and what a scenario has the protocol's first run do.
struct Midi {
    midi_runs: u32,
    usb_starts: u32,
    usb_stops: u32,
    ram_app_calls: u32,
    ram_app_mhz: u32,
    usb_started_at: u64,
    /// The stub's first run: a failed install (written if `mu_written`) or a RAM app run (cmd 0x25); then it waits.
    mu_first: Outcome,
    mu_ram_app: bool,
    mu_written: bool,
}

static M: Global<Midi> = Global::new(Midi {
    midi_runs: 0,
    usb_starts: 0,
    usb_stops: 0,
    ram_app_calls: 0,
    ram_app_mhz: 0,
    usb_started_at: 0,
    mu_first: Outcome::Cancelled,
    mu_ram_app: false,
    mu_written: false,
});

// Safety: as for boot.rs's `t`.
fn m() -> &'static mut Midi {
    unsafe { M.get() }
}

pub fn usb_midi_start() {
    m().usb_starts += 1;
    m().usb_started_at = sim().now;
}

pub fn usb_midi_stop() {
    m().usb_stops += 1;
}

pub fn usb_midi_poll() {}

pub fn usb_midi_send(_message: &[u8]) {}

pub fn mu_run(_key: u16, _sector: &mut [u8; SECTOR], written: &mut bool) -> Outcome {
    *written = false;
    m().midi_runs += 1;
    if m().midi_runs == 1 && m().mu_first != Outcome::Cancelled {
        *written = m().mu_written;
        return m().mu_first;
    }
    if m().midi_runs == 1 && m().mu_ram_app {
        mu_start_app();
    }
    loop {
        mu_idle();
        if mu_cancel() {
            return Outcome::Cancelled;
        }
    }
}

/// A RAM app that takes the panel down and leaves the screen black.
extern "C" fn t_ram_app(mhz: u32) {
    m().ram_app_calls += 1;
    m().ram_app_mhz = mhz;
    SPI1_CON.write(0);
    sim().panel_on = false;
    sim().fb.fill(0);
}

/// The RAM app that mu_start_app() calls is t_ram_app: where `midi_update::app_ram` says an app is, here.
pub fn app_ram() -> *mut u8 {
    t_ram_app as *mut u8
}

const T_MENU_KEYS: [u32; 4] = [KEY_PREV, KEY_NEXT, KEY_SELECT, KEY_RECOVERY];
const T_FOREVER: u32 = 100_000_000;

fn t_midi_prep(fixture: usize) {
    t_prep(fixture);
    boot_test_midi_reset();
    let m = m();
    (m.midi_runs, m.usb_starts, m.usb_stops) = (0, 0, 0);
    (m.ram_app_calls, m.ram_app_mhz) = (0, 0);
    m.usb_started_at = 0;
    m.mu_first = Outcome::Cancelled;
    m.mu_written = false;
    m.mu_ram_app = false;
}

fn t_midi_shown(state: &str) -> bool {
    t_row_is(3, "MIDI MODE")
        && t_row_is(4, state)
        && t_row_is(8, "TO OPEN THE MENU:")
        && t_row_is(9, MENU_KEYS)
}

/// No fixture: a blank flash (the public malformed-image case, t_prep(4)) has no app, so the menu comes up by itself
/// with APP NOT FOUND. Runs on every checkout, for the board the binary is built for.
fn t_midi_blank() {
    // nobody at the unit: MIDI mode, why the app didn't start in view
    t_midi_prep(4);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && m().midi_runs == 1 && m().usb_starts == 1);
    check!(t_midi_shown("APP NOT FOUND") && t_row_is(2, ""));
    // NEXT leaves it: the whole menu, the selection where it starts, the key not taken as a step
    t_midi_prep(4);
    t_key(KEY_NEXT, 3000, 3300);
    sim().limit = 5_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && m().midi_runs == 1 && m().usb_stops == 1);
    check!(t_row_is(3, "APP NOT FOUND"));
    check!(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE"));
    // every key leaves it (the release wait and the key mapping differ per board)
    for key in T_MENU_KEYS {
        t_midi_prep(4);
        t_key(key, 3000, 3300);
        sim().limit = 5_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(
            m().midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE")
        );
    }
    // PLAY held at power-on: the menu itself, no MIDI mode
    t_midi_prep(4);
    t_open_menu();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(
        m().midi_runs == 0
            && t().app_calls == 0
            && t_row_is(3, "APP NOT FOUND")
            && t_row_is(8, "> DOWNLOAD MODE")
    );
}

/// A key that stays down: the menu all the same, and no press taken from it.
fn t_midi_stuck() {
    // each key pressed in MIDI mode and never let go: the menu, at its first item (PLAY would have picked it)
    for key in T_MENU_KEYS {
        t_midi_prep(4);
        t_key(key, 3000, T_FOREVER);
        sim().limit = 8_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(
            m().midi_runs == 1
                && t().resets == 0
                && t_row_is(3, "APP NOT FOUND")
                && t_row_is(8, "> DOWNLOAD MODE")
        );
    }
    // let go at last, the next press is a step again
    t_midi_prep(4);
    t_key(KEY_NEXT, 3000, 6000);
    t_key(KEY_NEXT, 6500, 6600);
    sim().limit = 8_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(8, "  DOWNLOAD MODE") && t_row_is(9, "> MIDI MODE"));
    // down before MIDI mode opens: it stays in MIDI mode, and another key still leaves it
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, T_FOREVER);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, T_FOREVER);
    t_key(KEY_PREV, 4000, 4100);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    // PLAY stuck once the power-on check has read it up: nothing waits for it, MIDI mode opens
    const FROM: [u32; 2] = [50, 200];
    for from in FROM {
        t_midi_prep(4);
        t_key(KEY_SELECT, from, T_FOREVER);
        sim().limit = 6_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(m().midi_runs == 1 && m().usb_starts == 1 && t_midi_shown("APP NOT FOUND"));
        // and another key leaves it, PLAY not taken as a press in the menu
        t_midi_prep(4);
        t_key(KEY_SELECT, from, T_FOREVER);
        t_key(KEY_NEXT, 4000, 4100);
        sim().limit = 6_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(
            m().midi_runs == 1
                && t().resets == 0
                && t_row_is(3, "APP NOT FOUND")
                && t_row_is(8, "> DOWNLOAD MODE")
        );
    }
    // down from 5 ms, the power-on check sees PLAY held: that asks for the menu itself, no MIDI mode, as before
    t_midi_prep(4);
    t_key(KEY_SELECT, 5, T_FOREVER);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 0 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
}

/// A key down when MIDI mode opens counts again once it has read as up for 50 scans in a row.
fn t_midi_released() {
    // MIDI mode opens after keys that go down at 100 ms, past the power-on checks
    t_midi_prep(4);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT && m().usb_started_at > 150_000);
    // NEXT down at power-on, let go, pressed again: it leaves
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, 1500);
    t_key(KEY_NEXT, 4000, 4300);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    // all four down when it opens, let go, then each one leaves
    for key in T_MENU_KEYS {
        t_midi_prep(4);
        t_key(KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY, 100, 3500);
        t_key(key, 4500, 4800);
        sim().limit = 7_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(
            m().midi_runs == 1
                && t().resets == 0
                && t_row_is(3, "APP NOT FOUND")
                && t_row_is(8, "> DOWNLOAD MODE")
        );
    }
    // still down: none of them leaves
    t_midi_prep(4);
    t_key(
        KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY,
        100,
        T_FOREVER,
    );
    sim().limit = 7_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
    // a stuck key that reads up for 10 ms now and then doesn't make MIDI mode leave by itself
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, 3000);
    t_key(KEY_NEXT, 3010, 4000);
    t_key(KEY_NEXT, 4010, T_FOREVER);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
}

/// Count scans directly, so the boundary does not depend on how long a scan takes on either board.
fn t_midi_release_boundary() {
    t_midi_prep(4);
    hal_early();
    check!(apply_config());
    board_init(SMK);
    ALONE.set(true);
    for released_scans in [49, 50] {
        STUCK.set(KEY_NEXT);
        sim().keyfn = Some(|_| KEY_NEXT);
        check!(!mu_cancel());
        sim().keyfn = None;
        for _ in 0..released_scans {
            assert!(!mu_cancel());
        }
        sim().keyfn = Some(|_| KEY_NEXT);
        for _ in 0..19 {
            assert!(!mu_cancel());
        }
        check!(mu_cancel() == (released_scans == 50));
    }
    ALONE.set(false);
    board_restore();
    sim_flush();
    check!(sim().err == 0);
}

/// An install that fails with nothing written: MIDI mode opened by itself stays, the failure in view; only a key
/// leaves. Chosen from the menu, it goes back to the menu as before.
fn t_midi_failures() {
    const FAILS: [Outcome; 3] = [Outcome::Lost, Outcome::BadPackage, Outcome::WrongKey];
    const TEXTS: [&str; 3] = ["INSTALLER STOPPED", "BAD PACKAGE", "WRONG CHIP KEY"];
    for i in 0..FAILS.len() {
        t_midi_prep(4);
        m().mu_first = FAILS[i];
        sim().limit = 30_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(m().midi_runs == 2 && m().usb_starts == 1 && m().usb_stops == 0 && t().resets == 0);
        check!(t_midi_shown("APP NOT FOUND") && t_row_is(13, TEXTS[i]));
        t_midi_prep(4);
        m().mu_first = FAILS[i];
        t_key(KEY_NEXT, 6000, 6100);
        sim().limit = 8_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(
            m().midi_runs == 2
                && m().usb_stops == 1
                && t_row_is(3, "APP NOT FOUND")
                && t_row_is(8, "> DOWNLOAD MODE")
        );
    }
    // chosen from the menu: PLAY at power-on, NEXT, PLAY; it fails, and the menu is back
    t_midi_prep(4);
    m().mu_first = Outcome::Lost;
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(
        m().midi_runs == 1
            && m().usb_stops == 1
            && t_row_is(3, "APP NOT FOUND")
            && t_row_is(9, "> MIDI MODE")
    );
    // opened by itself, it fails after writing: never again over a half-written app, it restarts
    for i in 0..FAILS.len() {
        t_midi_prep(4);
        m().mu_first = FAILS[i];
        m().mu_written = true;
        sim().limit = 8_000_000;
        check!(t_go() == END_RESET);
        check!(t().resets == 1 && m().midi_runs == 1 && m().usb_stops == 1 && !t_usb_reset());
        check!(t_row_is(13, TEXTS[i]) && t_row_is(14, "RESTARTS: TRY AGAIN"));
    }
}

/// The other ways the menu comes up by itself: their reason stays in view in MIDI mode.
fn t_midi_reasons() {
    // the flash isn't 1 MiB: FLASH ERROR
    t_midi_prep(4);
    sim().jedec = (sim().jedec & !0xff) | 0x13;
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t_row_is(2, "FLASH ERROR") && t_midi_shown("APP NOT FOUND"));
    if SMK {
        // calibration to back up, and the flash refuses the write: TRIM ERROR
        t_midi_prep(4);
        sim_nor()[0xc1000 + 0x224..][..8].copy_from_slice(b"trimdat\0");
        *nor_budget() = 0;
        sim().limit = 3_000_000;
        check!(t_go() == END_TIMEOUT);
        check!(m().midi_runs == 1 && t_row_is(2, "TRIM ERROR") && t_midi_shown("APP NOT FOUND"));
    }
    // a RAM app runs (cmd 0x25) and returns, the screen left black: the reason is drawn again with the rest
    t_midi_prep(4);
    sim().jedec = (sim().jedec & !0xff) | 0x13;
    m().mu_ram_app = true;
    sim().limit = 4_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(
        m().ram_app_calls == 1
            && m().ram_app_mhz == 60
            && m().usb_starts == 2
            && m().usb_stops == 1
    );
    check!(t_row_is(2, "FLASH ERROR") && t_midi_shown("APP NOT FOUND"));
}

/// A synthetic app on a blank flash (the layout of rust/tests/formats.rs's synthetic banks), for the chip key 980F the
/// simulator's efuse gives: the header, the application directory pointing at a bank at 0x10000, and the bank's area
/// head and app.bin, its 32 zero bytes the entry. Stored bytes, worked out apart from rust/src/formats.rs from §5.1
/// and §5.2 (CRC16/XMODEM and ENC, checked against §5.1's vectors), so a wrong CRC or cipher there can't pass here.
const T_APP_LINES: [(usize, [u8; 32]); 5] = [
    // 0x00000, flash header: size 0xFF000, alignment 16, ENC FFFF
    (
        0x00000,
        [
            0xfb, 0xa2, 0x9f, 0x1f, 0x1f, 0x3e, 0x7c, 0xf8, 0xf0, 0x31, 0xac, 0x67, 0xce, 0xad,
            0x5b, 0x97, 0x0f, 0x1e, 0x3c, 0x78, 0xd1, 0x83, 0x27, 0x4e, 0x9c, 0x19, 0x13, 0x26,
            0x4c, 0x98, 0x30, 0x60,
        ],
    ),
    // 0x00020, app_dir_head: bank 0x10000, no info record, ENC FFFF
    (
        0x00020,
        [
            0x71, 0x26, 0x9f, 0x1f, 0x1f, 0x3e, 0x7d, 0xf8, 0xf0, 0xc1, 0xa3, 0x67, 0x4f, 0xbd,
            0x5b, 0x97, 0x6e, 0x6e, 0x4c, 0x27, 0xb5, 0xea, 0x55, 0x11, 0xf4, 0x7c, 0x72, 0x42,
            0x4c, 0x98, 0x30, 0x60,
        ],
    ),
    // 0x10000, app_area_head: entry 0x02000120, extent 0x1000, ENC 980F
    (
        0x10000,
        [
            0x55, 0x04, 0x81, 0x03, 0xf9, 0xb3, 0x64, 0xca, 0xb1, 0x72, 0xc4, 0x88, 0x93, 0x01,
            0x23, 0x46, 0xcc, 0x2a, 0xe5, 0x54, 0x77, 0x5e, 0x1c, 0xb2, 0xf9, 0x05, 0x9e, 0x97,
            0x88, 0xf9, 0xd3, 0xa6,
        ],
    ),
    // 0x10020, app.bin: 32 bytes at 0x120, data CRC 0000, ENC 980F^8
    (
        0x10020,
        [
            0x8e, 0x78, 0x5e, 0xbc, 0x79, 0xb3, 0x64, 0xc8, 0x91, 0x62, 0xc4, 0x88, 0x92, 0x20,
            0x61, 0xc2, 0xc4, 0x1b, 0x87, 0xe1, 0xfc, 0x74, 0x75, 0x17, 0x0f, 0x1e, 0x1d, 0x3a,
            0x74, 0xc9, 0xb3, 0x66,
        ],
    ),
    // 0x10120, app.bin's 32 zero bytes, ENC 980F^48
    (
        0x10120,
        [
            0x47, 0xaf, 0x5e, 0xbc, 0x59, 0xb2, 0x64, 0xc8, 0xb1, 0x62, 0xe5, 0xca, 0x94, 0x28,
            0x50, 0xa0, 0x61, 0xe3, 0xc6, 0xad, 0x5a, 0xb4, 0x68, 0xf1, 0xc3, 0x86, 0x2d, 0x5a,
            0xb4, 0x68, 0xd0, 0xa0,
        ],
    ),
];

fn t_synthetic_app() {
    for (address, bytes) in T_APP_LINES {
        sim_nor()[address..][..32].copy_from_slice(&bytes);
    }
}

/// What an app may leave as it returns: the panel's bus off.
fn t_app_leaves_panel_off() {
    SPI1_CON.write(0);
    sim().panel_on = false;
}

/// The path after an app exits: MIDI mode starts by itself there too, from whatever the app left.
fn t_midi_app_exit() {
    // the synthetic app boots as any app does, and no MIDI mode
    t_midi_prep(4);
    t_synthetic_app();
    check!(t_go() == END_APP);
    check!(t().app_calls == 1 && m().midi_runs == 0 && m().usb_starts == 0);
    // it returns, the panel left off: MIDI mode, with APP EXITED above the app's state
    t_midi_prep(4);
    t_synthetic_app();
    t().app_returns = true;
    t().app_hook = Some(t_app_leaves_panel_off);
    sim().limit = 4_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 1 && m().midi_runs == 1 && m().usb_starts == 1);
    check!(t_row_is(2, "APP EXITED") && t_midi_shown("APP OK"));
    // a key: the menu, and BOOT APP from it resets to try again (§7)
    t_midi_prep(4);
    t_synthetic_app();
    t().app_returns = true;
    t().app_hook = Some(t_app_leaves_panel_off);
    t_key(KEY_NEXT, 4000, 4100);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(
        m().midi_runs == 1
            && m().usb_stops == 1
            && t_row_is(2, "APP EXITED")
            && t_row_is(3, "APP OK")
    );
    check!(t_row_is(8, "> BOOT APP") && t_row_is(9, "  DOWNLOAD MODE"));
    t_midi_prep(4);
    t_synthetic_app();
    t().app_returns = true;
    t_key(KEY_NEXT, 4000, 4100);
    t_key(KEY_SELECT, 4600, 4700);
    check!(t_go() == END_RESET);
    check!(t().app_calls == 1 && m().midi_runs == 1 && !t_usb_reset());
    // stuck in MIDI mode after it: the menu still comes
    t_midi_prep(4);
    t_synthetic_app();
    t().app_returns = true;
    t_key(KEY_SELECT, 4000, T_FOREVER);
    sim().limit = 9_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 1 && t().resets == 0 && t_row_is(8, "> BOOT APP"));
    // PLAY stuck from 200 ms, through the app and its return: MIDI mode all the same
    t_midi_prep(4);
    t_synthetic_app();
    t().app_returns = true;
    t_key(KEY_SELECT, 200, T_FOREVER);
    sim().limit = 6_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(
        t().app_calls == 1
            && m().midi_runs == 1
            && t_row_is(2, "APP EXITED")
            && t_midi_shown("APP OK")
    );
}

/// With the board's flash dump (a real image, its app damaged by one bit: APP BAD CRC).
fn t_midi_by_default() {
    let f = t_own_fixture();
    // the app damaged, nobody at the unit: MIDI mode
    t_midi_prep(f);
    t_damage_app();
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && m().midi_runs == 1);
    check!(t_midi_shown("APP BAD CRC"));
    // NEXT leaves it: the whole menu, the selection where it starts
    t_midi_prep(f);
    t_damage_app();
    t_key(KEY_NEXT, 3000, 3300);
    sim().limit = 5_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && m().midi_runs == 1);
    check!(t_row_is(3, "APP BAD CRC"));
    check!(
        t_row_is(8, "> DOWNLOAD MODE")
            && t_row_is(9, "  MIDI MODE")
            && t_row_is(10, "  BOOT ANYWAY")
            && t_row_is(11, "")
    );
    // then BOOT ANYWAY, as before: two steps down, PLAY, PLAY held for the confirmation
    t_midi_prep(f);
    t_damage_app();
    t_key(KEY_NEXT, 3000, 3300);
    t_key(KEY_NEXT, 4000, 4100);
    t_key(KEY_NEXT, 4400, 4500);
    t_key(KEY_SELECT, 5000, 5100);
    t_key(KEY_SELECT, 5600, 9100);
    check!(t_go() == END_RESET);
    check!(m().midi_runs == 1 && t().reset_retained[..8] == *b"BOOTANY\0");
    // PLAY held at power-on: the menu itself, no MIDI mode
    t_midi_prep(f);
    t_open_menu();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(m().midi_runs == 0 && t().app_calls == 0 && t_row_is(8, "> BOOT APP"));
}

#[test]
fn boot_midi() {
    t_map();
    t_fm1_matrix_enable();
    println!(
        "boot simulation: the menu's MIDI mode by default ({})",
        if SMK { "SMK-37" } else { "FM-1" }
    );
    t_midi_blank();
    t_midi_stuck();
    t_midi_released();
    t_midi_release_boundary();
    t_midi_failures();
    t_midi_reasons();
    t_midi_app_exit();
    // the flash dump of the board is an extra: testdata/<board>-stock.bin
    let path = if SMK {
        "testdata/smk37-stock.bin"
    } else {
        "testdata/fm1-stock.bin"
    };
    let f = t_own_fixture();
    match File::open(path) {
        Ok(mut input) => {
            assert!(input.read_exact(&mut t_fixture_cache()[f]).is_ok());
            // Exactly the flash, as boot.rs reads its dumps: a longer file is not one.
            assert!(matches!(input.read(&mut [0]), Ok(0)));
            drop(input);
            t().fixture_present[f] = true;
            t_midi_by_default();
        }
        Err(e)
            if e.kind() == ErrorKind::NotFound
                && std::env::var_os("REQUIRE_FIXTURES").is_none() =>
        {
            println!("SKIP: {path} not present (the checks on a real dump)");
        }
        Err(e) => {
            println!(
                "FAIL: cannot read {path} (errno {})",
                e.raw_os_error().unwrap_or(0)
            );
            t().failures += 1;
        }
    }
    println!("{} checks, {} failed", t().checks, t().failures);
    // The C program's exit code.
    assert!(t().failures == 0);
}
