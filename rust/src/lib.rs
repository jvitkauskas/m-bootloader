// SPDX-License-Identifier: MIT
//! m-bootloader in Rust: one crate, built once for each image with the `--cfg`s the Makefile gives it.
// (With the tests in, for this computer, it is an ordinary program.)
#![cfg_attr(not(test), no_std)]
#![cfg_attr(target_arch = "pi32v2", feature(asm_experimental_arch))]
// Stage 0 is linked whole, so it is built with only what it has: docs/SPEC.md §4.4.
pub mod board;
#[cfg(any(stage1, boot_sim))]
pub mod boot;
#[cfg(not(stage0))]
pub mod contract;
#[cfg(not(stage0))]
pub mod display;
#[cfg(not(stage0))]
pub mod formats;
pub mod hw;
#[cfg(not(any(stage0, stage1)))]
pub mod installer;
// (And for its tests: by itself, `host`, and on the simulated chip, `lifecycle_test`.)
#[cfg(any(loader_a, loader_b, host, lifecycle_test))]
pub mod loader;
#[cfg(not(stage0))]
pub mod midi_update;
pub mod platform;
// On this computer, for the tests, the C library has these.
#[cfg(target_arch = "pi32v2")]
pub mod runtime;
#[cfg(not(any(stage0, boot_sim)))]
pub mod sdboot;
#[cfg(not(any(stage0, stage1)))]
pub mod sfx;
#[cfg(ram_stage)]
pub mod stage;
pub mod stage0;
#[cfg(any(test_request_sd, test_request_midi))]
pub mod test_request;
#[cfg(not(stage1))]
pub mod upkr;
#[cfg(not(any(stage0, boot_sim)))]
pub mod usb_midi;

/// The tests (rust/tests/mod.rs), built in for this computer by `rustc --test`.
#[cfg(test)]
#[path = "../tests/mod.rs"]
mod tests;

// The simulated chip has no USB, and doesn't model the card's bus: its tests stand in for both.
#[cfg(boot_sim)]
pub mod sdboot {
    pub use crate::tests::{sd_probe, sd_stage2};
}

#[cfg(all(boot_sim, midi_mode))]
pub mod usb_midi {
    pub use crate::tests::{usb_midi_poll, usb_midi_send as send, usb_midi_start, usb_midi_stop};
}

/// Bytes that the DMA or other firmware reads in words: aligned for it.
#[repr(align(4))]
pub struct Aligned<const N: usize>(pub [u8; N]);

/// A global of this program, which has one thread and no interrupts.
#[repr(transparent)]
pub struct Global<T>(core::cell::UnsafeCell<T>);

unsafe impl<T> Sync for Global<T> {}

impl<T> Global<T> {
    pub const fn new(value: T) -> Self {
        Global(core::cell::UnsafeCell::new(value))
    }

    /// # Safety
    /// Nothing else may use the global while the reference lives.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get(&self) -> &mut T {
        unsafe { &mut *self.0.get() }
    }
}

impl<T: Copy> Global<T> {
    pub fn read(&self) -> T {
        unsafe { *self.0.get() }
    }

    pub fn set(&self, value: T) {
        unsafe { *self.0.get() = value }
    }
}

#[cfg(target_arch = "pi32v2")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // Never reached: with -Cpanic=immediate-abort a panic is a trap where it happens.
    loop {}
}
