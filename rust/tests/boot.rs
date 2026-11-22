// SPDX-License-Identifier: MIT
//! The boots of a board on the simulated chip: the whole start-up and menu (rust/src/boot.rs, with stage 0 in front
//! of it), which has an entry for these tests to reset its private data between simulated power cycles.
//! The register/NOR model and scenario approach are adapted from the other independent implementation (a), under
//! MIT; no original implementation is used.
//!
//! Built for the FM-1, for the SMK-37 (`smk`) and for a test build's requests (`test_requests`). The MIDI mode
//! test (boot_midi.rs, `midi_mode`) uses what is here and runs its own scenarios, not these.
use super::escape;
use super::sim::*;
use crate::Global;
use crate::board::{board_init, board_keys, board_restore, board_wait};
#[cfg(not(midi_mode))]
use crate::boot::test::boot_test_effective_width;
use crate::boot::test::{boot_test_clock_meta, boot_test_memory_meta, boot_test_reset};
#[cfg(not(midi_mode))]
use crate::formats::{crc16, le16, le32, put_le16, put_le32, request_valid};
use crate::formats::{enc, range};
use crate::hw::{Reg, Text, bit};
use crate::platform::{gpio_output, hal_early};
use crate::stage0::{apply_config, stage0_main};
#[cfg(not(midi_mode))]
use crate::text;
use core::ffi::{c_int, c_void};
#[cfg(not(midi_mode))]
use std::fs::File;
#[cfg(not(midi_mode))]
use std::io::{ErrorKind, Read};

pub(super) const SMK: bool = cfg!(smk);
#[cfg(not(midi_mode))]
const GREEN: u16 = 0x07e0;
#[cfg(not(midi_mode))]
const GREY: u16 = 0x8410;
/// The screen's first row.
#[cfg(not(midi_mode))]
const TITLE: &str = if SMK {
    "SMK-37 M-BOOTLOADER"
} else {
    "FM-1 M-BOOTLOADER"
};
/// The retained area (docs/SPEC.md §6.1).
const RETAINED: usize = 0x01c7_fd80;
// The test's own copies, so that a wrong value in the crate fails here and is not followed.
// The keys as `board_keys()` gives them, which the simulated keyboard's (`SK_*`) are the numbers of too.
pub(super) const KEY_PREV: u32 = 1;
pub(super) const KEY_NEXT: u32 = 2;
pub(super) const KEY_SELECT: u32 = 4;
pub(super) const KEY_RECOVERY: u32 = 8;
/// Where a staged updater is loaded (docs/SPEC.md §2.1), and the size of M-VAVE's, which is the most one may be.
const UPDATE_LOAD: u32 = 0x01c0_a800;
const UPDATE_MAX: u32 = 0x5b1c;

/// A check, which is counted, and told of and counted again when it fails: the run goes on, and its last line has
/// both counts.
macro_rules! check {
    ($c:expr) => {
        $crate::tests::boot::t_check($c, file!(), line!(), stringify!($c))
    };
}
#[cfg(midi_mode)]
pub(super) use check;

pub(super) fn t_check(passed: bool, file: &str, line: u32, text: &str) {
    t().checks += 1;
    if !passed {
        eprintln!("{file}:{line}: {text}");
        t().failures += 1;
    }
}

// The ways out of a boot.
pub(super) const END_APP: i32 = 1;
pub(super) const END_UPDATE: i32 = 2;
pub(super) const END_RESET: i32 = 3;
pub(super) const END_TIMEOUT: i32 = 4;

/// Keys held from one time to another, in microseconds.
#[derive(Clone, Copy)]
struct Key {
    mask: u32,
    from: u64,
    to: u64,
}

/// What the scenarios set up, and what the stand-ins below record for them. (The MIDI mode test looks at little of
/// what they record.)
#[cfg_attr(midi_mode, allow(dead_code))]
pub(super) struct Test {
    pub(super) checks: u32,
    pub(super) failures: u32,
    cases: u32,
    pub(super) app_calls: u32,
    update_calls: u32,
    pub(super) resets: u32,
    sd_calls: u32,
    sd_card: bool,
    sd_leaves_spi_off: bool,
    sd_result: Option<Text>,
    pub(super) app_returns: bool,
    update_returns: bool,
    bad_quad_on_return: bool,
    update_hook: Option<fn()>,
    /// What a returning app leaves behind, run as it returns.
    pub(super) app_hook: Option<fn()>,
    app_object: [u8; 24],
    app_header: [u8; 32],
    pub(super) reset_retained: [u8; 256],
    update_object: [u8; 24],
    update_clock: [u8; 52],
    // (No check looks at it.)
    #[allow(dead_code)]
    update_memory: [u8; 60],
    app_entry: u32,
    update_entry: u32,
    update_end: u32,
    sfc: [u32; 5],
    pin_snapshot: [[u32; 8]; 2],
    phy_snapshot: u32,
    init_at_update: u32,
    init_at_app: u32,
    request_crc_at_update: [u8; 2],
    keys: [Key; 64],
    nkeys: usize,
    pub(super) fixture_present: [bool; 4],
}

static T: Global<Test> = Global::new(Test {
    checks: 0,
    failures: 0,
    cases: 0,
    app_calls: 0,
    update_calls: 0,
    resets: 0,
    sd_calls: 0,
    sd_card: false,
    sd_leaves_spi_off: false,
    sd_result: None,
    app_returns: false,
    update_returns: false,
    bad_quad_on_return: false,
    update_hook: None,
    app_hook: None,
    app_object: [0; 24],
    app_header: [0; 32],
    reset_retained: [0; 256],
    update_object: [0; 24],
    update_clock: [0; 52],
    update_memory: [0; 60],
    app_entry: 0,
    update_entry: 0,
    update_end: 0,
    sfc: [0; 5],
    pin_snapshot: [[0; 8]; 2],
    phy_snapshot: 0,
    init_at_update: 0,
    init_at_app: 0,
    request_crc_at_update: [0; 2],
    keys: [Key {
        mask: 0,
        from: 0,
        to: 0,
    }; 64],
    nkeys: 0,
    fixture_present: [false; 4],
});
static T_ROM_PARAM: Global<[u8; 8]> = Global::new([0; 8]);
static T_DEVICE: Global<[u32; 4]> = Global::new([0; 4]);
static T_FIXTURE_CACHE: Global<[[u8; 0x10_0000]; 4]> = Global::new([[0; 0x10_0000]; 4]);
#[cfg(not(midi_mode))]
const T_FIXTURE_NAMES: [&str; 4] = [
    "fm1-stock",
    "smk37-stock",
    "smk37-snoop37",
    "smk37-baudgirl",
];
const T_OFFSETS: [u32; 8] = [0, 8, 12, 16, 20, 24, 28, 32];

