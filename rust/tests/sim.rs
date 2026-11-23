// SPDX-License-Identifier: MIT
//! Register-level simulator of the chip blocks the drivers use, with a model of the SPI NOR on SPI0 and of the
//! panel on SPI1. It checks the protocol the drivers speak (opcodes, dummy clocks, write enable, page wrap,
//! protection), not electrical behaviour. The flash's storage is nor.rs, which is reached from here too.
//!
//! Adapted from the other independent implementation (a), under MIT. See NOTICE.md.
// Not every test uses all of the chip.
#![allow(dead_code)]

pub use super::nor::*;
use crate::Global;
use crate::formats::enc;
use core::cell::UnsafeCell;
use core::ffi::c_void;

// Power, clocks, timer
const PWR_CON: u32 = 0x10000;
const CLK_DIV: u32 = 0x10008;
const CLK_CON0: u32 = 0x1000C;
const CLK_CON2: u32 = 0x10014;
const CHIP_ID: u32 = 0x10200;
/// +0 control, +4 count, +8 period, +0xC pwm
const TMR0: u32 = 0x10400;
const PLL1_A: u32 = 0x119A0;
const PLL1_B: u32 = 0x119A4;
const PLL2_A: u32 = 0x119A8;
const PLL2_B: u32 = 0x119AC;
// SPI blocks: +0 control, +4 baud, +8 byte buffer, +0xC DMA address, +0x10 count
const SPI0: u32 = 0x11C00;
const SPI1: u32 = 0x11D00;
const SPI_CON: u32 = 0x00;
const SPI_BAUD: u32 = 0x04;
const SPI_BUF: u32 = 0x08;
const SPI_ADR: u32 = 0x0C;
const SPI_CNT: u32 = 0x10;
const SPI_EN: u32 = 1 << 0;
/// separate TX/RX lines
const SPI_FULL: u32 = 1 << 3;
/// direction: 1 = receive
const SPI_RX: u32 = 1 << 12;
/// write 1: clear completion
const SPI_CLR: u32 = 1 << 14;
/// completion
const SPI_DONE: u32 = 1 << 15;
/// DMA / byte width: 0 single, 1 dual, 2 quad
const fn spi_w(w: u32) -> u32 {
    w << 10
}
// Peripheral ENC (on the SPI0 DMA path)
const ENC_CON: u32 = 0x13000;
const ENC_KEY: u32 = 0x13004;
const ENC_ADR: u32 = 0x13008;
// P33 serial bridge
const P33_PMU: u32 = 0x13E00;
const P33_RTC: u32 = 0x13E04;
const P33_CON: u32 = 0x13E08;
const P33_DAT: u32 = 0x13E0C;
// High-speed USB PHY control, shared with the SMK-37 key LED commons
const PHY_CON: u32 = 0x16A0C;
// SFC (flash controller for XIP) and its ENC
const SFC_CON: u32 = 0x40200;
const SFC_BAUD: u32 = 0x40204;
const SFC_CODE: u32 = 0x40208;
const SFC_BASE: u32 = 0x4020C;
const XENC_CON: u32 = 0x40300;
const XENC_KEY: u32 = 0x40304;
// GPIO ports A..H = 0..7 and pin mux
const fn sim_gpio(p: u32) -> u32 {
    0x50000 + 0x40 * p
}
const GP_OUT: u32 = 0x00;
const GP_IN: u32 = 0x04;
/// 1 = input
const GP_DIR: u32 = 0x08;
/// digital input enable
const GP_DIE: u32 = 0x0C;
const GP_PU: u32 = 0x10;
const GP_PD: u32 = 0x14;
const GP_HD0: u32 = 0x18;
const GP_HD: u32 = 0x1C;
const GP_DIEH: u32 = 0x20;
const PA: u32 = 0;
const PB: u32 = 1;
const PC: u32 = 2;
const PD: u32 = 3;
const PE: u32 = 4;
const PF: u32 = 5;
const PG: u32 = 6;
const PH: u32 = 7;
const MUX0: u32 = 0x5101C;
const MUX1: u32 = 0x51020;
// Cache
const CACHE_CON: u32 = 0x01EE_E008;
// P33 register addresses (not CPU addresses)
const P33_WDOG: u32 = 0x80;
const P33_VDDIO: u32 = 0x05;
const P33_DCDC14: u32 = 0x06;
const P33_DVDD: u32 = 0x09;

