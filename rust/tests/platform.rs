// SPDX-License-Identifier: MIT
//! The clock, the timer, P33 and the flash's pins (docs/SPEC.md §3), on registers that this test stands in for.
use crate::Global;
use crate::hw::bit;
use crate::platform::{
    flash_capacity, flash_init, hal_clock_a, hal_clock_b, hal_delay_us, hal_fault, hal_fault_clear,
    hal_xip, p33_and, p33_or, p33_read, p33_write,
};

const SPI: usize = 0x11c00;
const P33_CTRL: usize = 0x13e08;
const P33_DATA: usize = 0x13e0c;
const TIMER: usize = 0x10400;

struct Model {
    low: [u32; 0x60000 / 4],
    cache: [u32; 0xb200 / 4],
    cache_control: u32,
    previous_address: usize,
    spi_write: bool,
    spi_reply: bool,
    p33_write_pending: bool,
    p33_reply: bool,
    timer_stuck: bool,
    spi_position: usize,
    p33_count: usize,
    p33_busy: u32,
    timer_count: usize,
    p33_bytes: [u8; 16],
    periods: [u32; 16],
    timer_seen: u32,
}

static MODEL: Global<Model> = Global::new(Model {
    low: [0; 0x60000 / 4],
    cache: [0; 0xb200 / 4],
    cache_control: 0,
    previous_address: 0,
    spi_write: false,
    spi_reply: false,
    p33_write_pending: false,
    p33_reply: false,
    timer_stuck: false,
    spi_position: 0,
    p33_count: 0,
    p33_busy: 0,
    timer_count: 0,
    p33_bytes: [0; 16],
    periods: [0; 16],
    timer_seen: 0,
});

/// Safety, for every use: what this gives is not kept across a call of the code under test, which comes back here
/// for every register it reads or writes.
fn model() -> &'static mut Model {
    unsafe { MODEL.get() }
}

/// Commit writes made through the previous register pointer before the next access.
/// Completion/response values are supplied by this independent peripheral model.
fn advance() {
    let m = model();
    if m.low[SPI / 4] & bit(14) != 0 {
        m.low[SPI / 4] &= !(bit(14) | bit(15));
    }
    if m.spi_write {
        const JEDEC: [u8; 3] = [0xeb, 0x60, 0x14];
        if m.low[SPI / 4] & bit(12) != 0 {
            assert!(m.spi_position < JEDEC.len());
            m.low[(SPI + 8) / 4] = JEDEC[m.spi_position] as u32;
            m.spi_position += 1;
        } else {
            m.low[(SPI + 8) / 4] = 0xff;
        }
        m.low[SPI / 4] |= bit(15);
        m.spi_write = false;
        m.spi_reply = true;
    }
    let ctrl = P33_CTRL / 4;
    if m.p33_write_pending && m.previous_address == P33_CTRL && m.low[ctrl] & bit(4) != 0 {
        assert!(m.low[ctrl] & 0x1d == 0x1d);
        // CS, byte fields and start held.
        assert!(m.p33_count < m.p33_bytes.len());
        m.p33_bytes[m.p33_count] = m.low[P33_DATA / 4] as u8;
        m.p33_count += 1;
        m.p33_write_pending = false;
        m.low[ctrl] |= bit(1);
        m.p33_busy = 2;
    }
    if m.low[TIMER / 4] != m.timer_seen {
        assert!(m.low[TIMER / 4] == 0x4000 || m.low[TIMER / 4] == 1);
        if m.low[TIMER / 4] == 1 {
            assert!(m.low[(TIMER + 4) / 4] == 0 && m.low[(TIMER + 8) / 4] != 0);
            assert!(m.timer_count < m.periods.len());
            m.periods[m.timer_count] = m.low[(TIMER + 8) / 4];
            m.timer_count += 1;
            if !m.timer_stuck {
                m.low[TIMER / 4] |= bit(15);
            }
        }
        m.timer_seen = m.low[TIMER / 4];
    }
}

/// The crate's registers, under `platform_sim`: every access of one comes here for where it is.
pub fn platform_register(address: usize) -> *mut u32 {
    advance();
    let m = model();
    assert!(address & 3 == 0);
    if address == SPI + 8 {
        if m.spi_reply {
            m.spi_reply = false;
        } else {
            m.spi_write = true;
        }
    }
    if address == P33_DATA {
        if m.p33_reply {
            m.p33_reply = false;
        } else {
            m.p33_write_pending = true;
        }
    }
    if address == P33_CTRL && m.p33_busy != 0 {
        m.p33_busy -= 1;
        if m.p33_busy == 0 {
            m.low[P33_CTRL / 4] &= !bit(1);
            m.low[P33_DATA / 4] = 0x5a;
            m.p33_reply = true;
        }
    }
    m.previous_address = address;
    if address < 0x60000 {
        return &raw mut m.low[address / 4];
    }
    if address == 0x1eee008 {
        return &raw mut m.cache_control;
    }
    assert!((0x1f00000..0x1f0b200).contains(&address));
    &raw mut m.cache[(address - 0x1f00000) / 4]
}