// Safety, for these two: a boot runs on a thread of its own while the test waits for it, so one thread at a time
// is here; and what these give is not kept across a call that can reach the other's use of it (the chip calls the
// keys' function, the bootloader the stand-ins below).
pub(super) fn t() -> &'static mut Test {
    unsafe { T.get() }
}

pub(super) fn t_fixture_cache() -> &'static mut [[u8; 0x10_0000]; 4] {
    unsafe { T_FIXTURE_CACHE.get() }
}

/// Memory of the chip, which `t_map` has put where the chip has it: `n` bytes at `address`.
fn mem(address: usize, n: usize) -> &'static mut [u8] {
    // Safety: mapped for as long as the program runs, and used by one thread at a time (see `t`); what this gives
    // is done with before the bootloader goes on.
    unsafe { core::slice::from_raw_parts_mut(core::ptr::with_exposed_provenance_mut(address), n) }
}

/// `N` bytes of the retained area from `off`.
fn ret<const N: usize>(off: usize) -> &'static mut [u8; N] {
    mem(RETAINED + off, N).try_into().unwrap()
}

/// The panel's SPI.
pub(super) const SPI1_CON: Reg = Reg::at::<0x11d00>();

// The card is on the panel's bus, which the simulated chip doesn't model; these stand in for rust/src/sdboot.rs.
pub fn sd_probe(_lsb_mhz: u32) -> bool {
    t().sd_card
}

pub fn sd_stage2(_lsb_mhz: u32) -> Option<Text> {
    t().sd_calls += 1;
    if t().sd_leaves_spi_off {
        SPI1_CON.write(0);
        sim().panel_on = false;
    }
    t().sd_result
}

/// The application's start: what it would find is recorded, and it ends the boot, or returns.
///
/// # Safety
/// `object` is what the app is handed: 24 bytes.
pub unsafe fn boot_sim_call_app(entry: u32, object: *mut u8) {
    sim_flush();
    t().app_calls += 1;
    t().app_entry = entry;
    t().init_at_app = sim().init_count;
    t().app_object = unsafe { object.cast::<[u8; 24]>().read() };
    t().app_header = *ret(160);
    t().sfc[0] = sim_rd(0x40200);
    t().sfc[1] = sim_rd(0x40204);
    t().sfc[2] = sim_rd(0x4020c);
    t().sfc[3] = sim_rd(0x40300);
    t().sfc[4] = sim_rd(0x40304);
    for p in 0..2 {
        for r in 0..8 {
            t().pin_snapshot[p][r] =
                sim_reg_get(0x50000 + (if p != 0 { 7 } else { 0 }) * 0x40 + T_OFFSETS[r]);
        }
    }
    t().phy_snapshot = sim_reg_get(0x16a0c);
    if !t().app_returns {
        escape::leave(END_APP);
    }
    if t().bad_quad_on_return {
        sim().bad_quad = true;
    }
    if let Some(hook) = t().app_hook {
        hook();
    }
}

/// A staged updater's start.
///
/// # Safety
/// `object` is what the updater is handed: 24 bytes.
pub unsafe fn boot_sim_call_updater(entry: u32, object: *mut u8, end: u32) {
    sim_flush();
    t().update_calls += 1;
    t().update_entry = entry;
    t().update_end = end;
    t().init_at_update = sim().init_count;
    t().update_object = unsafe { object.cast::<[u8; 24]>().read() };
    // Safety: the bootloader's own, of these sizes, which it is not using while it calls this.
    unsafe {
        t().update_clock = boot_test_clock_meta().cast::<[u8; 52]>().read();
        t().update_memory = boot_test_memory_meta().cast::<[u8; 60]>().read();
    }
    t().request_crc_at_update = *ret(2);
    if let Some(hook) = t().update_hook {
        hook();
    }
    if !t().update_returns {
        escape::leave(END_UPDATE);
    }
}

pub fn boot_sim_reset() -> ! {
    sim_flush();
    t().resets += 1;
    t().reset_retained = *ret(0);
    escape::leave(END_RESET)
}

fn t_timeout() {
    escape::leave(END_TIMEOUT)
}

fn t_keyfn(now: u64) -> u32 {
    let t = t();
    let mut result = 0;
    for key in &t.keys[..t.nkeys] {
        if now >= key.from && now < key.to {
            result |= key.mask;
        }
    }
    result
}

/// Keys held from one time to another, in milliseconds.
pub(super) fn t_key(mask: u32, from: u32, to: u32) {
    let t = t();
    assert!(t.nkeys < 64);
    t.keys[t.nkeys] = Key {
        mask,
        from: from as u64 * 1000,
        to: to as u64 * 1000,
    };
    t.nkeys += 1;
}

/// The read function of the ROM's boot device, as the ROM would have it. (Nothing calls it: stage 1 takes the
/// device's descriptor and reads nothing of it.)
extern "C" fn t_rom_read(param: *mut c_void, dest: *mut c_void, offset: u32, size: u32) -> c_int {
    assert!(range(offset, size, *nor_size()));
    // Safety: the ROM's caller gives room for `size` bytes, and the device's parameters.
    let (param, dest) = unsafe {
        (
            &*param.cast::<[u8; 8]>(),
            core::slice::from_raw_parts_mut(dest.cast::<u8>(), size as usize),
        )
    };
    dest.copy_from_slice(&sim_nor()[offset as usize..][..size as usize]);
    if param[1] == 1 {
        assert!(size.is_multiple_of(32));
        for line in dest.chunks_exact_mut(32) {
            enc(0xffff, line);
        }
    }
    0
}

fn t_reset_boot_state() {
    // Safety: no boot is running: the last one was left where it ended.
    unsafe { boot_test_reset() };
    t().sd_calls = 0;
    t().sd_card = false;
    t().sd_result = None;
}