/// What a test sets the chip up with, and what it reads back from it: `sim()`.
pub struct Sim {
    // flash chip model (storage is sim_nor())
    /// mfr<<16 | type<<8 | cap
    pub jedec: u32,
    pub uid: [u8; 8],
    pub sr1: u8,
    pub sr2: u8,
    /// part accepts 01 with two data bytes
    pub combined_wrsr: bool,
    /// status writes have no effect
    pub ignore_wrsr: bool,
    /// 6B / 3B reads return inverted data
    pub bad_quad: bool,
    pub bad_dual: bool,
    /// status reads that report WIP after a program/erase
    pub busy_polls: u32,
    pub busy_left: u32,
    /// efuse: [bank][row]
    pub fuse: [[u32; 8]; 4],
    pub chip_id: u32,
    // counters / diagnostics
    /// protocol violations seen
    pub err: u32,
    /// the first of them
    pub errmsg: String,
    pub dma_reads: u32,
    pub dma_bytes: u32,
    pub byte_rx: u32,
    pub byte_tx: u32,
    pub status_polls: u32,
    pub wrsr_cnt: u32,
    pub p33_writes: u32,
    // display
    pub panel_on: bool,
    pub phy_writes: u32,
    pub init_count: u32,
    pub xip_count: u32,
    pub dma_dummy_quad: u32,
    pub dma_dummy_dual: u32,
    pub fb: [u16; 240 * 240],
    /// pixels written
    pub fb_pix: u32,
    /// log of command bytes in order
    pub cmds: [u32; 512],
    pub ncmd: u32,
    pub cmd_args: [[u8; 16]; 512],
    pub cmd_nargs: [u8; 512],
    /// zero-length DMA attempts
    pub dma_zero: u32,
    // key matrix, shift registers, LEDs
    /// 1 FM-1, 2 SMK-37
    pub board: u32,
    /// SK_* keys held at that time
    pub keyfn: Option<fn(now_us: u64) -> u32>,
    /// virtual microseconds
    pub now: u64,
    /// §3.2's measured slow-clock time spent in pre-switch busy loops.
    pub startup_us: u64,
    /// latched shift-register word (logical bit order)
    pub word: u32,
    /// clocks since the last latch
    pub sr_clocks: u32,
    /// prev, next, sel, unwanted LED on-time
    pub lit_us: [u64; 4],
    pub key_reads: u32,
    /// virtual time after which on_timeout runs (0 = none)
    pub limit: u64,
    pub on_timeout: Option<fn()>,
    pub pll_seen: bool,
    pub p33_before_pll: u32,
}

pub const SK_PREV: u32 = 1;
pub const SK_NEXT: u32 = 2;
pub const SK_SEL: u32 = 4;
pub const SK_REC: u32 = 8;

impl Sim {
    /// Everything zero, as before the first `sim_reset`.
    const fn new() -> Sim {
        Sim {
            jedec: 0,
            uid: [0; 8],
            sr1: 0,
            sr2: 0,
            combined_wrsr: false,
            ignore_wrsr: false,
            bad_quad: false,
            bad_dual: false,
            busy_polls: 0,
            busy_left: 0,
            fuse: [[0; 8]; 4],
            chip_id: 0,
            err: 0,
            errmsg: String::new(),
            dma_reads: 0,
            dma_bytes: 0,
            byte_rx: 0,
            byte_tx: 0,
            status_polls: 0,
            wrsr_cnt: 0,
            p33_writes: 0,
            panel_on: false,
            phy_writes: 0,
            init_count: 0,
            xip_count: 0,
            dma_dummy_quad: 0,
            dma_dummy_dual: 0,
            fb: [0; 240 * 240],
            fb_pix: 0,
            cmds: [0; 512],
            ncmd: 0,
            cmd_args: [[0; 16]; 512],
            cmd_nargs: [0; 512],
            dma_zero: 0,
            board: 0,
            keyfn: None,
            now: 0,
            startup_us: 0,
            word: 0,
            sr_clocks: 0,
            lit_us: [0; 4],
            key_reads: 0,
            limit: 0,
            on_timeout: None,
            pll_seen: false,
            p33_before_pll: 0,
        }
    }
}

static SIM: Global<Sim> = Global::new(Sim::new());

/// The chip as the tests see it. Safety, for every use of this and of the other statics here: the tests run one
/// at a time, and what this gives is not kept across a call that reaches the chip, which uses it itself.
pub fn sim() -> &'static mut Sim {
    unsafe { SIM.get() }
}

// ---------------------------------------------------------- generic regs

/// Registers. The drivers are handed pointers into them, so nothing makes a reference to them.
struct Cells<const N: usize>(UnsafeCell<[u32; N]>);

unsafe impl<const N: usize> Sync for Cells<N> {}

impl<const N: usize> Cells<N> {
    const fn new() -> Self {
        Cells(UnsafeCell::new([0; N]))
    }

    fn at(&self, i: usize) -> *mut u32 {
        assert!(i < N);
        // Safety: inside the array.
        unsafe { self.0.get().cast::<u32>().add(i) }
    }

    fn clear(&self) {
        // Safety: the array is this program's, and the tests run one at a time.
        unsafe { self.0.get().write_bytes(0, 1) }
    }
}

const NREG: usize = 0x60000 / 4;
static REG: Cells<NREG> = Cells::new();
static CACHE_MEM: Cells<{ 0xB200 / 4 }> = Cells::new();
/// The values of the registers that are anywhere else: their addresses are in `State::extra`.
static EXTRA: Cells<256> = Cells::new();

const NDMA: usize = 4096;

/// The rest of what the model keeps for itself.
struct State {
    pending_addr: usize,
    pending_before: u32,
    pending_kind: u32,
    spi_reply: bool,
    p33_reply: bool,
    p33_data_pending: bool,
    extra: [u32; 256],
    nextra: u32,
    // DMA address map
    dma_tab: [*const c_void; NDMA],
    dma_next: u32,
    // P33
    p33: [u8; 1024],
    p33_asserted: bool,
    p33_n: u32,
    p33_op: u32,
    p33_addr: u32,
    // key matrix and LEDs
    sr_acc: u32,
    sr_bits: [u32; 32],
}

static STATE: Global<State> = Global::new(State {
    pending_addr: 0,
    pending_before: 0,
    pending_kind: 0,
    spi_reply: false,
    p33_reply: false,
    p33_data_pending: false,
    extra: [0; 256],
    nextra: 0,
    dma_tab: [core::ptr::null(); NDMA],
    dma_next: 0,
    p33: [0; 1024],
    p33_asserted: false,
    p33_n: 0,
    p33_op: 0,
    p33_addr: 0,
    sr_acc: 0,
    sr_bits: [0; 32],
});

