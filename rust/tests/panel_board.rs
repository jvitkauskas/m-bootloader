// SPDX-License-Identifier: MIT
//! The keys, LEDs and shift registers of both boards (docs/SPEC.md §8) and the panel (§8.4), on pins and registers
//! that this test stands in for.
use crate::Global;
use crate::board::{board_init, board_keys, board_restore, board_wait};
use crate::display;
use crate::hw::bit;

// The keys as `board_keys()` gives them: the test's own copies, so that a wrong value in the crate fails here.
const KEY_PREV: u32 = 1;
const KEY_SELECT: u32 = 4;
const KEY_RECOVERY: u32 = 8;

struct Model {
    registers: [u32; 0x60000 / 4],
    stream_word: u32,
    words: [u32; 2048],
    bits: u32,
    word_count: usize,
    reads: u32,
    pressed: u32,
    dmas: u32,
    serial_count: usize,
    delays: u32,
    smk: bool,
    failed: bool,
    delay_fail: bool,
    spi_fail: bool,
    serial: [u8; 20000],
    pending_byte: bool,
}

static MODEL: Global<Model> = Global::new(Model {
    registers: [0; 0x60000 / 4],
    stream_word: 0,
    words: [0; 2048],
    bits: 0,
    word_count: 0,
    reads: 0,
    pressed: 0,
    dmas: 0,
    serial_count: 0,
    delays: 0,
    smk: false,
    failed: false,
    delay_fail: false,
    spi_fail: false,
    serial: [0; 20000],
    pending_byte: false,
});

/// Safety, for every use: what this gives is not kept across a call of the code under test, which comes back here
/// for every register and pin it reads or writes.
fn model() -> &'static mut Model {
    unsafe { MODEL.get() }
}

fn gpio(p: u32, o: u32) -> usize {
    (0x50000 + p * 0x40 + o) as usize
}

/// The crate's registers, under `platform_sim`: every access of one comes here for where it is.
pub fn platform_register(a: usize) -> *mut u32 {
    let m = model();
    assert!(a & 3 == 0 && a < 0x60000);
    if m.pending_byte && a != 0x11d08 {
        assert!(m.serial_count < m.serial.len());
        m.serial[m.serial_count] = m.registers[0x11d08 / 4] as u8;
        m.serial_count += 1;
        m.pending_byte = false;
    }
    if a == 0x11d08 {
        m.pending_byte = true;
    }
    if a == 0x11d00 {
        let count = m.registers[0x11d10 / 4];
        if count != 0 {
            assert!(count <= 384 && count & 1 == 0);
            m.dmas += 1;
            m.registers[0x11d10 / 4] = 0;
        }
        if !m.spi_fail {
            m.registers[a / 4] |= bit(15);
        } else {
            m.registers[a / 4] &= !bit(15);
        }
    }
    &raw mut m.registers[a / 4]
}

// Stage 0's functions, which the crate takes from this test (`import_early`) the way stage 1 takes them from
// stage 0: by their names, through the linker.

