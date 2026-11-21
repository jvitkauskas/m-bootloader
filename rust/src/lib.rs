// SPDX-License-Identifier: MIT
//! m-bootloader in Rust: one crate, built once for each image with the `--cfg`s the Makefile gives it.
#![no_std]
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
#[cfg(not(stage0))]
pub mod midi_update;
pub mod platform;
#[cfg(not(any(stage0, boot_sim)))]
pub mod sdboot;
pub mod stage0;
#[cfg(not(any(stage0, boot_sim)))]
pub mod usb_midi;

// The simulated chip has no USB, and doesn't model the card's bus: its tests stand in for both.
#[cfg(boot_sim)]
pub mod sdboot {
    unsafe extern "C" {
        pub safe fn sd_probe(lsb_mhz: u32) -> bool;
        pub safe fn sd_stage2(lsb_mhz: u32) -> Option<crate::hw::Text>;
    }
}

#[cfg(boot_sim)]
pub mod usb_midi {
    unsafe extern "C" {
        pub safe fn usb_midi_start();
        pub safe fn usb_midi_poll();
        pub fn usb_midi_send(data: *const u8, size: u32);
        pub safe fn usb_midi_stop();
    }
}

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

/// On the host, for the tests: a panic ends the test.
#[cfg(not(target_arch = "pi32v2"))]
mod host {
    unsafe extern "C" {
        fn abort() -> !;
    }

    #[panic_handler]
    fn panic(_: &core::panic::PanicInfo) -> ! {
        unsafe { abort() }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn rust_eh_personality() {}
}