pub(super) fn t_prep(fixture: usize) {
    t_reset_boot_state();
    sim_reset();
    if fixture < 4 {
        assert!(t().fixture_present[fixture]);
        nor_load(&t_fixture_cache()[fixture]);
    } else {
        // Public malformed-image case: no fixture bytes are used.
        sim_nor()[..0x10_0000].fill(0xff);
        *nor_size() = 0x10_0000;
        *nor_budget() = -1;
        nor_clear_log();
    }
    sim().board = if SMK { 2 } else { 1 };
    sim().keyfn = Some(t_keyfn);
    sim().on_timeout = Some(t_timeout);
    sim().limit = 12_000_000;
    t().nkeys = 0;
    (t().app_calls, t().update_calls, t().resets) = (0, 0, 0);
    (t().app_returns, t().update_returns) = (false, false);
    t().bad_quad_on_return = false;
    t().update_hook = None;
    t().app_hook = None;
    t().app_object.fill(0);
    t().reset_retained.fill(0);
    ret::<256>(0).fill(0);
    mem(UPDATE_LOAD as usize, UPDATE_MAX as usize).fill(0);
    mem(0x01c0_2050, 18).copy_from_slice(b"1.0-abcdef0-dirty\0");
    // The descriptor has 32-bit addresses, as the chip's are. This computer's may be longer, and then these are
    // their low halves: nothing reads them (see `t_rom_read`).
    // Safety: no boot is running, and nothing else has these.
    unsafe {
        T_ROM_PARAM.get().fill(0);
        let device = T_DEVICE.get();
        device[0] = T_ROM_PARAM.get().as_ptr().addr() as u32;
        device[1] = 0;
        device[2] = 1;
        device[3] = t_rom_read as *const () as usize as u32;
    }
    sim_p33_set(0x80, 0x31);
    sim_p33_set(9, 0xa5);
    sim_p33_set(6, 0xf8);
    sim_p33_set(5, 4);
    for i in 0..16 {
        sim_p33_set(0xd0 + i, 0xff);
    }
    for i in 0..3 {
        sim_p33_set(0xad + i, 0xff);
    }
    sim().fuse[0][2] = 0x1234;
    sim().fuse[0][3] = u32::MAX;
    sim().fuse[1][3] = 0x0098_0f00;
    t().cases += 1;
}

/// A boot from stage 0's entry, to the way out it takes.
pub(super) fn t_go() -> i32 {
    // The boot is on a thread of its own, which its end stops where it is.
    // Safety: the descriptor is a static, and nothing writes it while a boot runs.
    let end = escape::run(|| stage0_main(unsafe { T_DEVICE.get() }.as_ptr()));
    assert!(end.is_err(), "stage0_main returned");
    check!(sim().err == 0);
    if sim().err != 0 {
        eprintln!("simulator: {}", sim().errmsg);
    }
    end.unwrap_err()
}

pub(super) fn t_usb_reset() -> bool {
    t().reset_retained[..16] == *b"usb_update_mode\0"
}

pub(super) fn t_own_fixture() -> usize {
    if SMK { 1 } else { 0 }
}

// Decode the panel's actual RGB565 framebuffer using Appendix A's glyphs.
const T_CHARACTERS: &[u8; 45] = b" -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const T_GLYPHS: [[u8; 5]; 45] = [
    [0, 0, 0, 0, 0],
    [8, 8, 8, 8, 8],
    [0, 0x60, 0x60, 0, 0],
    [0x60, 0x10, 8, 4, 3],
    [0, 0x36, 0x36, 0, 0],
    [0, 8, 0x14, 0x22, 0x41],
    [0x41, 0x22, 0x14, 8, 0],
    [8, 8, 0x3e, 8, 8],
    [2, 1, 0x51, 9, 6],
    [0x3e, 0x51, 0x49, 0x45, 0x3e],
    [0, 0x42, 0x7f, 0x40, 0],
    [0x42, 0x61, 0x51, 0x49, 0x46],
    [0x21, 0x41, 0x45, 0x4b, 0x31],
    [0x18, 0x14, 0x12, 0x7f, 0x10],
    [0x27, 0x45, 0x45, 0x45, 0x39],
    [0x3c, 0x4a, 0x49, 0x49, 0x30],
    [1, 0x71, 9, 5, 3],
    [0x36, 0x49, 0x49, 0x49, 0x36],
    [6, 0x49, 0x49, 0x29, 0x1e],
    [0x7e, 9, 9, 9, 0x7e],
    [0x7f, 0x49, 0x49, 0x49, 0x36],
    [0x3e, 0x41, 0x41, 0x41, 0x22],
    [0x7f, 0x41, 0x41, 0x41, 0x3e],
    [0x7f, 0x49, 0x49, 0x49, 0x41],
    [0x7f, 9, 9, 9, 1],
    [0x3e, 0x41, 0x49, 0x49, 0x7a],
    [0x7f, 8, 8, 8, 0x7f],
    [0, 0x41, 0x7f, 0x41, 0],
    [0x20, 0x40, 0x41, 0x3f, 1],
    [0x7f, 8, 0x14, 0x22, 0x41],
    [0x7f, 0x40, 0x40, 0x40, 0x40],
    [0x7f, 2, 0x0c, 2, 0x7f],
    [0x7f, 4, 8, 0x10, 0x7f],
    [0x3e, 0x41, 0x41, 0x41, 0x3e],
    [0x7f, 9, 9, 9, 6],
    [0x3e, 0x41, 0x51, 0x21, 0x5e],
    [0x7f, 9, 0x19, 0x29, 0x46],
    [0x46, 0x49, 0x49, 0x49, 0x31],
    [1, 1, 0x7f, 1, 1],
    [0x3f, 0x40, 0x40, 0x40, 0x3f],
    [0x1f, 0x20, 0x40, 0x20, 0x1f],
    [0x3f, 0x40, 0x38, 0x40, 0x3f],
    [0x63, 0x14, 8, 0x14, 0x63],
    [3, 4, 0x78, 4, 3],
    [0x61, 0x51, 0x49, 0x45, 0x43],
];

fn t_row(row: usize) -> String {
    let fb = &sim().fb;
    let mut text = String::new();
    for c in 0..20 {
        let bg = fb[(row * 16 + 15) * 240 + c * 12 + 11];
        let mut character = b'?';
        for (g, glyph) in T_GLYPHS.iter().enumerate() {
            let mut matches = true;
            'cell: for y in 0..16 {
                for x in 0..12 {
                    let on = x < 10 && y < 14 && glyph[x / 2] & 1 << (y / 2) != 0;
                    if on == (fb[(row * 16 + y) * 240 + c * 12 + x] == bg) {
                        matches = false;
                        break 'cell;
                    }
                }
            }
            if matches {
                character = T_CHARACTERS[g];
                break;
            }
        }
        text.push(character as char);
    }
    text.truncate(text.trim_end_matches(' ').len());
    text
}

pub(super) fn t_row_is(row: usize, wanted: &str) -> bool {
    let got = t_row(row);
    let equal = got == wanted;
    if !equal {
        eprintln!("row {row}: '{got}', wanted '{wanted}'");
    }
    equal
}

pub(super) fn t_damage_app() {
    sim_nor()[0x9000] ^= 0x10;
}