fn reset_model() {
    let m = model();
    m.low.fill(0);
    m.cache.fill(0xa5a5_a5a5);
    m.previous_address = 0;
    m.spi_write = false;
    m.spi_reply = false;
    m.p33_write_pending = false;
    m.p33_reply = false;
    m.timer_stuck = false;
    m.spi_position = 0;
    m.p33_count = 0;
    m.p33_busy = 0;
    m.timer_count = 0;
    m.timer_seen = 0;
    m.cache_control = bit(14);
    hal_fault_clear();
}

fn test_drive_and_mux() {
    let (hd0, hd, mux) = (0x9123_4568, 0x1234_0001, 0xa504_0020);
    for entry_bit in 0..2 {
        reset_model();
        model().low[(0x500c0 + 0x18) / 4] = hd0;
        model().low[(0x500c0 + 0x1c) / 4] = hd;
        model().low[0x5101c / 4] = mux | if entry_bit != 0 { bit(16) } else { 0 };
        assert!(flash_init(1, 1));
        advance();
        assert!(flash_capacity() == 0x100000 && model().spi_position == 3);
        assert!(model().low[(0x500c0 + 0x18) / 4] == hd0);
        // §3.4: HD0 untouched.
        assert!(model().low[(0x500c0 + 0x1c) / 4] == hd | bit(4));
        assert!(model().low[0x5101c / 4] & bit(16) == if entry_bit != 0 { bit(16) } else { 0 });
        assert!(hal_xip(0x4000, 0x980f, 1, 1));
        assert!(model().low[0x5101c / 4] & bit(16) == 0);
        model().spi_position = 0;
        assert!(flash_init(1, 1));
        advance();
        // §3.6: raw SPI after XIP.
        assert!(model().low[0x5101c / 4] & bit(16) == 0);
        assert!(model().low[0x5101c / 4] & !(bit(5) | bit(16)) == mux & !(bit(5) | bit(16)));
        assert!(!hal_fault());
    }
}

fn test_timer() {
    reset_model();
    assert!(hal_clock_a());
    assert!(hal_delay_us(120000));
    advance();
    let m = model();
    assert!(m.timer_count == 2 && m.periods[0] == 6000000 && m.periods[1] == 1200000);
    m.timer_count = 0;
    assert!(hal_delay_us(150000));
    advance();
    let m = model();
    assert!(m.timer_count == 2 && m.periods[0] == 6000000 && m.periods[1] == 3000000);
    m.timer_count = 0;
    assert!(hal_delay_us(0));
    advance();
    assert!(model().timer_count == 0);
    model().low[0x10014 / 4] = 0x80;
    model().low[0x10008 / 4] = 0;
    assert!(hal_clock_b() == 96);
    assert!(hal_delay_us(50));
    advance();
    let m = model();
    assert!(m.timer_count == 1 && m.periods[0] == 2400 && m.low[TIMER / 4] == 0x4000);
    m.timer_stuck = true;
    assert!(!hal_delay_us(1));
    advance();
    assert!(hal_fault() && model().low[TIMER / 4] == 0x4000);
}

fn expect_transaction(op: u32, address: u32, value: u32) {
    advance();
    let m = model();
    assert!(m.p33_count == 3 && m.p33_busy == 0);
    assert!(m.p33_bytes[0] as u32 == op | address >> 8);
    assert!(m.p33_bytes[1] == address as u8 && m.p33_bytes[2] as u32 == value);
    assert!(m.low[P33_CTRL / 4] & !31 == 0xa500_0000);
    assert!(m.low[P33_CTRL / 4] & bit(0) == 0 && m.low[P33_CTRL / 4] & bit(4) != 0);
    assert!(!hal_fault());
    model().p33_count = 0;
}

fn test_p33() {
    reset_model();
    model().low[P33_CTRL / 4] = 0xa500_0000;
    assert!(p33_read(0x123) == 0x5a);
    expect_transaction(0x80, 0x123, 0x5e);
    p33_write(0x205, 0x6c);
    expect_transaction(0, 0x205, 0x6c);
    p33_or(0x80, 0x10);
    expect_transaction(0x20, 0x80, 0x10);
    p33_and(0x80, 0xef);
    expect_transaction(0x40, 0x80, 0xef);
}

#[test]
fn platform() {
    test_drive_and_mux();
    test_timer();
    test_p33();
    println!(
        "platform: §3.4 HD register, §3.3 timer, §3.1 P33 and §3.6 raw/XIP mux regression checks passed"
    );
}
