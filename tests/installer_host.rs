// SPDX-License-Identifier: MIT
//! What the installer does to the flash, built for this computer from the code the chip runs
//! (rust/src/formats.rs), for tests/fwsc_test.py:
//!   installer-host install IMAGE CONTAINER OUT   boot_install on IMAGE's boot area
//!   installer-host decode IMAGE REQUEST END OUT  updater_decode on IMAGE with a raw 80-byte REQUEST
//!   installer-host retire IMAGE END OUT          request_retire for the stream ending at END
use mboot::formats::{
    Flash, Header, Nor, Request, Updater, boot_install, header_read, range, request_retire,
    updater_decode,
};
use std::cell::Cell;
use std::ffi::{CStr, OsStr, OsString, c_char, c_int};
use std::fs::File;
use std::io::{Error, Read, Write, stderr};
use std::os::unix::ffi::OsStrExt;
use std::process::exit;

const CAPACITY: usize = 0x10_0000;
// Its own copies, so that a wrong value in the crate shows in the tests that run this and is not followed.
const WORK_SIZE: usize = 4096;
const UPDATE_LOAD: u32 = 0x01c0_a800;
const UPDATE_MAX: u32 = 0x5b1c;
/// The most of a container that is read.
const CONTAINER: usize = 0x4000;

unsafe extern "C" {
    fn strerror(errnum: c_int) -> *const c_char;
}

/// Says what went wrong with a file, as C's perror says it, and ends the program.
fn fail(path: &OsStr, e: Error) -> ! {
    let mut line = Vec::new();
    if !path.is_empty() {
        line.extend_from_slice(path.as_bytes());
        line.extend_from_slice(b": ");
    }
    match e.raw_os_error() {
        // Safety: the C library's text for the error, which is read at once.
        Some(code) => line.extend_from_slice(unsafe { CStr::from_ptr(strerror(code)) }.to_bytes()),
        None => line.extend_from_slice(e.to_string().as_bytes()),
    }
    line.push(b'\n');
    let _ = stderr().write_all(&line);
    exit(2)
}

/// What a file holds, up to `limit` bytes of it.
fn load(path: &OsStr, limit: usize) -> Vec<u8> {
    let file = File::open(path).unwrap_or_else(|e| fail(path, e));
    let mut data = Vec::new();
    file.take(limit as u64)
        .read_to_end(&mut data)
        .unwrap_or_else(|e| fail(path, e));
    data
}

fn save(path: &OsStr, data: &[u8]) {
    if let Err(e) = File::create(path).and_then(|mut f| f.write_all(data)) {
        fail(path, e)
    }
}

/// The flash: an image of it.
struct Image<'a>(&'a [Cell<u8>]);

impl<'a> Image<'a> {
    fn of(flash: &'a mut [u8]) -> Self {
        Image(Cell::from_mut(flash).as_slice_of_cells())
    }
}

impl Flash for Image<'_> {
    fn capacity(&self) -> u32 {
        CAPACITY as u32
    }

    fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool {
        for (to, from) in dst.iter_mut().zip(&self.0[address as usize..]) {
            *to = from.get();
        }
        true
    }
}

impl Nor for Image<'_> {
    /// NOR: programming only clears bits.
    fn program(&self, address: u32, src: &[u8]) -> bool {
        if !range(address, src.len() as u32, CAPACITY as u32) {
            return false;
        }
        for (to, from) in self.0[address as usize..].iter().zip(src) {
            to.set(to.get() & from);
        }
        true
    }

    fn erase(&self, _: u32, _: u32) -> bool {
        unreachable!("nothing that is run here erases")
    }
}

/// A number as C's strtoul reads it in base 0, and the 32 bits of it that are kept: white space, a sign, 0x for
/// hexadecimal or 0 for octal, then the digits, up to what isn't one. No digits is 0, and too many is all ones.
fn number(mut s: &[u8]) -> u32 {
    while let [b' ' | b'\t'..=b'\r', rest @ ..] = s {
        s = rest;
    }
    let negative = s.first() == Some(&b'-');
    if let [b'-' | b'+', rest @ ..] = s {
        s = rest;
    }
    let base = match s {
        [b'0', b'x' | b'X', digit, ..] if digit.is_ascii_hexdigit() => {
            s = &s[2..];
            16
        }
        [b'0', ..] => 8,
        _ => 10,
    };
    let mut value = Some(0u64);
    for digit in s.iter().map_while(|&c| (c as char).to_digit(base)) {
        value = value.and_then(|v| v.checked_mul(base as u64)?.checked_add(digit as u64));
    }
    match value {
        Some(v) if negative => v.wrapping_neg() as u32,
        Some(v) => v as u32,
        None => u32::MAX,
    }
}

fn main() {
    let args: Vec<OsString> = std::env::args_os().collect();
    if args.len() == 5 && args[1] == "install" {
        let mut flash = load(&args[2], CAPACITY);
        if flash.len() != CAPACITY {
            exit(2);
        }
        let container = load(&args[3], CONTAINER);
        if !boot_install(
            flash.first_chunk_mut().unwrap(),
            &container,
            CAPACITY as u32,
        ) {
            exit(1);
        }
        save(&args[4], &flash);
        return;
    }
    if args.len() == 6 && args[1] == "decode" {
        let mut flash = load(&args[2], CAPACITY + 1);
        let Ok(request) = Request::try_from(load(&args[3], 81)) else {
            exit(2);
        };
        if flash.len() != CAPACITY {
            exit(2);
        }
        let f = Image::of(&mut flash);
        let (mut out, mut work) = (vec![0; UPDATE_MAX as usize], [0; WORK_SIZE]);
        let mut u = Updater {
            load: 0,
            size: 0,
            stream_end: 0,
            crc: 0,
        };
        if !updater_decode(&f, &request, &mut out, &mut work, &mut u)
            || u.load != UPDATE_LOAD
            || u.stream_end != number(args[4].as_bytes())
        {
            exit(1);
        }
        save(&args[5], &out[..u.size as usize]);
        return;
    }
    if args.len() == 5 && args[1] == "retire" {
        let mut flash = load(&args[2], CAPACITY);
        if flash.len() != CAPACITY {
            exit(2);
        }
        let nor = Image::of(&mut flash);
        let mut header = Header::EMPTY;
        if !header_read(&nor, &mut header)
            || request_retire(&nor, header.alignment, number(args[3].as_bytes())) == 0
        {
            exit(1);
        }
        save(&args[4], &flash);
        return;
    }
    eprintln!(
        "usage: installer-host install IMAGE CONTAINER OUT | decode IMAGE REQUEST END OUT | retire IMAGE END OUT"
    );
    exit(2);
}
