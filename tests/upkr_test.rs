// SPDX-License-Identifier: MIT
//! Stage 1's unpacker, built for this computer: it must give back stage 1 exactly, and no damage to the pack may
//! overrun, hang or be accepted. Run on every build: upkr-test <pack> <what it unpacks to>
use mboot::upkr::unpack_stage1;
use std::cell::Cell;
use std::process::exit;

const ROOM: usize = 0x8000;
// The pack's header and the unpacker's scratch (docs/SPEC.md §4.4): the test's own copies.
const HEADER: usize = 8;
const PROBS: usize = 641;

fn load(path: &str) -> Vec<u8> {
    let mut data = std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("{path}: {e}");
        exit(1)
    });
    data.truncate(ROOM);
    data
}

/// `pack` unpacked into `out`, whose length is the room it is given.
fn unpack(out: &mut [u8], pack: &[u8]) -> bool {
    let mut pack = pack.to_vec();
    unpack_stage1(
        Cell::from_mut(out).as_slice_of_cells(),
        Cell::from_mut(&mut pack[..]).as_slice_of_cells(),
        &mut [0; PROBS],
    )
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        exit(2);
    }
    let (name, mut pack, stage1) = (&args[1], load(&args[1]), load(&args[2]));
    let (size, want) = (pack.len(), stage1.len());
    let mut out = vec![0; ROOM];
    if !unpack(&mut out, &pack) || out[..want] != stage1[..] {
        eprintln!("{name}: stage 1 did not come back identical");
        exit(1);
    }
    // Too little room, or a pack cut short.
    if unpack(&mut out[..want - 1], &pack)
        || unpack(&mut out, &pack[..HEADER - 1])
        || unpack(&mut out, &pack[..size - 1])
    {
        eprintln!("{name}: accepted a bad size");
        exit(1);
    }
    // A header that claims less than the stream holds: nothing may be written past what it claims.
    let claimed = want / 2;
    let held = [pack[2], pack[3]];
    pack[2] = claimed as u8;
    pack[3] = (claimed >> 8) as u8;
    out.fill(0x5a);
    if unpack(&mut out, &pack) {
        eprintln!("{name}: accepted a short size");
        exit(1);
    }
    if let Some(at) = (claimed..ROOM).find(|&at| out[at] != 0x5a) {
        eprintln!("{name}: wrote past the size, at {at}");
        exit(1);
    }
    pack[2] = held[0];
    pack[3] = held[1];
    // Every byte damaged in turn, and truncated streams. A wrong result must never be accepted.
    let (mut accepted, mut refused) = (0u32, 0u32);
    for at in 0..size {
        let mut mask = 1u32;
        while mask <= 0x80 {
            pack[at] ^= mask as u8;
            if unpack(&mut out, &pack) {
                accepted += 1;
                if at >= HEADER && out[..want] != stage1[..] {
                    eprintln!("{name}: damage at {at} accepted with wrong output");
                    exit(1);
                }
            } else {
                refused += 1;
            }
            pack[at] ^= mask as u8;
            mask <<= 3;
        }
    }
    let mut cut = HEADER;
    while cut < size {
        let saved = [pack[0], pack[1]];
        pack[0] = (cut - HEADER) as u8;
        pack[1] = ((cut - HEADER) >> 8) as u8;
        if unpack(&mut out, &pack[..cut]) {
            eprintln!("{name}: accepted a stream cut at {cut}");
            exit(1);
        }
        pack[0] = saved[0];
        pack[1] = saved[1];
        cut += 7;
    }
    println!(
        "{name}: {size} bytes unpack to {want}; damage refused {refused} times, harmless {accepted} times"
    );
}
