// SPDX-License-Identifier: MIT
//! Unpacks stage 1: Upkr with two parity contexts, after upkr's c_unpacker (public domain, Dennis Ranke), then the
//! call filter of tools/pack_stage1.py. Input and output are bounded, so a bad stream can't run away: docs/SPEC.md
//! §4.4.
//!
//! The self-extracting installer unpacks in place: its pack lies inside the room it unpacks into (src/sfx.rs). So the
//! output and the input are reached through pointers, with the bounds checked by hand, and not as two slices.
pub const HEADER: usize = 8;
pub const PROBS: usize = 641;
const HAS_OFFSET: usize = 512;
const OFFSET: usize = 513;
const LENGTH: usize = 577;

struct Upkr<'a> {
    input: *const u8,
    end: *const u8,
    probs: &'a mut [u8; PROBS],
    state: u32,
}

impl Upkr<'_> {
    fn bit(&mut self, context: usize) -> u32 {
        while self.state < 4096 {
            if self.input == self.end {
                self.state = 4096;
            } else {
                // Safety: inside the pack, as it isn't at its end.
                unsafe {
                    self.state = self.state << 8 | *self.input as u32;
                    self.input = self.input.add(1);
                }
            }
        }
        let prob = self.probs[context] as u32;
        let (low, high) = (self.state & 255, self.state >> 8);
        if low < prob {
            self.state = prob * high + low;
            self.probs[context] = (prob + ((256 - prob + 8) >> 4)) as u8;
            1
        } else {
            self.state = (256 - prob) * high + low - prob;
            self.probs[context] = (prob - ((prob + 8) >> 4)) as u8;
            0
        }
    }

    /// 0 if it doesn't fit: no real number is 0.
    fn number(&mut self, mut context: usize) -> u32 {
        let (mut n, mut at) = (0, 0);
        while self.bit(context) != 0 {
            if at > 30 {
                return 0;
            }
            n |= self.bit(context + 1) << at;
            at += 1;
            context += 2;
        }
        n | 1 << at
    }
}

/// The bytes produced, or `u32::MAX` if the stream is bad.
///
/// # Safety
/// `out` has room for `capacity` bytes, and `input` to `end` can be read.
unsafe fn unpack(out: *mut u8, capacity: u32, input: *const u8, end: *const u8, probs: &mut [u8; PROBS]) -> u32 {
    for p in probs.iter_mut() {
        *p = 128;
    }
    let mut s = Upkr { input, end, probs, state: 0 };
    let (mut at, mut offset, mut after_match) = (0u32, 0, false);
    loop {
        let base = (at as usize & 1) << 8;
        if s.bit(base) != 0 {
            if after_match || s.bit(HAS_OFFSET) != 0 {
                let n = s.number(OFFSET);
                if n == 1 {
                    return at;
                }
                offset = n.wrapping_sub(1);
            }
            let mut n = s.number(LENGTH);
            if offset == 0 || offset > at || n == 0 || n > capacity - at {
                return u32::MAX;
            }
            while n != 0 {
                // Safety: at + n <= capacity and offset <= at, both checked above.
                unsafe { *out.add(at as usize) = *out.add((at - offset) as usize) };
                at += 1;
                n -= 1;
            }
            after_match = true;
        } else {
            let mut c = 1;
            while c < 256 {
                c = c << 1 | s.bit(base + c) as usize;
            }
            if at == capacity {
                return u32::MAX;
            }
            // Safety: at < capacity.
            unsafe { *out.add(at as usize) = c as u8 };
            at += 1;
            after_match = false;
        }
    }
}

/// Unpacks a packed stage 1 (docs/SPEC.md §4.4) to `out`, which has room for `capacity` bytes. False if anything is
/// wrong with it. `probs` is scratch.
///
/// # Safety
/// `out` has room for `capacity` bytes and `pack` holds `pack_size`. They may overlap, where the pack is never
/// written over before it is read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn stage1_unpack(out: *mut u8, capacity: u32, pack: *const u8, pack_size: u32, probs: &mut [u8; PROBS]) -> bool {
    if pack_size < HEADER as u32 {
        return false;
    }
    // Safety, to the end: the header is inside the pack, and everything else is checked against the sizes given.
    unsafe {
        let field = |at: usize| *pack.add(at) as u32 | (*pack.add(at + 1) as u32) << 8;
        let (packed, size, want) = (field(0), field(2), field(4));
        let stream = pack.add(HEADER);
        if packed > pack_size - HEADER as u32 || size > capacity || unpack(out, size, stream, stream.add(packed as usize), probs) != size {
            return false;
        }
        // A relative call's target, in halfwords, was made absolute.
        let mut i = 0;
        while i + 4 <= size {
            let p = out.add(i as usize);
            if *p == 0xbf && *p.add(1) == 0xea {
                let v = (*p.add(2) as u32 | (*p.add(3) as u32) << 8).wrapping_sub(i >> 1);
                *p.add(2) = v as u8;
                *p.add(3) = (v >> 8) as u8;
                i += 4;
            } else {
                i += 2;
            }
        }
        let mut crc = 0u32;
        for i in 0..size as usize {
            crc ^= (*out.add(i) as u32) << 8;
            for _ in 0..8 {
                crc = (if crc & 0x8000 != 0 { crc << 1 ^ 0x1021 } else { crc << 1 }) & 0xffff;
            }
        }
        crc == want
    }
}
