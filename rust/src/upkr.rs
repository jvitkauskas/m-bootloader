// SPDX-License-Identifier: MIT
//! Unpacks stage 1: Upkr with two parity contexts, after upkr's c_unpacker (public domain, Dennis Ranke), then the
//! call filter of tools/pack_stage1.py. Input and output are bounded, so a bad stream can't run away: docs/SPEC.md
//! §4.4.
//!
//! The self-extracting installer unpacks in place: its pack lies inside the room it unpacks into (src/sfx.rs). So the
//! output and the input are slices of cells, which may share bytes, and not a `&mut [u8]` and a `&[u8]`.
use core::cell::Cell;

/// Bytes that another slice may also reach.
pub type Bytes = [Cell<u8>];
pub const HEADER: usize = 8;
pub const PROBS: usize = 641;
const HAS_OFFSET: usize = 512;
const OFFSET: usize = 513;
const LENGTH: usize = 577;
/// The bits of a number at most: `number()` reads contexts up to `2 * NUMBER_BITS` past its first.
const NUMBER_BITS: u32 = 31;
// Every context is inside the table, so no stream can make an index into it fail: the last is 639.
const _: () = assert!(
    LENGTH + 2 * (NUMBER_BITS as usize) == PROBS - 2
        && OFFSET + 2 * (NUMBER_BITS as usize) < LENGTH
);

struct Upkr<'a> {
    input: &'a Bytes,
    at: usize,
    probs: &'a mut [u8; PROBS],
    state: u32,
}

impl Upkr<'_> {
    fn bit(&mut self, context: usize) -> u32 {
        while self.state < 4096 {
            match self.input.get(self.at) {
                Some(b) => {
                    self.state = self.state << 8 | b.get() as u32;
                    self.at += 1;
                }
                None => self.state = 4096,
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
            if at >= NUMBER_BITS {
                return 0;
            }
            n |= self.bit(context + 1) << at;
            at += 1;
            context += 2;
        }
        n | 1 << at
    }
}

/// The bytes produced, or `usize::MAX` if the stream is bad or `out` is too small for it.
fn unpack(out: &Bytes, input: &Bytes, probs: &mut [u8; PROBS]) -> usize {
    for p in probs.iter_mut() {
        *p = 128;
    }
    let mut s = Upkr {
        input,
        at: 0,
        probs,
        state: 0,
    };
    let (mut at, mut offset, mut after_match) = (0usize, 0, false);
    loop {
        let base = (at & 1) << 8;
        if s.bit(base) != 0 {
            if after_match || s.bit(HAS_OFFSET) != 0 {
                let n = s.number(OFFSET);
                if n == 1 {
                    return at;
                }
                offset = n.wrapping_sub(1) as usize;
            }
            let n = s.number(LENGTH) as usize;
            if offset == 0 || offset > at || n == 0 || n > out.len() - at {
                return usize::MAX;
            }
            // From `offset` back, byte by byte: the two may overlap.
            let Some(window) = out.get(at - offset..at + n) else {
                return usize::MAX;
            };
            for (from, to) in window.iter().zip(&window[offset..]) {
                to.set(from.get());
            }
            at += n;
            after_match = true;
        } else {
            let mut c = 1;
            while c < 256 {
                c = c << 1 | s.bit(base + c) as usize;
            }
            let Some(to) = out.get(at) else {
                return usize::MAX;
            };
            to.set(c as u8);
            at += 1;
            after_match = false;
        }
    }
}

/// Unpacks a packed stage 1 (docs/SPEC.md §4.4) to the start of `out`. False if anything is wrong with it. `probs` is
/// scratch. `out` and `pack` may share bytes, where the pack is never written over before it is read.
pub fn unpack_stage1(out: &Bytes, pack: &Bytes, probs: &mut [u8; PROBS]) -> bool {
    let Some((header, stream)) = pack.split_first_chunk::<HEADER>() else {
        return false;
    };
    let field = |at: usize| header[at].get() as usize | (header[at + 1].get() as usize) << 8;
    let (packed, size, want) = (field(0), field(2), field(4) as u32);
    let (Some(stream), Some(out)) = (stream.get(..packed), out.get(..size)) else {
        return false;
    };
    if unpack(out, stream, probs) != size {
        return false;
    }
    // A relative call's target, in halfwords, was made absolute.
    let mut i = 0;
    while let Some([a, b, low, high]) = out.get(i..i + 4) {
        if a.get() == 0xbf && b.get() == 0xea {
            let v = (low.get() as u32 | (high.get() as u32) << 8).wrapping_sub(i as u32 >> 1);
            low.set(v as u8);
            high.set((v >> 8) as u8);
            i += 4;
        } else {
            i += 2;
        }
    }
    let mut crc = 0u32;
    for b in out {
        crc ^= (b.get() as u32) << 8;
        for _ in 0..8 {
            crc = (if crc & 0x8000 != 0 {
                crc << 1 ^ 0x1021
            } else {
                crc << 1
            }) & 0xffff;
        }
    }
    crc == want
}
