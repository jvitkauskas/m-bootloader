// SPDX-License-Identifier: MIT
//! The chip's registers, and the two instructions Rust has no word for.

/// A 32-bit register of the chip.
#[derive(Clone, Copy)]
pub struct Reg(u32);

#[cfg(platform_sim)]
unsafe extern "C" {
    /// The tests' chip.
    fn platform_register(address: usize) -> *mut u32;
}

impl Reg {
    /// # Safety
    /// `address` is a register of this chip (docs/SPEC.md §2.2).
    pub const unsafe fn at(address: u32) -> Reg {
        Reg(address)
    }

    /// The register `offset` bytes on, of the same peripheral.
    pub const fn add(self, offset: u32) -> Reg {
        Reg(self.0 + offset)
    }

    #[inline(always)]
    fn pointer(self) -> *mut u32 {
        #[cfg(platform_sim)]
        return unsafe { platform_register(self.0 as usize) };
        #[cfg(not(platform_sim))]
        return self.0 as *mut u32;
    }

    #[inline(always)]
    pub fn read(self) -> u32 {
        unsafe { self.pointer().read_volatile() }
    }

    #[inline(always)]
    pub fn write(self, value: u32) {
        unsafe { self.pointer().write_volatile(value) }
    }

    /// Reads it, and writes it back changed.
    #[inline(always)]
    pub fn modify(self, f: impl FnOnce(u32) -> u32) {
        let p = self.pointer();
        unsafe { p.write_volatile(f(p.read_volatile())) }
    }

    #[inline(always)]
    pub fn set(self, bits: u32) {
        self.modify(|v| v | bits)
    }

    #[inline(always)]
    pub fn clear(self, bits: u32) {
        self.modify(|v| v & !bits)
    }
}

pub const fn bit(n: u32) -> u32 {
    1 << n
}

/// Lets what was written reach the hardware before going on.
#[inline(always)]
pub fn sync_cpu() {
    #[cfg(target_arch = "pi32v2")]
    unsafe {
        core::arch::asm!("csync", options(nostack, preserves_flags))
    }
}

#[inline(always)]
pub fn irq_off() {
    #[cfg(target_arch = "pi32v2")]
    unsafe {
        core::arch::asm!("cli", options(nostack, preserves_flags))
    }
}

/// Text for the screen: NUL-terminated, as the C that is left takes it. One word, where a `&str` is two.
#[derive(Clone, Copy, PartialEq)]
#[repr(transparent)]
pub struct Text(core::ptr::NonNull<u8>);

unsafe impl Sync for Text {}

impl Text {
    /// `s` ends in a NUL, which `text!` adds.
    pub const fn new(s: &'static str) -> Text {
        assert!(!s.is_empty() && s.as_bytes()[s.len() - 1] == 0);
        Text(core::ptr::NonNull::new(s.as_ptr().cast_mut()).unwrap())
    }

    pub const fn as_ptr(self) -> *const u8 {
        self.0.as_ptr()
    }
}

#[macro_export]
macro_rules! text {
    ($s:expr) => {
        const { $crate::hw::Text::new(concat!($s, "\0")) }
    };
}
