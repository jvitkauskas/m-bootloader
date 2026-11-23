// SPDX-License-Identifier: MIT
//! Simulated NOR storage: enforces 1->0 programming, 256-byte pages, 4 KiB / 64 KiB erase units, logs every
//! modifying operation, can lose "power". The simulated chip's flash (sim.rs), which the tests fill and look into.
// Not every test uses all of the chip.
#![allow(dead_code)]

use crate::Global;

pub const NOR_MAX: u32 = 0x10_0000;

/// A modifying operation: `kind` is 'P' for a page program and 'E' for an erase.
#[derive(Clone, Copy)]
pub struct NorOp {
    pub kind: char,
    pub addr: u32,
    pub len: u32,
}

static SIM_NOR: Global<[u8; NOR_MAX as usize]> = Global::new([0; NOR_MAX as usize]);
static NOR_SIZE: Global<u32> = Global::new(0);
static NOR_LOG: Global<[NorOp; 1 << 16]> = Global::new(
    [NorOp {
        kind: '\0',
        addr: 0,
        len: 0,
    }; 1 << 16],
);
static NOR_NLOG: Global<u32> = Global::new(0);
static NOR_BUDGET: Global<i64> = Global::new(-1);
static NOR_READS: Global<u32> = Global::new(0);

// Safety, for every one of these: the tests run one at a time, and what these give is not kept across a call that
// reaches the chip, which uses them itself.

/// The flash's bytes.
pub fn sim_nor() -> &'static mut [u8; NOR_MAX as usize] {
    unsafe { SIM_NOR.get() }
}

/// How much of `sim_nor` the flash has.
pub fn nor_size() -> &'static mut u32 {
    unsafe { NOR_SIZE.get() }
}

/// The modifying operations, in order: the first `nor_nlog` are in use.
pub fn nor_log() -> &'static mut [NorOp; 1 << 16] {
    unsafe { NOR_LOG.get() }
}

pub fn nor_nlog() -> &'static mut u32 {
    unsafe { NOR_NLOG.get() }
}

/// The modifying operations left before the "power" is lost and nothing more is written; -1 for no limit.
pub fn nor_budget() -> &'static mut i64 {
    unsafe { NOR_BUDGET.get() }
}

pub fn nor_reads() -> &'static mut u32 {
    unsafe { NOR_READS.get() }
}

pub fn nor_load(img: &[u8]) {
    sim_nor()[..img.len()].copy_from_slice(img);
    *nor_size() = img.len() as u32;
    *nor_budget() = -1;
    nor_clear_log();
}

pub fn nor_clear_log() {
    *nor_nlog() = 0;
    *nor_reads() = 0;
}

fn spend() -> bool {
    if *nor_budget() == 0 {
        return false;
    }
    if *nor_budget() > 0 {
        *nor_budget() -= 1;
    }
    true
}

fn log_op(k: char, a: u32, n: u32) {
    if (*nor_nlog() as usize) < nor_log().len() {
        nor_log()[*nor_nlog() as usize] = NorOp {
            kind: k,
            addr: a,
            len: n,
        };
        *nor_nlog() += 1;
    }
}

/// One page program; the caller guarantees it stays inside a page. False if the power was lost before it.
pub fn nor_page(addr: u32, s: &[u8]) -> bool {
    if !spend() {
        return false;
    }
    log_op('P', addr, s.len() as u32);
    for (i, &b) in s.iter().enumerate() {
        sim_nor()[addr as usize + i] &= b;
    }
    true
}

/// False if the power was lost before it.
pub fn nor_erase(addr: u32, n: u32) -> bool {
    if !spend() {
        return false;
    }
    log_op('E', addr, n);
    sim_nor()[addr as usize..][..n as usize].fill(0xff);
    true
}
