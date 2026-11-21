// SPDX-License-Identifier: MIT
//! Installs firmware over USB MIDI, as a firmware's update loader does, so that browser installers can reach it from
//! a phone: docs/SPEC.md §11.6. The host answers our read requests with parts of the package. The whole package is
//! read and checked before anything is erased.
use core::ffi::c_char;

pub const SECTOR: usize = 0x1000;

/// What cmd 0x12 tells a host about what's answering.
#[repr(C)]
pub struct Unit {
    pub jedec: u32,
    pub capacity: u32,
    pub uid: [u8; 8],
    pub kind: u8,
    pub version: *const c_char,
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
    #[cfg(mu_test_app_ram)]
    {
        unsafe extern "C" {
            static mut mu_test_app_ram: u8;
        }
        return &raw mut mu_test_app_ram;
    }
    #[cfg(not(mu_test_app_ram))]
    return 0x01c3_0000 as *mut u8;
}

#[cfg(not(boot_sim))]
mod protocol;
#[cfg(not(boot_sim))]
pub use protocol::*;

// The simulated chip's tests have their own.
#[cfg(boot_sim)]
unsafe extern "C" {
    pub safe fn mu_run(key: u16, sector: &mut [u8; SECTOR], written: &mut bool) -> Outcome;
}
