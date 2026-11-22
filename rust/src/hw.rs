// SPDX-License-Identifier: MIT
//! The chip's registers, and the two instructions Rust has no word for.

/// A 32-bit register of the chip.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct Reg(u32);

/// Where the chip has registers (docs/SPEC.md §2.2), and the cache's tags (§3.6): not its RAM, nor the flash.
const fn is_register(address: u32) -> bool {
    address.is_multiple_of(4) && matches!(address, 0x1_0000..0x2_0000 | 0x4_0000..0x6_0000 | 0x1ee_e000..0x1ee_f000 | 0x1f0_0000..0x1f0_c000)
}

#[cfg(platform_sim)]
unsafe extern "C" {
    /// The tests' chip.
    fn platform_register(address: usize) -> *mut u32;
}

impl Reg {
    /// The register at `ADDRESS`, which does not compile unless the chip has registers there.
    pub const fn at<const ADDRESS: u32>() -> Reg {
        const { assert!(is_register(ADDRESS)) };
        Reg(ADDRESS)
    }

    pub const fn address(self) -> u32 {
        self.0
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

/// A register of a port: A is 0, and so on.
#[inline(always)]
pub fn gpio(port: u32, offset: u32) -> Reg {
    Reg(0x50000 + 0x40 * port + offset)
}

/// The address the DMA is given for a pointer.
pub fn dma_address(p: *mut u8) -> u32 {
    #[cfg(boot_sim)]
    {
        unsafe extern "C" {
            fn platform_dma_address(pointer: *const core::ffi::c_void) -> u32;
        }
        return unsafe { platform_dma_address(p.cast()) };
    }
    #[cfg(not(boot_sim))]
    return p as u32;
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

/// `N` bytes of the retained area, which survives a reset (docs/SPEC.md §6.1), from `OFFSET`: inside the area, or this
/// does not compile.
pub fn retained<const OFFSET: usize, const N: usize>() -> &'static mut [u8; N] {
    const { assert!(OFFSET + N <= 0x280) };
    // Safety: RAM that is this program's to read and write. No caller keeps two of these at once.
    unsafe { &mut *((0x01c7_fd80 + OFFSET) as *mut [u8; N]) }
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

    /// # Safety
    /// A NUL-terminated string is at `address`, for as long as the program runs.
    pub const unsafe fn at(address: usize) -> Text {
        Text(core::ptr::NonNull::new(core::ptr::without_provenance_mut(address)).unwrap())
    }

    pub const fn as_ptr(self) -> *const u8 {
        self.0.as_ptr()
    }

    /// Without its NUL.
    pub fn as_bytes(self) -> &'static [u8] {
        unsafe { core::ffi::CStr::from_ptr(self.0.as_ptr().cast()) }.to_bytes()
    }
}

#[macro_export]
macro_rules! text {
    ($s:expr) => {
        const { $crate::hw::Text::new(concat!($s, "\0")) }
    };
}
