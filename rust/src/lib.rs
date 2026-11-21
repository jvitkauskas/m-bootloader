// SPDX-License-Identifier: MIT
//! m-bootloader in Rust: one crate, built once for each image with the `--cfg`s the Makefile gives it.
#![no_std]
pub mod formats;

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