#[cfg(not(midi_mode))]
fn t_no_scan_request() {
    sim_nor()[0xbef00..][..80].fill(0xff);
}

#[cfg(not(midi_mode))]
fn t_ram_request() {
    ret::<80>(8).copy_from_slice(&sim_nor()[0xbef00..][..80]);
    assert!(request_valid(ret(8)));
}

pub(super) fn t_open_menu() {
    t_key(KEY_SELECT, 0, 1200);
}

#[cfg(not(midi_mode))]
fn t_pick_bypass() {
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 6100);
}

#[cfg(not(midi_mode))]
fn t_normal() {
    for f in (if SMK { 1 } else { 0 })..(if SMK { 4 } else { 1 }) {
        if !t().fixture_present[f] {
            continue;
        }
        t_prep(f);
        sim().jedec = if SMK { 0xeb6014 } else { 0x856014 };
        sim().limit = 1_000_000;
        check!(t_go() == END_APP);
        check!(sim().startup_us > 0 && sim().startup_us < 100_000);
        check!(t().app_calls == 1 && t().update_calls == 0 && t().resets == 0);
        check!(t().app_entry == 0x0200_0120);
        check!(le32(&t().app_object, 0) == 0x01c7_fe20);
        check!(le32(&t().app_object, 4) == 0x4000);
        check!(le32(&t().app_object, 8) == 0x0200_0000);
        check!(le16(&t().app_object, 12) == 0x980f);
        check!(le16(&t().app_object, 14) == (0x1234 | (u32::MAX & 0xfff2_38fc)) as u16);
        check!(t().app_object[16..] == t_fixture_cache()[f][0xfff40..][..8]);
        let mut decoded: [u8; 32] = *t_fixture_cache()[f].first_chunk().unwrap();
        enc(0xffff, &mut decoded);
        check!(decoded == t().app_header);
        check!(t().sfc[0] == 0x0098_0381 && t().sfc[1] == 1 && t().sfc[2] == 0x4000);
        check!(t().sfc[3] == 1 && t().sfc[4] == 0x980f);
        check!(sim_reg_get(0x1ee_e008) & 0x300 == 0x300);
        check!(sim().ncmd == 0 && sim().now < 700_000);
        check!(sim_p33_get(0x80) == 0x21);
        check!(sim_p33_get(9) == 0xab && sim_p33_get(6) == 0xfb && sim_p33_get(5) == 4);
        for i in 0..16 {
            check!(sim_p33_get(0xd0 + i) == 0);
        }
        for i in 0..3 {
            check!(sim_p33_get(0xad + i) == 0);
        }
        check!(sim_reg_get(0x119a0) == 0x4540_0203);
        check!(sim_reg_get(0x119a4) == 0x3f50_3026);
        check!(sim_reg_get(0x119a8) == 0x0940_022b);
        check!(sim_reg_get(0x119ac) == 0x0750_310c);
        check!(sim().dma_dummy_quad > 0);
        if SMK {
            check!(sim_nor()[0xff010..][..0x22c] == t_fixture_cache()[f][0xc1000..][..0x22c]);
        } else {
            check!(*nor_nlog() == 0);
        }
        check!(
            le32(ret::<4>(216), 0) == 0
                && le32(ret::<4>(220), 0) == 0
                && le32(ret::<4>(224), 0) == 0
        );
        if SMK {
            check!(sim().phy_writes == 0);
        }
    }
}

#[cfg(not(midi_mode))]
fn t_keys_and_menu() {
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 1400);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(sim().ncmd == 0 && t().app_calls == 0 && *nor_nlog() == 0);
    check!(sim().init_count == 0 && sim().dma_reads == 0);
    check!(sim().now >= 1_000_000 && sim().now < 1_200_000);
    if SMK {
        check!(sim().phy_writes == 0);
    }
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY | KEY_SELECT, 0, 1400);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(sim().ncmd == 0);
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 700);
    check!(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 400);
    t_key(KEY_RECOVERY, 415, 1400);
    check!(t_go() == END_RESET && t_usb_reset());
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 400);
    t_key(KEY_RECOVERY, 450, 1400);
    check!(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 700);
    check!(t_go() == END_APP);
    check!(sim().ncmd == 0);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 400);
    t_key(KEY_SELECT, 415, 1400);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT && sim().panel_on && t().app_calls == 0);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 400);
    t_key(KEY_SELECT, 450, 1400);
    check!(t_go() == END_APP && sim().ncmd == 0);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 5000);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_keyfn(sim().now) == KEY_SELECT);
    check!(t().app_calls == 0 && t().update_calls == 0 && t().resets == 0);
    check!(t_row_is(0, TITLE));
    check!(t_row_is(1, "1.0-ABCDEF0-DIRTY"));
    check!(t_row_is(3, "APP OK"));
    check!(t_row_is(8, "> BOOT APP"));
    check!(t_row_is(9, "  DOWNLOAD MODE"));
    check!(t_row_is(10, ""));
    check!(t_row_is(13, ""));
    check!(t_row_is(
        14,
        if SMK {
            "< > MOVE SEQ PLAY OK"
        } else {
            "OCT-/+ MOVE PLAY OK"
        }
    ));
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 2000);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && t().update_calls == 0 && t().resets == 0);
    check!(t_row_is(8, "> BOOT APP"));
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 2000);
    t_key(KEY_SELECT, 2300, 2400);
    check!(t_go() == END_APP);
    check!(t().app_calls == 1 && t().update_calls == 0 && t().resets == 0);
    t_prep(t_own_fixture());
    t_open_menu();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0);
    check!(t_row_is(0, TITLE));
    check!(t_row_is(3, "APP OK"));
    check!(t_row_is(8, "> BOOT APP"));
    check!(t_row_is(9, "  DOWNLOAD MODE"));
    check!(t_row_is(10, ""));
    check!(sim().panel_on && sim().fb_pix >= 240 * 240);
    check!(
        sim().lit_us[0] != 0
            && sim().lit_us[1] != 0
            && sim().lit_us[2] != 0
            && sim().lit_us[3] == 0
    );
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_SELECT, 2000, 2100);
    check!(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t_row_is(3, "DOWNLOAD MODE"));
    // Navigation wraps: up from the first item is the last.
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_PREV, 1600, 1700);
    sim().limit = 2_300_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(8, "  BOOT APP") && t_row_is(9, "> DOWNLOAD MODE"));
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_PREV, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t_row_is(3, "DOWNLOAD MODE"));
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1602);
    sim().limit = 2_300_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(8, "> BOOT APP"));
}

