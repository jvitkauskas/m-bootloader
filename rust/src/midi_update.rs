// SPDX-License-Identifier: MIT
//! Installs firmware over USB MIDI, as a firmware's update loader does, so that browser installers can reach it from
//! a phone: docs/SPEC.md §11.6. The host answers our read requests with parts of the package. The whole package is
//! read and checked before anything is erased.
use crate::hw::Text;

pub const SECTOR: usize = 0x1000;

/// What cmd 0x12 tells a host about what's answering.
#[repr(C)]
pub struct Unit {
    pub jedec: u32,
    pub capacity: u32,
    pub uid: [u8; 8],
    pub kind: u8,
    pub version: Text,
}

/// What runs the protocol provides: the bootloader's menu, the RAM stage, or a test.
pub trait Host {
    /// One whole F0..F7 message.
    fn send(message: &[u8]);
    /// Runs USB, and takes about a millisecond.
    fn idle();
    fn cancel() -> bool;
    fn read(address: u32, out: &mut [u8]) -> bool;
    /// One sector.
    fn erase(address: u32) -> bool;
    fn program(address: u32, data: &[u8]) -> bool;
    /// Each pass counts 0 to 100: reading the package to check it, then writing it.
    fn progress(writing: bool, percent: u32);
    fn unit_info() -> Unit;
    /// Runs the app loaded in RAM with USB stopped, then starts USB and what the app may have changed.
    fn start_app();
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// Nothing was written.
    Cancelled,
    Done,
    /// The host asked for download mode.
    Download,
    BadPackage,
    WrongKey,
    /// The host stopped answering.
    Lost,
    FlashError,
    /// The host asked for a restart; nothing was written.
    Restart,
}

/// Where a RAM app is loaded, linked and entered.
pub fn app_ram() -> *mut u8 {
    // The tests have their own.
    #[cfg(mu_test_app_ram)]
    return crate::tests::app_ram();
    #[cfg(not(mu_test_app_ram))]
    return 0x01c3_0000 as *mut u8;
}

#[cfg(not(boot_sim))]
mod protocol;
#[cfg(not(boot_sim))]
pub use protocol::*;
