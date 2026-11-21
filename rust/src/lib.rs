// SPDX-License-Identifier: MIT
//! m-bootloader in Rust: one crate, built once for each image with the `--cfg`s the Makefile gives it.
#![no_std]
#![cfg_attr(target_arch = "pi32v2", feature(asm_experimental_arch))]
pub mod contract;
pub mod formats;
pub mod hw;
pub mod platform;
// The simulated chip doesn't model the card's bus: its tests stand in for this.
#[cfg(not(boot_sim))]
pub mod sdboot;
// The simulated chip's tests have their own MIDI mode.
#[cfg(not(boot_sim))]
pub mod midi_update;

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