#[cfg(not(midi_mode))]
fn t_bad_apps_and_bypass() {
    t_prep(t_own_fixture());
    t_damage_app();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 0 && t_row_is(3, "APP BAD CRC"));
    check!(t_row_is(2, ""));
    check!(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  BOOT ANYWAY"));
    t_prep(t_own_fixture());
    sim_nor()[0x24000..][..0x1000].fill(0xff);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(3, "APP INCOMPLETE"));
    check!(t_row_is(2, ""));
    t_prep(t_own_fixture());
    sim_nor()[0x60] ^= 1;
    t_no_scan_request();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(3, "APP NOT FOUND"));
    check!(t_row_is(2, ""));
    check!(t().app_calls == 0 && t().update_calls == 0);
    t_prep(t_own_fixture());
    t_damage_app();
    *ret(0) = *b"BOOTANY\0";
    check!(t_go() == END_APP);
    check!(le32(ret::<4>(0), 0) == 0 && le32(ret::<4>(4), 0) == 0);
    let consumed: [u8; 8] = *ret(0);
    t_prep(t_own_fixture());
    t_damage_app();
    *ret(0) = consumed;
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT && t().app_calls == 0);
    t_prep(t_own_fixture());
    t_damage_app();
    t_pick_bypass();
    check!(t_go() == END_RESET);
    check!(t().app_calls == 0 && sim().xip_count == 1);
    check!(t().reset_retained[..8] == *b"BOOTANY\0");
    t_prep(t_own_fixture());
    t_damage_app();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 4000);
    t_key(KEY_PREV, 3200, 3300);
    sim().limit = 5_000_000;
    check!(t_go() == END_TIMEOUT && t().app_calls == 0 && t().resets == 0);
    t_prep(t_own_fixture());
    t().app_returns = true;
    t_key(KEY_SELECT, 2500, 2600);
    check!(t_go() == END_RESET);
    check!(t().app_calls == 1 && sim().xip_count == 1 && !t_usb_reset());
    t_prep(t_own_fixture());
    t_damage_app();
    t().app_returns = true;
    *ret(0) = *b"BOOTANY\0";
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT && t().app_calls == 1);
    check!(le32(ret::<4>(0), 0) == 0 && le32(ret::<4>(4), 0) == 0);
    check!(t_row_is(2, "APP EXITED"));
}

#[cfg(not(midi_mode))]
fn t_updater_damage() {
    t_damage_app();
    // An updater owns the hardware and may change it. Reinitialization must
    // replace these values before any application read or XIP attempt.
    sim_wr(0x11c00, 0);
    sim_wr(0x13000, 0x1234_5678);
}

#[cfg(not(midi_mode))]
fn t_updater_repair() {
    sim_nor()[0x60..][..32].copy_from_slice(&t_fixture_cache()[1][0x60..][..32]);
    let mut changed: [u8; 32] = *sim_nor().first_chunk().unwrap();
    enc(0xffff, &mut changed);
    changed[16] ^= 1;
    let crc = crc16(0, &changed[2..]);
    put_le16(&mut changed, 0, crc);
    enc(0xffff, &mut changed);
    sim_nor()[..32].copy_from_slice(&changed);
    sim_wr(0x11c00, 0);
}

#[cfg(not(midi_mode))]
fn t_updates() {
    t_prep(1);
    t_ram_request();
    check!(t_go() == END_UPDATE);
    check!(t().update_calls == 1 && t().app_calls == 0);
    check!(t().update_entry == UPDATE_LOAD && t().update_end > 0xb9040);
    check!(crc16(0, mem(UPDATE_LOAD as usize, 23324)) == 0xb295);
    check!(le32(&t().update_object, 0) == 0x0200_0120);
    check!(le16(&t().update_object, 12) == 0x980f);
    check!(le32(&t().update_object, 20) == 0x01c7_fdb0);
    check!(le16(&t().request_crc_at_update, 0) == 0);
    check!(t().update_clock[28..][..1] == *b"\0");
    t_prep(1);
    t_ram_request();
    t().update_returns = true;
    check!(t_go() == END_APP);
    check!(t().update_calls == 1 && t().app_calls == 1);
    check!(le16(ret::<2>(8), 0) == 0);
    check!(t().init_at_app > t().init_at_update);
    t_prep(1);
    t_ram_request();
    t().update_returns = true;
    t().update_hook = Some(t_updater_damage);
    sim().limit = 3_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().update_calls == 1 && t().app_calls == 0);
    check!(le16(ret::<2>(8), 0) == 0);
    check!(t_row_is(3, "APP BAD CRC"));
    check!(t_row_is(2, "UPDATE RETURNED"));
    check!(sim().xip_count == 0);
    check!(sim().init_count > t().init_at_update);
    t_prep(1);
    t_ram_request();
    put_le32(ret::<4>(8 + 72), 0, 0xb8000);
    put_le16(ret::<2>(8), 0, crc16(0, ret::<78>(10)));
    check!(t_go() == END_APP);
    check!(t().update_calls == 0 && le16(ret::<2>(8), 0) == 0);
    check!(sim().init_count >= 3);
    t_prep(1);
    t_ram_request();
    put_le32(ret::<4>(8 + 72), 0, 0xffff_fff0);
    put_le16(ret::<2>(8), 0, crc16(0, ret::<78>(10)));
    t_damage_app();
    sim().limit = 3_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().update_calls == 0 && t().app_calls == 0 && le16(ret::<2>(8), 0) == 0);
    check!(t_row_is(3, "APP BAD CRC"));
    check!(t_row_is(2, "UPDATER REFUSED"));
    check!(sim().xip_count == 0);
    t_prep(1);
    sim_nor()[0x60] ^= 1;
    check!(t_go() == END_UPDATE);
    check!(t().update_calls == 1 && le32(&t().update_object, 0) == 0);
    // (A pointer in what the updater is handed is the low 32 bits of this computer's.)
    check!(le32(&t().update_object, 20) == boot_test_memory_meta().addr() as u32);
    t_prep(1);
    sim_nor()[0x60] ^= 1;
    t().update_returns = true;
    sim().limit = 3_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().update_calls == 1 && t().app_calls == 0);
    check!(t_row_is(3, "APP NOT FOUND"));
    check!(sim().init_count > t().init_at_update);
    t_prep(1);
    sim_nor()[0x60] ^= 1;
    sim_nor()[0xb9040 + 8 + 500] ^= 0x20;
    sim().limit = 3_500_000;
    check!(t_go() == END_TIMEOUT && t().update_calls == 0);
    check!(t_row_is(3, "APP NOT FOUND"));
    // An updater can repair metadata and change the header; both are re-read.
    t_prep(1);
    t_ram_request();
    sim_nor()[0x60] ^= 1;
    t().update_returns = true;
    t().update_hook = Some(t_updater_repair);
    check!(t_go() == END_APP);
    check!(t().update_calls == 1 && t().app_calls == 1);
    check!(t().init_at_app > t().init_at_update);
    let mut fresh_header: [u8; 32] = *sim_nor().first_chunk().unwrap();
    enc(0xffff, &mut fresh_header);
    check!(t().app_header == fresh_header);
    check!(le16(ret::<2>(8), 0) == 0);
    // The same cycle returns through the app and menu: the request stays used.
    t_prep(1);
    t_ram_request();
    t().update_returns = true;
    t().app_returns = true;
    t_key(KEY_SELECT, 3500, 3600);
    check!(t_go() == END_RESET);
    check!(t().update_calls == 1 && t().app_calls == 1 && sim().xip_count == 1);
    check!(le16(&t().reset_retained, 8) == 0);
    // First matching magic+CRC, type zero: do not skip to older valid data.
    t_prep(1);
    sim_nor()[0x60] ^= 1;
    sim_nor().copy_within(0xbef00..0xbef00 + 80, 0xfef00);
    put_le16(sim_nor(), 0xfef02, 0);
    let crc = crc16(0, &sim_nor()[0xfef02..][..78]);
    put_le16(sim_nor(), 0xfef00, crc);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT && t().update_calls == 0);
    check!(t_row_is(5, "UPDATE 2 FLASH"));
}

