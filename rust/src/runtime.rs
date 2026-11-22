// SPDX-License-Identifier: MIT
//! What the compiler calls by name: copying, and division, which the chip has no instruction for. No vendor
//! compiler-runtime is used. Stage 0 has the ones it needs and stage 1 calls those through the linker (docs/SPEC.md
//! §4.4), adding the rest.
//!
//! The compiler turns a loop that copies or fills into a call to these very functions. So each loop here stores or
//! reads its bytes as volatile, which it leaves alone, and which costs nothing.

#[cfg(not(stage1))]
mod first {
    use core::ffi::c_void;
    use core::mem::MaybeUninit;

    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn memcpy(d: *mut c_void, s: *const c_void, n: usize) -> *mut c_void {
        let (dst, src) = (d.cast::<MaybeUninit<u8>>(), s.cast::<MaybeUninit<u8>>());
        for i in 0..n {
            // (As bytes that may not be initialised: the compiler copies structs with their padding.)
            unsafe { dst.add(i).write_volatile(src.add(i).read()) };
        }
        d
    }

    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn memset(d: *mut c_void, v: i32, n: usize) -> *mut c_void {
        for i in 0..n {
            unsafe { d.cast::<u8>().add(i).write_volatile(v as u8) };
        }
        d
    }

    /// Unsigned restoring division.
    #[unsafe(no_mangle)]
    pub extern "C" fn __udivsi3(a: u32, b: u32) -> u32 {
        let (mut q, mut r) = (0, 0u32);
        if b == 0 {
            return 0;
        }
        for i in (0..32).rev() {
            let top = r >> 31;
            r = r << 1 | a >> i & 1;
            if top != 0 || r >= b {
                r = r.wrapping_sub(b);
                q |= 1 << i;
            }
        }
        q
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __umodsi3(a: u32, b: u32) -> u32 {
        if b != 0 {
            a.wrapping_sub(__udivsi3(a, b).wrapping_mul(b))
        } else {
            0
        }
    }
}

#[cfg(stage1)]
unsafe extern "C" {
    safe fn __udivsi3(a: u32, b: u32) -> u32;
}
#[cfg(not(any(stage0, stage1)))]
use first::__udivsi3;

#[cfg(not(stage0))]
mod second {
    use super::__udivsi3;
    use core::ffi::{c_char, c_void};
    use core::mem::MaybeUninit;

    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn memmove(
        to: *mut c_void,
        from: *const c_void,
        n: usize,
    ) -> *mut c_void {
        let (d, s) = (to.cast::<MaybeUninit<u8>>(), from.cast::<MaybeUninit<u8>>());
        unsafe {
            if d as usize > s as usize {
                for i in (0..n).rev() {
                    d.add(i).write_volatile(s.add(i).read());
                }
            } else {
                for i in 0..n {
                    d.add(i).write_volatile(s.add(i).read());
                }
            }
        }
        to
    }

    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn memcmp(a: *const c_void, b: *const c_void, n: usize) -> i32 {
        for i in 0..n {
            let (x, y) = unsafe {
                (
                    a.cast::<u8>().add(i).read_volatile(),
                    b.cast::<u8>().add(i).read_volatile(),
                )
            };
            if x != y {
                return x as i32 - y as i32;
            }
        }
        0
    }

    /// The same, to those who only ask whether two are equal.
    ///
    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn bcmp(a: *const c_void, b: *const c_void, n: usize) -> i32 {
        unsafe { memcmp(a, b, n) }
    }

    /// # Safety
    /// As C's.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn strlen(s: *const c_char) -> usize {
        let mut n = 0;
        while unsafe { s.add(n).read_volatile() } != 0 {
            n += 1;
        }
        n
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __divsi3(a: i32, b: i32) -> i32 {
        let q = __udivsi3(a.unsigned_abs(), b.unsigned_abs());
        (if (a < 0) != (b < 0) {
            q.wrapping_neg()
        } else {
            q
        }) as i32
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __modsi3(a: i32, b: i32) -> i32 {
        if b != 0 {
            a.wrapping_sub(__divsi3(a, b).wrapping_mul(b))
        } else {
            0
        }
    }
}