fn st() -> &'static mut State {
    unsafe { STATE.get() }
}

fn slot(a: u32) -> *mut u32 {
    if a < 0x60000 {
        return REG.at((a >> 2) as usize);
    }
    if (0x01F0_0000..0x01F0_B200).contains(&a) {
        return CACHE_MEM.at(((a - 0x01F0_0000) >> 2) as usize);
    }
    let s = st();
    for i in 0..s.nextra as usize {
        if s.extra[i] == a {
            return EXTRA.at(i);
        }
    }
    if s.nextra < 256 {
        let i = s.nextra as usize;
        s.extra[i] = a;
        s.nextra += 1;
        // Safety: a register of this model, as for `get` and `put`.
        unsafe { EXTRA.at(i).write(0) };
        return EXTRA.at(i);
    }
    EXTRA.at(255)
}

/// What the register at `a` holds, without the model taking part.
fn get(a: u32) -> u32 {
    // Safety: a register of this model, which nothing has a reference to.
    unsafe { slot(a).read() }
}

fn put(a: u32, v: u32) {
    unsafe { slot(a).write(v) }
}

/// A violation of the protocol: counted, and the first one's message kept. (C kept the first 159 bytes of it;
/// none of the messages is that long.)
pub fn sim_fail(msg: &str) {
    if sim().err == 0 {
        sim().errmsg = msg.to_string();
    }
    sim().err += 1;
}

// ------------------------------------------------------ DMA address map

/// The address the DMA is given for a pointer of this computer: the crate's `hw::dma_address` asks for it.
pub fn platform_dma_address(p: *const c_void) -> u32 {
    let s = st();
    for i in 0..NDMA {
        if s.dma_tab[i] == p {
            // The same address may now come from a new borrow or a reused stack slot.
            s.dma_tab[i] = p;
            return 0x1000_0000 + i as u32 * 0x10000;
        }
    }
    let i = s.dma_next % NDMA as u32;
    s.dma_tab[i as usize] = p;
    s.dma_next += 1;
    0x1000_0000 + i * 0x10000
}

/// Null if nothing was given that address.
fn dma_ptr(a: u32) -> *mut u8 {
    let i = (a.wrapping_sub(0x1000_0000) / 0x10000) as usize;
    if a < 0x1000_0000 || i >= NDMA || st().dma_tab[i].is_null() {
        sim_fail("DMA to an unknown address");
        return core::ptr::null_mut();
    }
    st().dma_tab[i]
        .cast::<u8>()
        .cast_mut()
        .wrapping_add((a & 0xFFFF) as usize)
}

// --------------------------------------------------------------- P33

/// P33 register file (not CPU space)
pub fn sim_p33_get(r: u32) -> u8 {
    st().p33[(r & 1023) as usize]
}

pub fn sim_p33_set(r: u32, v: u8) {
    st().p33[(r & 1023) as usize] = v;
}

fn p33_exec(op: u32, a: u32, d: u32) -> u32 {
    let p33 = &mut st().p33;
    let a = a as usize;
    let v = p33[a] as u32;
    sim().p33_writes += (op != 0x80) as u32;
    match op {
        0x00 => p33[a] = d as u8,
        0x20 => p33[a] |= d as u8,
        0x40 => p33[a] &= d as u8,
        0x80 => {
            if d != 0x5E {
                sim_fail("P33 read without dummy 0x5E");
            }
            return p33[a] as u32;
        }
        _ => sim_fail("P33 unknown operation"),
    }
    if a == 0xB1 && d & 2 != 0 {
        // efuse byte strobe
        let (bank, row, j) = (p33[0xB0] as u32 >> 6, d >> 6, (d >> 2) & 15);
        let w = if sim().chip_id & 0xFF != 0 { 4 } else { 3 };
        if j >= w {
            p33[0xB2] = 0;
        } else {
            p33[0xB2] = (sim().fuse[(bank & 3) as usize][(row & 7) as usize] >> (8 * j)) as u8;
        }
    }
    v
}

fn p33_bridge_write(mut val: u32) {
    let old = get(P33_CON);
    if old & 1 == 0 && val & 1 != 0 {
        st().p33_n = 0;
        st().p33_asserted = true;
    }
    if old & 1 != 0 && val & 1 == 0 {
        if st().p33_n != 3 {
            sim_fail("P33 transaction without exactly three bytes");
        }
        st().p33_asserted = false;
    }
    if val & 0x10 != 0 && old & 0x10 == 0 {
        let d = get(P33_DAT) & 0xFF;
        if !st().p33_asserted {
            sim_fail("P33 byte outside an asserted transaction");
        }
        if val & 0xC != 0xC {
            sim_fail("P33 byte without bits 3:2 = 11");
        }
        if st().p33_n == 0 {
            st().p33_op = d & 0xE0;
            st().p33_addr = (d & 3) << 8;
        } else if st().p33_n == 1 {
            st().p33_addr |= d;
        } else if st().p33_n == 2 {
            let (op, addr) = (st().p33_op, st().p33_addr);
            put(P33_DAT, p33_exec(op, addr, d));
        }
        st().p33_n += 1;
        // start is self-clearing, busy bit 1 stays clear
        val &= !0x12;
    }
    put(P33_CON, val);
}

// ------------------------------------------------------------ SPI0 + NOR

struct Chip {
    /// selected
    cs: bool,
    /// bytes since select
    n: u32,
    cmd: u8,
    addr: u32,
    /// dummy clock cycles done
    dummy: u32,
    /// running read address
    rd: u32,
    pbuf: [u8; 512],
    pn: u32,
    pbase: u32,
    wel: bool,
    /// data bytes returned in a read
    did: u32,
}