#[unsafe(no_mangle)]
pub extern "C" fn gpio_level(port: u32, pin: u32, high: bool) {
    let m = model();
    let out = gpio(port, 0) / 4;
    let old = m.registers[out] & bit(pin) != 0;
    if high {
        m.registers[out] |= bit(pin);
    } else {
        m.registers[out] &= !bit(pin);
    }
    if port == 0 && pin == 3 && high && !old {
        m.stream_word = m.stream_word << 1 | (m.registers[out] & bit(4) != 0) as u32;
        m.bits += 1;
    }
    if port == 0 && pin == 1 && high && !old {
        assert!(m.bits == if m.smk { 32 } else { 16 });
        assert!(m.word_count < 2048);
        m.words[m.word_count] = m.stream_word;
        m.word_count += 1;
        m.bits = 0;
        m.stream_word = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn gpio_output(port: u32, pin: u32, high: bool) {
    gpio_level(port, pin, high);
    model().registers[gpio(port, 8) / 4] &= !bit(pin);
}

#[unsafe(no_mangle)]
pub extern "C" fn gpio_input(port: u32, pin: u32, up: bool, down: bool) {
    let m = model();
    m.registers[gpio(port, 8) / 4] |= bit(pin);
    m.registers[gpio(port, 12) / 4] |= bit(pin);
    if up {
        m.registers[gpio(port, 16) / 4] |= bit(pin);
    } else {
        m.registers[gpio(port, 16) / 4] &= !bit(pin);
    }
    if down {
        m.registers[gpio(port, 20) / 4] |= bit(pin);
    } else {
        m.registers[gpio(port, 20) / 4] &= !bit(pin);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn gpio_read(port: u32, pin: u32) -> bool {
    let m = model();
    assert!(port == if m.smk { 7 } else { 0 });
    assert!(
        pin == if m.smk {
            8
        } else if m.reads % 4 < 2 {
            8
        } else {
            6
        }
    );
    let key = m.reads % 4;
    m.reads += 1;
    let enabled =
        m.smk || (m.registers[gpio(0, 0) / 4] | m.registers[gpio(0, 8) / 4]) & bit(2) == 0;
    !enabled || m.pressed & 1 << key == 0
}

#[unsafe(no_mangle)]
pub extern "C" fn hal_delay_us(usec: u32) -> bool {
    let m = model();
    assert!(usec <= 150000);
    m.delays += 1;
    if m.delay_fail {
        m.failed = true;
        return false;
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn hal_fault() -> bool {
    model().failed
}

#[unsafe(no_mangle)]
pub extern "C" fn hal_fault_clear() {
    model().failed = false;
}

fn reset(s: bool) {
    let m = model();
    m.registers.fill(0);
    m.words.fill(0);
    m.stream_word = 0;
    m.bits = 0;
    m.word_count = 0;
    m.reads = 0;
    m.pressed = 0;
    m.dmas = 0;
    m.serial_count = 0;
    m.delays = 0;
    m.smk = s;
    m.failed = false;
    m.delay_fail = false;
    m.spi_fail = false;
    m.pending_byte = false;
}

fn keys(s: bool) {
    reset(s);
    let (mut original, phy) = ([[0; 8]; 2], 0x7654_3210);
    let offsets = [0, 8, 12, 16, 20, 24, 28, 32];
    for p in 0..2 {
        for r in 0..8 {
            original[p][r] = (if p != 0 { 0x1234 } else { 0x56aa }) ^ (r as u32 * 17);
            model().registers[gpio(if p != 0 { 7 } else { 0 }, offsets[r]) / 4] = original[p][r];
        }
    }
    // Initial levels must permit detecting the first deliberate latch edge.
    original[0][0] &= !(bit(1) | bit(3));
    if !s {
        // ROM leaves PA2 undriven; a high output latch must be cleared too.
        original[0][0] |= bit(2);
        original[0][1] |= bit(2);
        model().registers[gpio(0, 8) / 4] = original[0][1];
    }
    model().registers[gpio(0, 0) / 4] = original[0][0];
    model().registers[0x16a0c / 4] = phy;
    board_init(s);
    assert!(model().word_count == 1);
    assert!(model().registers[0x16a0c / 4] == phy);
    if !s {
        assert!(model().registers[gpio(0, 0) / 4] & bit(2) == 0);
        assert!(model().registers[gpio(0, 8) / 4] & bit(2) == 0);
    }
    model().pressed = KEY_PREV | KEY_SELECT | KEY_RECOVERY;
    assert!(board_keys() == model().pressed);
    assert!(model().reads == 4);
    const SMK_STREAM: [u32; 4] = [0xffff_feff, 0xffff_ffef, 0xffff_fffe, 0xffff_fff7];
    const FM_STREAM: [u32; 4] = [0xf7fe, 0xeffd, 0xfbff, 0xff7f];
    for i in 0..4 {
        assert!(model().words[1 + i] == if s { SMK_STREAM[i] } else { FM_STREAM[i] });
    }
    assert!(model().words[5] == if s { u32::MAX } else { 65535 });
    board_wait(6, false);
    assert!(model().registers[0x16a0c / 4] == phy);
    model().word_count = 6;
    // Discard the unlit wait's final all-off shift.
    board_wait(6, true);
    if s {
        for i in 6..12 {
            assert!(model().words[i] == if i & 1 != 0 { 0xfeef_ffff } else { 0xfeff_ffff });
            // Shifted low byte still retains all three analog-mux bits.
            assert!(model().words[i] >> 24 & 0x38 == 0x38);
        }
    }
    board_restore();
    for p in 0..2 {
        for r in 0..8 {
            assert!(
                model().registers[gpio(if p != 0 { 7 } else { 0 }, offsets[r]) / 4]
                    == original[p][r]
            );
        }
    }
    assert!(model().registers[0x16a0c / 4] == phy);
}

fn panel(s: bool) {
    reset(s);
    assert!(!display::init(3));
    assert!(model().serial_count == 0 && model().dmas == 0);
    assert!(display::init(if s { 2 } else { 1 }));
    assert!(model().dmas != 0 && model().delays != 0);
    assert!(model().registers[0x51020 / 4] & bit(4) == if s { 0 } else { bit(4) });
    let mut before = model().dmas;
    display::rect(0, 0, 0, 10, 0xffff);
    display::rect(240, 0, 1, 1, 0xffff);
    display::rect(239, 239, 2, 1, 0xffff);
    display::line(15, 0xffff, b"HI");
    static RGB: [u8; 240 * 2 * 2] = [0; 240 * 2 * 2];
    display::pixels(0, 0, 0, 1, &RGB);
    display::pixels(0, 240, 1, 1, &RGB);
    display::pixels(200, 0, 41, 1, &RGB);
    display::pixels(0, 239, 1, 2, &RGB);
    assert!(model().dmas == before);
    // in buffer-sized pieces: 960 bytes as 384, 384 and 192
    display::pixels(0, 0, 240, 2, &RGB);
    assert!(model().dmas == before + 3);
    before = model().dmas;
    display::line(0, 0xffff, b"hello");
    assert!(model().dmas == before + 20);
    display::progress(0, 0x7e0);
    display::progress(100, 0x7e0);
    display::progress(255, 0x7e0);
    before = model().dmas;
    display::release();
    display::rect(0, 0, 10, 10, 0);
    assert!(model().dmas == before);
    model().delay_fail = true;
    assert!(!display::init(if s { 2 } else { 1 }));
    assert!(!model().failed);
    model().delay_fail = false;
    model().spi_fail = true;
    assert!(!display::init(if s { 2 } else { 1 }));
    assert!(!model().failed);
}

#[test]
fn panel_board() {
    keys(false);
    keys(true);
    panel(false);
    panel(true);
    println!(
        "board/panel: shift order, key columns, GPIO/PHY restoration, bounded display and zero-area paths passed"
    );
}