#[cfg(not(midi_mode))]
fn t_uart() {
    // Each with its NUL.
    const COMMANDS: [&[u8]; 2] = [b"UARTUPDATE\0", b"UART_UPDATE_CUSTOM\0"];
    for command in COMMANDS {
        t_prep(t_own_fixture());
        mem(RETAINED, command.len()).copy_from_slice(command);
        check!(t_go() == END_RESET && !t_usb_reset());
        check!(le32(&t().reset_retained, 0) == 0);
        check!(t().reset_retained[4..command.len()] == command[4..]);
        let keep: [u8; 24] = *t().reset_retained.first_chunk().unwrap();
        t_prep(t_own_fixture());
        *ret(0) = keep;
        check!(t_go() == END_APP);
    }
    t_prep(t_own_fixture());
    *ret(0) = *b"UARTUPDATEX\0";
    check!(t_go() == END_APP);
    t_prep(t_own_fixture());
    *ret(0) = *b"UARTUPDATE\0";
    sim_nor()[0x60] ^= 1;
    t_no_scan_request();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT && t().resets == 0);
    check!(ret::<11>(0) == b"UARTUPDATE\0");
}

#[cfg(not(midi_mode))]
fn t_confirmation_cosmetics() {
    const DEADLINES: [u64; 3] = [2_400_000, 3_500_000, 6_000_000];
    for phase in 0..3 {
        t_prep(t_own_fixture());
        t_damage_app();
        t_pick_bypass();
        sim().limit = DEADLINES[phase];
        check!(t_go() == END_TIMEOUT);
        check!(t().app_calls == 0 && t().update_calls == 0 && t().resets == 0);
        check!(t_row_is(8, "BOOT ANYWAY"));
        let prompt = if phase == 2 {
            if SMK {
                "RELEASE SEQ PLAY"
            } else {
                "RELEASE PLAY"
            }
        } else if SMK {
            "HOLD SEQ PLAY 3 S"
        } else {
            "HOLD PLAY 3 S"
        };
        check!(t_row_is(14, prompt));
        let (mut stale, mut invalid) = (0, 0);
        let (mut green, mut grey) = (false, false);
        for y in 208..224 {
            for x in 0..240 {
                let pixel = sim().fb[y * 240 + x];
                if !(210..222).contains(&y) {
                    stale += (pixel != 0) as u32;
                } else {
                    invalid += (pixel != GREEN && pixel != GREY) as u32;
                    green |= pixel == GREEN;
                    grey |= pixel == GREY;
                }
            }
        }
        check!(stale == 0);
        check!(invalid == 0);
        check!(green == (phase != 0));
        check!(grey == (phase != 2));
        for i in 0..*nor_nlog() {
            check!(nor_log()[i as usize].kind != 'E');
        }
    }
}

#[cfg(not(midi_mode))]
fn t_guard() {
    if !SMK {
        return;
    }
    t_prep(1);
    check!(t_go() == END_APP);
    check!(sim_nor()[0xff000..][..8] == *b"TRIMCOPY");
    let copied: [u8; 0x22c] = *sim_nor()[0xc1000..].first_chunk().unwrap();
    sim_nor()[0xc1000..][..0x1000].fill(0xff);
    sim_nor()[0x60] ^= 1;
    t_no_scan_request();
    t_reset_boot_state();
    ret::<256>(0).fill(0);
    sim().fb.fill(0);
    sim().now = 0;
    sim().limit = 3_000_000;
    sim().err = 0;
    t().app_calls = 0;
    t().update_calls = 0;
    check!(t_go() == END_TIMEOUT);
    check!(sim_nor()[0xc1000..][..0x22c] == copied);
    check!(t().app_calls == 0 && t().update_calls == 0);
}

#[cfg(not(midi_mode))]
fn t_put_config(blob: &[u8]) {
    let mut descriptor: [u8; 32] = *sim_nor()[0x40..].first_chunk().unwrap();
    enc(0xffff, &mut descriptor);
    put_le16(&mut descriptor, 2, crc16(0, blob));
    put_le32(&mut descriptor, 4, 0x38d0);
    put_le32(&mut descriptor, 8, blob.len() as u32);
    let crc = crc16(0, &descriptor[2..]);
    put_le16(&mut descriptor, 0, crc);
    enc(0xffff, &mut descriptor);
    sim_nor()[0x40..][..32].copy_from_slice(&descriptor);
    sim_nor()[0x38d0..][..blob.len()].copy_from_slice(blob);
}