impl Chip {
    const fn new() -> Chip {
        Chip {
            cs: false,
            n: 0,
            cmd: 0,
            addr: 0,
            dummy: 0,
            rd: 0,
            pbuf: [0; 512],
            pn: 0,
            pbase: 0,
            wel: false,
            did: 0,
        }
    }
}

static CH: Global<Chip> = Global::new(Chip::new());

fn ch() -> &'static mut Chip {
    unsafe { CH.get() }
}

fn protected() -> bool {
    sim().sr1 & 0x7C != 0 || sim().sr2 & 0x40 != 0
}

fn chip_select(sel: bool) {
    let ch = ch();
    if sel && !ch.cs {
        ch.n = 0;
        ch.pn = 0;
        ch.addr = 0;
        ch.dummy = 0;
        ch.did = 0;
    } else if !sel && ch.cs {
        // rising edge: execute
        match ch.cmd {
            0x06 => ch.wel = true,
            0x01 => {
                sim().wrsr_cnt += 1;
                if ch.wel && ch.pn >= 1 && !sim().ignore_wrsr {
                    sim().sr1 = (sim().sr1 & 0x03) | (ch.pbuf[0] & 0xFC);
                    if ch.pn >= 2 && sim().combined_wrsr {
                        sim().sr2 = ch.pbuf[1];
                    }
                    sim().busy_left = sim().busy_polls;
                }
                ch.wel = false;
            }
            0x31 => {
                if ch.wel && ch.pn >= 1 && !sim().ignore_wrsr {
                    sim().sr2 = ch.pbuf[0];
                    sim().busy_left = sim().busy_polls;
                }
                ch.wel = false;
            }
            0x02 => {
                if ch.wel && !protected() && ch.n >= 4 {
                    let base = ch.addr & !255;
                    if ch.pn > 256 {
                        sim_fail("page program with more than 256 bytes");
                    } else if (ch.addr & 255) + ch.pn > 256 {
                        // §3.4: the driver splits a program at page boundaries. The chip wraps inside the page,
                        // which is modelled below, so a test can also see the bytes it put in the wrong place.
                        sim_fail("page program across a page boundary");
                    }
                    for i in 0..ch.pn {
                        let a = base + ((ch.addr + i) & 255);
                        if a < *nor_size() && !nor_page(a, &ch.pbuf[i as usize..][..1]) {
                            break;
                        }
                    }
                    sim().busy_left = sim().busy_polls;
                }
                ch.wel = false;
            }
            0x20 | 0xD8 => {
                if ch.wel && !protected() && ch.n >= 4 {
                    let sz = if ch.cmd == 0x20 { 0x1000 } else { 0x10000 };
                    let a = ch.addr & !(sz - 1);
                    if ch.cmd == 0xD8 && ch.addr != a {
                        sim_fail("D8 erase address must be block-aligned");
                    }
                    if a + sz <= *nor_size() {
                        nor_erase(a, sz);
                    }
                    sim().busy_left = sim().busy_polls;
                }
                ch.wel = false;
            }
            _ => {}
        }
    }
    ch.cs = sel;
}

/// One byte on the SPI pins. `rx`: the chip drives the byte; `w`: 0/1/2 width.
fn chip_byte(tx: u8, rx: bool, w: u32) -> u8 {
    let ch = ch();
    let cyc = 8 >> w;
    let mut out = 0xFF;
    if !ch.cs {
        sim_fail("SPI byte while chip select is high");
        return 0xFF;
    }
    if ch.n == 0 {
        if rx || w != 0 {
            sim_fail("opcode must be sent single-bit");
        }
        ch.cmd = tx;
        ch.n = 1;
        return 0xFF;
    }
    match ch.cmd {
        0x9F => {
            out = (sim().jedec >> (16 - 8 * (ch.n - 1))) as u8;
            if !rx {
                sim_fail("JEDEC read driven by the host");
            }
        }
        0x05 => {
            out = sim().sr1 & !1;
            if sim().busy_left != 0 {
                out |= 1;
                sim().status_polls += 1;
                sim().busy_left -= 1;
            }
            if !rx {
                sim_fail("status read driven by the host");
            }
        }
        0x35 => out = sim().sr2,
        0x4B => {
            if ch.n <= 4 {
                if rx {
                    sim_fail("UID dummy must be sent");
                }
            } else {
                out = sim().uid[((ch.n - 5) & 7) as usize];
            }
        }
        0x01 | 0x31 | 0x02 => {
            if ch.cmd == 0x02 && ch.n <= 3 {
                ch.addr = (ch.addr << 8) | tx as u32;
            } else if (ch.pn as usize) < ch.pbuf.len() {
                ch.pbuf[ch.pn as usize] = tx;
                ch.pn += 1;
            }
            if rx {
                sim_fail("write command with a receive byte");
            }
        }
        0x20 | 0xD8 => {
            if ch.n <= 3 {
                ch.addr = (ch.addr << 8) | tx as u32;
            }
        }
        0x03 | 0x0B | 0x3B | 0x6B => {
            if ch.n <= 3 {
                if rx || w != 0 {
                    sim_fail("read address must be sent single-bit");
                }
                ch.addr = (ch.addr << 8) | tx as u32;
                if ch.n == 3 {
                    ch.rd = ch.addr;
                }
            } else {
                let need = if ch.cmd == 0x03 { 0 } else { 8 };
                if !rx {
                    sim_fail("read data phase driven by the host");
                }
                // §3.4: the dummy clocks and the data both, in the width of the read.
                if ch.cmd == 0x6B && w != 2 {
                    sim_fail("6B dummy/data not in quad width");
                }
                if ch.cmd == 0x3B && w != 1 {
                    sim_fail("3B dummy/data not in dual width");
                }
                if ch.cmd == 0x03 && w != 0 {
                    sim_fail("03 data must be single-bit");
                }
                if ch.dummy < need {
                    ch.dummy += cyc;
                    if ch.dummy > need {
                        sim_fail("dummy clocks overshoot");
                    }
                    ch.n += 1;
                    return 0xFF;
                }
                out = if ch.rd < *nor_size() {
                    sim_nor()[ch.rd as usize]
                } else {
                    0xFF
                };
                if (ch.cmd == 0x6B && sim().bad_quad) || (ch.cmd == 0x3B && sim().bad_dual) {
                    out = !out;
                }
                ch.rd += 1;
                ch.did += 1;
            }
        }
        _ => {}
    }
    ch.n += 1;
    out
}

