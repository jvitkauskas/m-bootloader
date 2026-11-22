// SPDX-License-Identifier: MIT
//! The self-extracting installer's layout (docs/SPEC.md §6.3), worked out with the unpacker the unit runs
//! (rust/src/upkr.rs), built for this computer:
//!   sfx-layout place installer.bin installer.pack   where the pack starts, as an address, so that unpacking it in
//!                                                   place at 0x01C0A800 never writes over what is still to be read
//!   sfx-layout check installer.bin sfx.bin pack end  the image as the unit gets it, unpacked as rust/src/sfx.rs does
//!                                                   it, from the linked sfx_pack to sfx_pack_end (addresses): the
//!                                                   installer must come back byte for byte
use mboot::upkr::{HEADER, PROBS, unpack_stage1};
use std::cell::Cell;
use std::process::exit;

const LOAD: usize = 0x01c0_a800;
/// The RAM a staged updater has, to 0x01C16800.
const WINDOW: usize = 0xc000;
/// More than the least gap that works.
const SAFETY: usize = 32;

fn load(path: &str) -> Vec<u8> {
    let data = std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("{path}: {e}");
        exit(2)
    });
    if data.len() > WINDOW {
        eprintln!("{path}: too big");
        exit(2);
    }
    data
}

/// What is in `ram`, with `pack` from `at` to `end`, unpacked in place as the unit does it: is it `installer`?
fn unpacks(ram: &mut [u8], at: usize, end: usize, installer: &[u8]) -> bool {
    let cells = Cell::from_mut(ram).as_slice_of_cells();
    unpack_stage1(&cells[..end], &cells[at..end], &mut [0; PROBS])
        && ram[..installer.len()] == *installer
}

/// The pack laid out with its end at the installer's size + gap, as the unit has it: does it unpack?
fn fits(installer: &[u8], pack: &[u8], gap: usize) -> bool {
    let end = installer.len() + gap;
    if end > WINDOW || pack.len() > end {
        return false;
    }
    let mut ram = vec![0; WINDOW];
    ram[end - pack.len()..end].copy_from_slice(pack);
    // (the room is the installer's size, as the header says, where the unit gives it all up to the pack's end)
    let cells = Cell::from_mut(&mut ram[..]).as_slice_of_cells();
    unpack_stage1(
        &cells[..installer.len()],
        &cells[end - pack.len()..end],
        &mut [0; PROBS],
    ) && ram[..installer.len()] == *installer
}

/// An address in the image's RAM, as given ("0x..."): its offset from LOAD, or past WINDOW when it isn't one.
fn offset(s: &str) -> usize {
    let value = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(hex) => usize::from_str_radix(hex, 16),
        None => s.parse(),
    };
    match value {
        Ok(v) if v >= LOAD && v - LOAD <= WINDOW => v - LOAD,
        _ => WINDOW + 1,
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 4 {
        exit(2);
    }
    let installer = load(&args[2]);
    let size = installer.len();
    if args[1] == "place" && args.len() == 4 {
        let pack = load(&args[3]);
        let mut gap = 0;
        while gap < 0x1000 && !fits(&installer, &pack, gap) {
            gap += 4;
        }
        if gap >= 0x1000 || !fits(&installer, &pack, gap + SAFETY) {
            eprintln!("{}: no place unpacks it in place", args[3]);
            exit(1);
        }
        // where the pack starts: its end SAFETY bytes past the least gap, the start word aligned
        let start = (size + gap + SAFETY - pack.len() + 3) & !3;
        println!("0x{:08x}", LOAD + start);
        return;
    }
    if args[1] == "check" && args.len() == 6 {
        let image = load(&args[3]);
        let (n, at, end) = (image.len(), offset(&args[4]), offset(&args[5]));
        if at > end || end > n {
            eprintln!(
                "{}: the pack {}-{} is not in the image",
                args[3], args[4], args[5]
            );
            exit(1);
        }
        // as rust/src/sfx.rs calls it: room to the pack's end, the pack from sfx_pack to sfx_pack_end
        let mut ram = vec![0; WINDOW];
        ram[..n].copy_from_slice(&image);
        if at + HEADER <= end
            && (image[at + 2] as usize | (image[at + 3] as usize) << 8) == size
            && unpacks(&mut ram, at, end, &installer)
        {
            println!(
                "{}: unpacks in place to {} ({} bytes, the pack at +{}, the image {} bytes)",
                args[3], args[2], size, at, n
            );
            return;
        }
        eprintln!("{}: does not unpack to {} in place", args[3], args[2]);
        exit(1);
    }
    exit(2);
}