#[cfg(not(midi_mode))]
fn t_config_and_flash() {
    const DROPPED: [&[u8]; 9] = [
        b"PORT_INPUT\0",
        b"PORT_OUTPUT\0",
        b"ERR_PORT_OUTPUT\0",
        b"BOOT_DELAY\0",
        b"UTRX\0",
        b"UTTX\0",
        b"DVDD\0",
        b"VDDIO\0",
        b"DCDC14\0",
    ];
    t_prep(t_own_fixture());
    let mut blob = [0; 1024];
    let mut pos = 699;
    blob[..pos].copy_from_slice(&sim_nor()[0x38d0..][..pos]);
    for dropped in DROPPED {
        blob[pos] = 4;
        pos += 1;
        // With its NUL.
        let size = dropped.len();
        blob[pos..][..size].copy_from_slice(dropped);
        pos += size;
        blob[pos..][..4].fill(0xff);
        pos += 4;
    }
    t_put_config(&blob[..pos]);
    check!(t_go() == END_APP);
    check!(sim_p33_get(9) == 0xab && sim_p33_get(6) == 0xfb && sim_p33_get(5) == 4);
    check!(sim().now < 700_000);
    t_prep(t_own_fixture());
    // The configuration is not read: a damaged one changes nothing.
    sim_nor()[0x38d0 + 100] ^= 1;
    check!(t_go() == END_APP && t().app_calls != 0);
    t_prep(t_own_fixture());
    let mut record: [u8; 32] = *sim_nor()[0x40..].first_chunk().unwrap();
    enc(0xffff, &mut record);
    put_le32(&mut record, 8, 0x2000);
    let crc = crc16(0, &record[2..]);
    put_le16(&mut record, 0, crc);
    enc(0xffff, &mut record);
    sim_nor()[0x40..][..32].copy_from_slice(&record);
    check!(t_go() == END_APP && t().app_calls != 0);
    t_prep(t_own_fixture());
    sim().bad_quad = true;
    check!(t_go() == END_APP);
    check!(t().sfc[0] == 0x0098_0281 && boot_test_effective_width() == 2);
    check!(sim().dma_dummy_dual > 0);
    t_prep(t_own_fixture());
    sim().bad_quad = true;
    sim().bad_dual = true;
    check!(t_go() == END_APP);
    check!(t().sfc[0] == 0x0090_0089 && boot_test_effective_width() == 1);
    t_prep(t_own_fixture());
    t().app_returns = true;
    t().bad_quad_on_return = true;
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().app_calls == 1 && sim().xip_count == 1);
    check!(t().sfc[0] == 0x0098_0381);
    check!(boot_test_effective_width() == 2);
    check!(t_row_is(13, "SPI WIDTH FALLBACK"));
    t_prep(t_own_fixture());
    sim().bad_quad = true;
    t_open_menu();
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(13, "SPI WIDTH FALLBACK"));
    t_prep(t_own_fixture());
    sim().sr2 = 0;
    sim().ignore_wrsr = true;
    check!(t_go() == END_APP && boot_test_effective_width() == 2);
    t_prep(t_own_fixture());
    sim().bad_quad = true;
    t_damage_app();
    *ret(0) = *b"BOOTANY\0";
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT && t().app_calls == 0);
    check!(t_row_is(3, "APP BAD CRC"));
}

#[cfg(not(midi_mode))]
fn t_pin_restoration() {
    for menu_path in [false, true] {
        t_prep(t_own_fixture());
        let mut saved = [[0; 8]; 2];
        for p in 0..2 {
            for r in 0..8 {
                let mut value = if T_OFFSETS[r] == 8 {
                    0xffff
                } else {
                    (0x1234 * (r as u32 + 1) + p as u32 * 0x111) & 0xffff
                };
                if p == 0 && r == 0 {
                    value &= !(bit(1) | bit(3));
                }
                saved[p][r] = value;
                sim_wr(
                    0x50000 + (if p != 0 { 7 } else { 0 }) * 0x40 + T_OFFSETS[r],
                    value,
                );
            }
        }
        sim_wr(0x16a0c, 0x00ab_0000);
        sim().phy_writes = 0;
        if menu_path {
            t_open_menu();
            t_key(KEY_SELECT, 2000, 2100);
        } else {
            t_key(KEY_SELECT, 0, 500);
        }
        check!(t_go() == END_APP);
        for p in 0..2 {
            for r in 0..8 {
                let mask = if !SMK && menu_path && p == 0 && (r == 0 || r == 1) {
                    !bit(2)
                } else {
                    u32::MAX
                };
                check!(t().pin_snapshot[p][r] & mask == saved[p][r] & mask);
            }
        }
        check!(t().phy_snapshot == 0x00ab_0000);
        if SMK && !menu_path {
            check!(sim().phy_writes == 0);
        }
    }
}

pub(super) fn t_fm1_matrix_enable() {
    if SMK {
        return;
    }
    for state in 0..4 {
        t_prep(4);
        hal_early();
        check!(apply_config());
        let original_out = bit(5) | if state & 1 != 0 { bit(2) } else { 0 };
        let original_dir = 0xffff & !(if state & 2 != 0 { bit(2) } else { 0 });
        sim_wr(0x50000, original_out);
        sim_wr(0x50008, original_dir);
        let all_keys = KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY;
        t_key(all_keys, 0, 1000);
        board_init(false);
        check!(sim_rd(0x50000) & bit(2) == 0);
        check!(sim_rd(0x50008) & bit(2) == 0);
        check!(board_keys() == all_keys);
        board_wait(3, true);
        check!(board_keys() == all_keys);
        // Every key is open with PA2 high, an input, or both.
        for disabled in 1..4 {
            sim_wr(
                0x50000,
                (sim_reg_get(0x50000) & !bit(2)) | if disabled & 1 != 0 { bit(2) } else { 0 },
            );
            sim_wr(
                0x50008,
                (sim_reg_get(0x50008) & !bit(2)) | if disabled & 2 != 0 { bit(2) } else { 0 },
            );
            check!(board_keys() == 0);
        }
        gpio_output(0, 2, false);
        check!(board_keys() == all_keys);
        board_restore();
        sim_flush();
        check!(sim_reg_get(0x50000) == original_out);
        check!(sim_reg_get(0x50008) == original_dir);
        check!(sim().err == 0 && sim().ncmd == 0 && *nor_nlog() == 0);
    }
}

#[cfg(not(midi_mode))]
fn t_public_recovery() {
    t_prep(4);
    t_key(KEY_RECOVERY, 0, 1400);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(sim().ncmd == 0 && t().app_calls == 0 && t().update_calls == 0 && *nor_nlog() == 0);
    check!(sim().init_count == 0 && sim().dma_reads == 0);
    t_prep(4);
    t_key(KEY_RECOVERY | KEY_SELECT, 0, 1400);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(sim().ncmd == 0 && t().app_calls == 0 && t().update_calls == 0);
    t_prep(4);
    // The I/O voltage is left as found, as stock V15 leaves it.
    sim_p33_set(5, 0xc1);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(sim_p33_get(5) == 0xc1);
    check!(t().app_calls == 0 && t().update_calls == 0 && t().resets == 0 && *nor_nlog() == 0);
    check!(t_row_is(0, TITLE));
    check!(t_row_is(1, "1.0-ABCDEF0-DIRTY"));
    check!(t_row_is(3, "APP NOT FOUND"));
    check!(t_row_is(8, "> DOWNLOAD MODE"));
}