/// SPI0 registers
fn spi0_wr(off: u32, val: u32) {
    const CON: u32 = SPI0 + SPI_CON;
    let w = (get(CON) >> 10) & 3;
    let rx = (get(CON) >> 12) & 1 != 0;
    if off == SPI_CON {
        put(CON, (val & !0x8000) | (get(CON) & 0x8000));
        if val & SPI_CLR != 0 {
            put(CON, get(CON) & !0x8000);
        }
        return;
    }
    if off == SPI_BUF {
        if get(CON) & 1 == 0 {
            sim_fail("SPI0 byte with the block disabled");
        }
        if rx {
            put(SPI0 + SPI_BUF, chip_byte(0xFF, true, w) as u32);
            sim().byte_rx += 1;
        } else {
            chip_byte(val as u8, false, w);
            sim().byte_tx += 1;
        }
        if w != 0 {
            sim_fail("multi-bit phase sent through byte register instead of DMA");
        }
        put(CON, get(CON) | 0x8000);
        return;
    }
    if off == SPI_CNT {
        let n = val & 0xFFFF;
        let p = dma_ptr(get(SPI0 + SPI_ADR));
        if n == 0 {
            sim().dma_zero += 1;
            sim_fail("zero-length DMA");
            return;
        }
        if get(CON) & 1 == 0 {
            sim_fail("SPI0 DMA with the block disabled");
        }
        if get(ENC_CON) != 2 {
            sim_fail("SPI0 DMA without ENC pass-through (control 2)");
        }
        if !rx {
            sim_fail("SPI0 DMA in the transmit direction is not modelled");
        }
        if w != 0 && get(CON) & SPI_FULL != 0 {
            sim_fail("multi-bit DMA with bit 3 set");
        }
        if ch().n == 4 && ch().dummy == 0 && (ch().cmd == 0x6B || ch().cmd == 0x3B) {
            let expected = if ch().cmd == 0x6B { 4 } else { 2 };
            if n != expected {
                sim_fail("wide dummy must be one exact-length DMA");
            }
        }
        if !p.is_null() {
            for i in 0..n as usize {
                let b = chip_byte(0xFF, true, w);
                // Safety: where the driver said the DMA may write this many bytes.
                unsafe { p.add(i).write(b) };
            }
        }
        if ch().cmd == 0x6B && ch().did == 0 {
            sim().dma_dummy_quad += 1;
        }
        if ch().cmd == 0x3B && ch().did == 0 {
            sim().dma_dummy_dual += 1;
        }
        sim().dma_reads += 1;
        sim().dma_bytes += n;
        put(CON, get(CON) | 0x8000);
        return;
    }
    put(SPI0 + off, val);
}

// ------------------------------------------------------------- SPI1 panel

struct Lcd {
    cmd: u32,
    nargs: u32,
    args: [u8; 16],
    wx0: u32,
    wx1: u32,
    wy0: u32,
    wy1: u32,
    cx: u32,
    cy: u32,
    mem: bool,
}

impl Lcd {
    const fn new() -> Lcd {
        Lcd {
            cmd: 0,
            nargs: 0,
            args: [0; 16],
            wx0: 0,
            wx1: 0,
            wy0: 0,
            wy1: 0,
            cx: 0,
            cy: 0,
            mem: false,
        }
    }
}

static LCD: Global<Lcd> = Global::new(Lcd::new());
/// The first byte of a pixel has come, and what it was. (`sim_reset` clears them with the panel's state.)
static HALF: Global<bool> = Global::new(false);
static HI: Global<u8> = Global::new(0);

fn port_pin(port: u32, pin: u32) -> bool {
    (get(sim_gpio(port) + GP_OUT) >> pin) & 1 != 0
}

fn lcd_byte(b: u8) {
    // which board? FM-1: CS PC7, DC PC8; SMK: CS PC8, DC PB5
    let fm1 = get(MUX1) & 0x10 != 0;
    let cs = if fm1 {
        port_pin(PC, 7)
    } else {
        port_pin(PC, 8)
    };
    let dc = if fm1 {
        port_pin(PC, 8)
    } else {
        port_pin(PB, 5)
    };
    let lcd = unsafe { LCD.get() };
    if cs {
        sim_fail("panel byte with chip select high");
        return;
    }
    if !dc {
        lcd.cmd = b as u32;
        lcd.nargs = 0;
        lcd.mem = b == 0x2C;
        if b == 0x2C {
            lcd.cx = lcd.wx0;
            lcd.cy = lcd.wy0;
        }
        let sim = sim();
        if sim.ncmd < 512 {
            sim.cmds[sim.ncmd as usize] = b as u32;
            sim.cmd_nargs[sim.ncmd as usize] = 0;
            sim.ncmd += 1;
        }
        if b == 0x29 {
            sim.panel_on = true;
        }
        return;
    }
    if lcd.mem {
        if !HALF.read() {
            HI.set(b);
            HALF.set(true);
        } else {
            if lcd.cy <= lcd.wy1 && lcd.cx <= lcd.wx1 && lcd.cx < 240 && lcd.cy < 240 {
                sim().fb[(lcd.cy * 240 + lcd.cx) as usize] = (HI.read() as u16) << 8 | b as u16;
                sim().fb_pix += 1;
            } else {
                sim_fail("pixel outside the window");
            }
            lcd.cx += 1;
            if lcd.cx > lcd.wx1 {
                lcd.cx = lcd.wx0;
                lcd.cy += 1;
            }
            HALF.set(false);
        }
        return;
    }
    if lcd.nargs < 16 {
        lcd.args[lcd.nargs as usize] = b;
    }
    lcd.nargs += 1;
    let sim = sim();
    if sim.ncmd != 0 && sim.ncmd <= 512 && lcd.nargs > 0 && lcd.nargs <= 16 {
        sim.cmd_args[sim.ncmd as usize - 1][lcd.nargs as usize - 1] = b;
        sim.cmd_nargs[sim.ncmd as usize - 1] = lcd.nargs as u8;
    }
    if lcd.cmd == 0x2A && lcd.nargs == 4 {
        lcd.wx0 = (lcd.args[0] as u32) << 8 | lcd.args[1] as u32;
        lcd.wx1 = (lcd.args[2] as u32) << 8 | lcd.args[3] as u32;
    }
    if lcd.cmd == 0x2B && lcd.nargs == 4 {
        lcd.wy0 = (lcd.args[0] as u32) << 8 | lcd.args[1] as u32;
        lcd.wy1 = (lcd.args[2] as u32) << 8 | lcd.args[3] as u32;
    }
}

fn spi1_wr(off: u32, val: u32) {
    const CON: u32 = SPI1 + SPI_CON;
    if off == SPI_CON {
        put(CON, (val & !0x8000) | (get(CON) & 0x8000));
        if val & SPI_CLR != 0 {
            put(CON, get(CON) & !0x8000);
        }
        return;
    }
    if off == SPI_BUF {
        if get(CON) & 1 == 0 {
            sim_fail("SPI1 byte with the block disabled");
        }
        lcd_byte(val as u8);
        put(CON, get(CON) | 0x8000);
        return;
    }
    if off == SPI_CNT {
        let n = val & 0xFFFF;
        let p = dma_ptr(get(SPI1 + SPI_ADR));
        if n == 0 {
            sim().dma_zero += 1;
            sim_fail("zero-length panel DMA");
            return;
        }
        if !p.is_null() {
            for i in 0..n as usize {
                // Safety: where the driver said the DMA may read this many bytes.
                lcd_byte(unsafe { p.add(i).read() });
            }
        }
        put(CON, get(CON) | 0x8000);
        return;
    }
    put(SPI1 + off, val);
}

// ------------------------------------------------- key matrix and LEDs

fn sr_clock(data: u32) {
    if sim().sr_clocks < 32 {
        st().sr_bits[sim().sr_clocks as usize] = data;
    }
    sim().sr_clocks += 1;
}

fn sr_latch() {
    let n = if sim().board == 1 { 16 } else { 32 };
    if sim().sr_clocks != n {
        let m = format!("latch after {} clocks (expected {})", sim().sr_clocks, n);
        sim_fail(&m);
    }
    let (sim, st) = (sim(), st());
    st.sr_acc = 0;
    if sim.board == 1 {
        let mut k = 0;
        while k < 16 && k < sim.sr_clocks {
            // first bit in = bit 15
            st.sr_acc |= st.sr_bits[k as usize] << (15 - k);
            k += 1;
        }
    } else {
        // stream: least significant byte first, MSB first within each byte
        let mut k = 0;
        while k < 32 && k < sim.sr_clocks {
            st.sr_acc |= st.sr_bits[k as usize] << (8 * (k / 8) + 7 - (k % 8));
            k += 1;
        }
    }
    sim.word = st.sr_acc;
    sim.sr_clocks = 0;
}

fn key_down(key: u32) -> bool {
    let now = sim().now;
    sim().keyfn.is_some_and(|keys| keys(now) & key != 0)
}

/// Is a key electrically "closed" given the selected columns?
fn key_closed(key: u32) -> bool {
    let w = sim().word;
    if !key_down(key) {
        return false;
    }
    if sim().board == 1 {
        // FM-1 PA2 enables the matrix only when actively driven low (8.2).
        if (get(sim_gpio(PA) + GP_DIR) | get(sim_gpio(PA) + GP_OUT)) & (1 << 2) != 0 {
            return false;
        }
        match key {
            SK_REC => return w & 0x0080 == 0,
            SK_SEL => return w & 0x0400 == 0,
            SK_PREV => return w & 0x0001 == 0 && w & 0x0800 == 0,
            SK_NEXT => return w & 0x0002 == 0 && w & 0x1000 == 0,
            _ => {}
        }
    } else {
        match key {
            SK_PREV => return w & (1 << 16) == 0,
            SK_NEXT => return w & (1 << 28) == 0,
            SK_SEL => return w & (1 << 24) == 0,
            SK_REC => return w & (1 << 27) == 0,
            _ => {}
        }
    }
    false
}