#[cfg(not(midi_mode))]
fn t_sd_menu() {
    // No card: no item.
    t_prep(4);
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().sd_calls == 0);
    check!(t_row_is(8, "> DOWNLOAD MODE"));
    // A card puts it on top, selected.
    t_prep(4);
    t().sd_card = true;
    sim().limit = 2_500_000;
    check!(t_go() == END_TIMEOUT);
    check!(t_row_is(8, "> SD CARD"));
    check!(t_row_is(9, "  DOWNLOAD MODE"));
    // A failure shows in the menu, which stays usable.
    t_prep(4);
    t().sd_card = true;
    t().sd_result = Some(text!("NO /MBOOT/MENU.BIN"));
    t_key(KEY_SELECT, 2000, 2100);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().sd_calls == 1 && t().resets == 0);
    check!(t_row_is(2, "NO /MBOOT/MENU.BIN"));
    check!(t_row_is(8, "> SD CARD"));
    // Stage 2 returning comes back here too, with the old reason cleared.
    t_prep(4);
    t().sd_card = true;
    t_key(KEY_SELECT, 2000, 2100);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().sd_calls == 1 && t().resets == 0);
    check!(t_row_is(2, ""));
    check!(t_row_is(8, "> SD CARD"));
    // An image that leaves the panel off still gets the menu back.
    t_prep(4);
    t().sd_card = true;
    t().sd_leaves_spi_off = true;
    t_key(KEY_SELECT, 2000, 2100);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    t().sd_leaves_spi_off = false;
    check!(t().sd_calls == 1 && t().resets == 0);
    check!(sim().panel_on);
    check!(t_row_is(8, "> SD CARD"));
    // Moving down still reaches the rest.
    t_prep(4);
    t().sd_card = true;
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t().sd_calls == 0);
}

#[cfg(all(test_requests, not(midi_mode)))]
fn t_requests() {
    // Starts the SD card item with no key, once, consumes the request, and ends in download mode.
    t_prep(4);
    t().sd_card = true;
    *ret(0) = *b"MBTEST\x04\0";
    sim().limit = 3_000_000;
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t().sd_calls == 1 && t().app_calls == 0 && t().resets == 1);
    // The same when the run fails.
    t_prep(4);
    t().sd_card = true;
    t().sd_result = Some(text!("NO /MBOOT/MENU.BIN"));
    *ret(0) = *b"MBTEST\x04\0";
    sim().limit = 3_000_000;
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t().sd_calls == 1 && t().app_calls == 0);
    // And with no card, without running anything.
    t_prep(4);
    *ret(0) = *b"MBTEST\x04\0";
    sim().limit = 3_000_000;
    check!(t_go() == END_RESET && t_usb_reset());
    check!(t().sd_calls == 0 && t().app_calls == 0);
    // A person choosing the item without a request stays in the menu, as in a release.
    t_prep(4);
    t().sd_card = true;
    t().sd_result = Some(text!("NO /MBOOT/MENU.BIN"));
    t_key(KEY_SELECT, 2000, 2100);
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().sd_calls == 1 && t().resets == 0);
    // Anything else chooses nothing, such as 3, which no item has.
    t_prep(4);
    t().sd_card = true;
    *ret(0) = *b"MBTEST\x03\0";
    sim().limit = 3_000_000;
    check!(t_go() == END_TIMEOUT);
    check!(t().sd_calls == 0 && t().resets == 0);
    check!(le32(ret::<4>(0), 0) == 0 && le32(ret::<4>(4), 0) == 0);
    check!(t_row_is(8, "> SD CARD"));
    // With firmware, that's a normal start, and so is having no request at all.
    if !t().fixture_present[t_own_fixture()] {
        return;
    }
    t_prep(t_own_fixture());
    *ret(0) = *b"MBTEST\x03\0";
    check!(t_go() == END_APP);
    t_prep(t_own_fixture());
    check!(t_go() == END_APP);
}

// What <sys/mman.h> has, on Linux.
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_PRIVATE: c_int = 2;
const MAP_ANONYMOUS: c_int = 0x20;
/// At that address or not at all: nothing that is already there is replaced.
const MAP_FIXED_NOREPLACE: c_int = 0x10_0000;

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
}

/// New memory at an address of the chip.
fn map(address: usize, size: usize) -> usize {
    // Safety: a new mapping, which replaces nothing.
    unsafe {
        mmap(
            core::ptr::without_provenance_mut(address),
            size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
            -1,
            0,
        )
    }
    .expose_provenance()
}

/// The chip's RAM, and where it has its flash mapped: the bootloader uses both at their addresses. (The start of
/// both C programs' `main`.)
pub(super) fn t_map() {
    let ram = map(0x01c0_0000, 0x80000);
    assert!(ram == 0x01c0_0000);
    let xip = map(0x0200_0000, 0x10_0000);
    assert!(xip == 0x0200_0000);
}

#[cfg(not(midi_mode))]
#[test]
fn boot() {
    t_map();
    for f in 0..4 {
        if (SMK && f == 0) || (!SMK && f > 1) {
            continue;
        }
        let name = T_FIXTURE_NAMES[f];
        let mut input = match File::open(format!("testdata/{name}.bin")) {
            Err(e) if e.kind() == ErrorKind::NotFound => {
                println!("SKIP: {name}.bin not present (boot fixture groups)");
                continue;
            }
            input => input,
        };
        assert!(
            input
                .as_mut()
                .is_ok_and(|input| input.read_exact(&mut t_fixture_cache()[f]).is_ok())
        );
        assert!(matches!(input.unwrap().read(&mut [0]), Ok(0)));
        t().fixture_present[f] = true;
    }
    println!(
        "{}",
        if SMK {
            "boot simulation: SMK-37"
        } else {
            "boot simulation: FM-1"
        }
    );
    t_fm1_matrix_enable();
    t_public_recovery();
    t_sd_menu();
    #[cfg(test_requests)]
    t_requests();
    t_normal();
    if t().fixture_present[t_own_fixture()] {
        t_keys_and_menu();
        t_bad_apps_and_bypass();
        t_uart();
        t_confirmation_cosmetics();
        t_guard();
        t_config_and_flash();
        t_pin_restoration();
    }
    if t().fixture_present[1] {
        t_updates();
    }
    println!(
        "boot simulation: {} scenarios, {} checks, {} failures",
        t().cases,
        t().checks,
        t().failures
    );
    // The C program's exit code.
    assert!(t().failures == 0);
}