fn check_input_pin(port: u32, pin: u32) {
    if get(sim_gpio(port) + GP_DIR) & (1 << pin) == 0
        || get(sim_gpio(port) + GP_DIE) & (1 << pin) == 0
        || get(sim_gpio(port) + GP_PU) & (1 << pin) == 0
        || get(sim_gpio(port) + GP_PD) & (1 << pin) != 0
    {
        sim_fail("key pin read without input + digital enable + pull-up");
    }
}

fn gpio_in(port: u32) -> u32 {
    let (mut v, dir) = (get(sim_gpio(port) + GP_OUT), get(sim_gpio(port) + GP_DIR));
    // inputs float high (pull-ups)
    v = (v & !dir) | dir;
    if port == PA && sim().board == 1 {
        check_input_pin(PA, 6);
        check_input_pin(PA, 8);
        sim().key_reads += 1;
        if key_closed(SK_REC) || key_closed(SK_SEL) {
            v &= !(1 << 6);
        }
        if key_closed(SK_PREV) || key_closed(SK_NEXT) {
            v &= !(1 << 8);
        }
    }
    if port == PH && sim().board == 2 {
        check_input_pin(PH, 8);
        sim().key_reads += 1;
        if key_closed(SK_REC) || key_closed(SK_SEL) || key_closed(SK_PREV) || key_closed(SK_NEXT) {
            v &= !(1 << 8);
        }
    }
    v
}

fn led_account(us: u32) {
    let (phy, pa, ph) = (
        get(PHY_CON),
        get(sim_gpio(PA) + GP_OUT),
        get(sim_gpio(PH) + GP_OUT),
    );
    let sim = sim();
    let (w, us) = (sim.word, us as u64);
    if sim.board == 1 {
        let (ph9, pa10) = ((ph >> 9) & 1 != 0, (pa >> 10) & 1 != 0);
        if ph9 && w & 0x0001 == 0 && w & 0x0800 == 0 {
            sim.lit_us[0] += us;
        }
        if ph9 && w & 0x0002 == 0 && w & 0x1000 == 0 {
            sim.lit_us[1] += us;
        }
        if pa10 && w & 0x0400 == 0 {
            sim.lit_us[2] += us;
        }
    } else if phy & 0x1000F == 0x1000F {
        // drive enabled
        let (g0, g1) = (phy & (1 << 17) == 0, phy & (1 << 18) == 0);
        if g0 && w & 1 == 0 {
            sim.lit_us[0] += us;
        }
        if g1 && w & 1 == 0 {
            sim.lit_us[1] += us;
        }
        if g1 && w & (1 << 12) == 0 {
            sim.lit_us[2] += us;
        }
        // KNOB BANK
        if g0 && w & (1 << 12) == 0 {
            sim.lit_us[3] += us;
        }
    }
}

/// A wait of the drivers that needs no timer: the crate's `platform::settle` tells of each one.
pub fn boot_sim_settle(iterations: u32) {
    sim_flush();
    if sim().board != 0 && sim_reg_get(CLK_CON0) & (1 << 8) == 0 {
        // §3.2: 672000 register-loop iterations took about 14 seconds.
        // Account only for A before the switch, not full-speed SPI CS waits.
        let us = (iterations as u64 * 1_000_000 / 48000) as u32;
        sim().startup_us += us as u64;
        sim_delay(us);
    }
}

pub fn sim_delay(us: u32) {
    led_account(us);
    sim().now += us as u64;
    if sim().limit != 0
        && sim().now > sim().limit
        && let Some(on_timeout) = sim().on_timeout
    {
        on_timeout();
    }
}

// --------------------------------------------------------------- dispatch

pub fn sim_rd(a: u32) -> u32 {
    if a == P33_CON || a == P33_DAT || a == P33_PMU || a == P33_RTC {
        return get(a);
    }
    if a == CHIP_ID {
        return sim().chip_id;
    }
    if a == CACHE_CON {
        // idle
        return get(a) | (1 << 14);
    }
    if a == TMR0 {
        // timer: expires at once
        return get(a) | 0x8000;
    }
    if a == sim_gpio(PD) + GP_IN {
        return get(sim_gpio(PD) + GP_OUT);
    }
    if a == sim_gpio(PA) + GP_IN {
        return gpio_in(PA);
    }
    if a == sim_gpio(PH) + GP_IN {
        return gpio_in(PH);
    }
    get(a)
}

pub fn sim_wr(a: u32, v: u32) {
    if a == PHY_CON {
        sim().phy_writes += 1;
    }
    if a == SPI0 && v == 0x28 {
        sim().init_count += 1;
    }
    if a == XENC_CON && v == 1 {
        let base = get(SFC_BASE);
        let key = get(XENC_KEY) as u16;
        assert!(base < *nor_size());
        let size = (*nor_size() - base) as usize;
        // Safety: where the chip has its flash mapped. A test that lets the bootloader come this far has put
        // memory there, for all of the flash.
        let dest = unsafe {
            core::slice::from_raw_parts_mut(
                core::ptr::with_exposed_provenance_mut::<u8>(0x0200_0000),
                size,
            )
        };
        dest.copy_from_slice(&sim_nor()[base as usize..][..size]);
        for (i, line) in dest.chunks_exact_mut(32).enumerate() {
            let q = i * 32;
            enc(key ^ (q >> 2) as u16, line);
        }
        sim().xip_count += 1;
    }
    if a == TMR0 && v == 1 {
        if sim().board != 0 && get(CLK_CON0) & (1 << 8) == 0 {
            sim_fail("Timer 0 used before the startup clock switch");
        }
        let mhz = if get(CLK_DIV) & 0x30700 == 0x10200 {
            60
        } else if get(CLK_CON2) & 255 == 0x40 && get(CLK_DIV) & 0x30700 == 0x100 {
            48
        } else {
            24
        };
        sim_delay(get(TMR0 + 8) / mhz);
    }
    if a == PLL1_A && !sim().pll_seen {
        sim().pll_seen = true;
        sim().p33_before_pll = sim().p33_writes;
    }
    if (SPI0..SPI0 + 0x20).contains(&a) {
        spi0_wr(a - SPI0, v);
        return;
    }
    if (SPI1..SPI1 + 0x20).contains(&a) {
        spi1_wr(a - SPI1, v);
        return;
    }
    if a == P33_CON {
        p33_bridge_write(v);
        return;
    }
    if a == sim_gpio(PD) + GP_OUT {
        let dir = get(sim_gpio(PD) + GP_DIR);
        put(a, v);
        if dir & 1 == 0 {
            // PD0 is an output: chip select
            chip_select(v & 1 == 0);
        }
        return;
    }
    if a == sim_gpio(PD) + GP_DIR {
        put(a, v);
        return;
    }
    if a == sim_gpio(PA) + GP_OUT && sim().board != 0 {
        let old = get(a);
        let dir = get(sim_gpio(PA) + GP_DIR);
        // only driven pins reach the registers
        put(a, v);
        if dir & 8 == 0 && old & 8 == 0 && v & 8 != 0 {
            // PA3 rising: clock in PA4
            sr_clock((v >> 4) & 1);
        }
        if dir & 2 == 0 && old & 2 == 0 && v & 2 != 0 {
            // PA1 rising: latch
            sr_latch();
        }
        return;
    }
    put(a, v);
}

/// last value written to an unmodelled CPU register
pub fn sim_reg_get(a: u32) -> u32 {
    get(a)
}

/// power-on state of every block; keeps sim_nor()
pub fn sim_reset() {
    REG.clear();
    CACHE_MEM.clear();
    let st = st();
    st.pending_addr = 0;
    st.pending_before = 0;
    st.pending_kind = 0;
    st.spi_reply = false;
    st.p33_reply = false;
    st.p33_data_pending = false;
    st.extra = [0; 256];
    EXTRA.clear();
    st.nextra = 0;
    st.p33 = [0; 1024];
    *ch() = Chip::new();
    *unsafe { LCD.get() } = Lcd::new();
    // (Nor does a pixel's first byte, sent before the reset.)
    HALF.set(false);
    HI.set(0);
    st.dma_tab = [core::ptr::null(); NDMA];
    st.dma_next = 0;
    st.p33_asserted = false;
    st.p33_n = 0;
    let sim = sim();
    *sim = Sim::new();
    sim.jedec = 0xEB6014;
    for i in 0..8 {
        sim.uid[i] = 0xA0 + i as u8;
    }
    sim.sr1 = 0;
    // QE already set
    sim.sr2 = 0x02;
    sim.combined_wrsr = true;
    sim.busy_polls = 3;
    sim.chip_id = 0;
    // GPIO dir after reset: inputs
    for i in 0..8 {
        put(sim_gpio(i) + GP_DIR, 0xFFFF);
    }
    // ROM state: 192 MHz PLL / 4
    put(CLK_CON2, 0x80);
    put(CLK_DIV, 0);
    // key 980F in bits 23:8
    sim.fuse[1][3] = 0x0098_0F00;
    put(SPI0, 0);
}

/// Commit the previous volatile MMIO lvalue before handing out the next one. Byte registers are writes on the first
/// access and replies on the second; DMA count registers are write-only in these drivers. Ordinary registers are
/// committed only when their value changed. The model below this adapter still checks the chip-side protocol
/// independently of the driver.
pub fn sim_flush() {
    if st().pending_addr == 0 {
        return;
    }
    let a = st().pending_addr as u32;
    let value = get(a);
    if st().pending_kind != 0 || value != st().pending_before {
        put(a, st().pending_before);
        sim_wr(a, value);
        if st().pending_kind == 1 {
            st().spi_reply = true;
        }
        if st().pending_kind == 2 {
            st().p33_data_pending = true;
        }
    }
    st().pending_addr = 0;
    st().pending_kind = 0;
}

/// Every register the drivers read or write: the crate's `hw::Reg` asks for it, and reads or writes what it gets.
pub fn platform_register(address: usize) -> *mut u32 {
    sim_flush();
    assert!(address & 3 == 0);
    let a = address as u32;
    if a == P33_CON && st().p33_data_pending {
        let v = get(a);
        if v & 0x10 != 0 {
            put(a, v & !0x10);
            p33_bridge_write(v);
            st().p33_data_pending = false;
            st().p33_reply = true;
        }
    }
    st().pending_addr = address;
    if a == SPI0 + SPI_BUF {
        if st().spi_reply {
            st().spi_reply = false;
        } else {
            st().pending_kind = 1;
        }
    } else if a == P33_DAT {
        if st().p33_reply {
            st().p33_reply = false;
        } else {
            st().pending_kind = 2;
        }
    } else if a == SPI1 + SPI_BUF || a == SPI0 + SPI_CNT || a == SPI1 + SPI_CNT {
        st().pending_kind = 3;
    }
    let value = sim_rd(a);
    put(a, value);
    st().pending_before = value;
    slot(a)
}
