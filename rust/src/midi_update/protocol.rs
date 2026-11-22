// SPDX-License-Identifier: MIT
//! The update protocol itself: see the module above.
use super::{Host, Outcome, SECTOR};
use crate::Global;
use crate::formats::{crc16, crc32, enc, le16, le32, put_le32};

/// The first address written, and the end: never the boot area, nor the key area at 0xFF000. On the SMK-37, not its
/// Bluetooth pairing and calibration either, at 0xC0000 to 0xC1FFF: a package's bytes for those are left unwritten.
const FIRST: u32 = 0x4000;
const LIMIT: u32 = 0xff000;
#[cfg(smk)]
const KEEP: core::ops::Range<u32> = 0xc0000..0xc2000;
#[cfg(not(smk))]
const KEEP: core::ops::Range<u32> = LIMIT..LIMIT;
#[cfg(smk)]
const IDENTITY: &[u8] = b"ota-SMK-37 Pro_900";
#[cfg(not(smk))]
const IDENTITY: &[u8] = b"ota-FM-1_900";
const APP_BOARD: u8 = if cfg!(smk) { 2 } else { 1 };
/// A RAM app, run with cmd 0x25: linked for the app's RAM, at most this many bytes, trailer included.
const APP_MAX: usize = 0x4fd80;
const FRAME_MAX: usize = 640;
const PACKET_MAX: usize = 512;
const TRIES: u32 = 5;
const TRY_MS: u32 = 2000;
const FINISH: u32 = 0xf000_0000;
const HEADER: usize = 0x400;
const ENTRY: usize = 0x50;
const MAX_SECTORS: usize = LIMIT as usize / SECTOR;
const RUN_MAGIC: u32 = 0x004e_5552; // "RUN"
const APP_MAGIC: u32 = 0x3150_5041; // "APP1"
const APP_FORMAT: u8 = 1;
const RUNS_APPS: u8 = 1;

fn app_ram(size: usize) -> &'static mut [u8] {
    // Safety: RAM that is the app's to be loaded into.
    unsafe { core::slice::from_raw_parts_mut(super::app_ram(), size) }
}

type Decoded = [u8; PACKET_MAX + 16];
type Sector = [u8; SECTOR];
type Crcs = [u16; MAX_SECTORS];

// What the receiver shares with the rest: mu_rx() runs inside the host's idle(), so nothing here is borrowed across a hook.
static FRAME: Global<[u8; FRAME_MAX]> = Global::new([0; FRAME_MAX]);
static FRAME_SIZE: Global<usize> = Global::new(0);
static COLLECTING: Global<bool> = Global::new(false);
static FRAME_READY: Global<bool> = Global::new(false);
static ERASING: Global<bool> = Global::new(false);
static DOWNLOAD_ASKED: Global<bool> = Global::new(false);
static DECODED: Global<Decoded> = Global::new([0; PACKET_MAX + 16]);
static WIRE: Global<[u8; FRAME_MAX]> = Global::new([0; FRAME_MAX]);
static SECTOR_CRC: Global<Crcs> = Global::new([0; MAX_SECTORS]);
/// How many messages have come from a host.
#[unsafe(no_mangle)]
#[allow(non_upper_case_globals)]
pub static mu_messages: Global<u32> = Global::new(0);

/// A byte from the USB-MIDI stream.
#[unsafe(no_mangle)]
pub extern "C" fn mu_rx(b: u8) {
    if b == 0xf0 {
        FRAME_SIZE.set(0);
        COLLECTING.set(!FRAME_READY.read());
    } else if b == 0xf7 {
        FRAME_READY.set(COLLECTING.read());
        COLLECTING.set(false);
    } else if COLLECTING.read() {
        let n = FRAME_SIZE.read();
        // Safety: the only other use of FRAME is in take(), which calls nothing.
        match unsafe { FRAME.get() }.get_mut(n) {
            Some(slot) => {
                *slot = b;
                FRAME_SIZE.set(n + 1);
            }
            None => COLLECTING.set(false),
        }
    }
}

/// Sends cmd with its body, 8-to-7 packed.
fn send_message<H: Host>(cmd: u8, body: &[u8]) {
    let size = body.len();
    let head = [0, 0x59, cmd, size as u8, (size >> 8) as u8, 0];
    // Safety: used only here, and mu_send() doesn't come back in.
    let wire = unsafe { WIRE.get() };
    let (mut sum, mut bits, mut n, mut at, mut accumulator) = (0u8, 0, 1, 0, 0u32);
    let total = size + 7;
    wire[0] = 0xf0;
    while at < total || bits != 0 {
        if bits < 7 && at < total {
            let byte = if at < 6 {
                head[at]
            } else if let Some(&b) = body.get(at - 6) {
                sum = sum.wrapping_add(b);
                b
            } else {
                !sum
            };
            at += 1;
            accumulator |= (byte as u32) << bits;
            bits += 8;
        }
        wire[n] = accumulator as u8 & 0x7f;
        n += 1;
        accumulator >>= 7;
        bits = if bits > 7 { bits - 7 } else { 0 };
    }
    wire[n] = 0xf7;
    H::send(&wire[..=n]);
}

fn send_identity<H: Host>() {
    let mut id = [0; 27];
    id[..IDENTITY.len()].copy_from_slice(IDENTITY);
    send_message::<H>(0x11, &id);
}

fn send_refusal<H: Host>() {
    send_message::<H>(0, &[1]);
}

/// The bytes of a frame, 7-to-8: how many, or 0 if they don't fit.
fn unpack(frame: &[u8], decoded: &mut Decoded) -> usize {
    let (mut out, mut bits, mut accumulator) = (0, 0, 0u32);
    for &b in frame {
        accumulator |= ((b & 0x7f) as u32) << bits;
        bits += 7;
        if bits >= 8 {
            let Some(slot) = decoded.get_mut(out) else {
                return 0;
            };
            *slot = accumulator as u8;
            out += 1;
            accumulator >>= 8;
            bits -= 8;
        }
    }
    out
}

#[derive(PartialEq)]
enum Kind {
    Nothing,
    /// Its size: decoded and checked.
    Message(usize),
    AskUpgrade,
    AskDownload,
    AskRestart,
}

/// The next message.
fn take(decoded: &mut Decoded) -> Kind {
    if !FRAME_READY.read() {
        return Kind::Nothing;
    }
    // Safety: nothing is called while this is borrowed.
    let frame = &unsafe { FRAME.get() }[..FRAME_SIZE.read()];
    let mut kind = Kind::Nothing;
    if let [0x22, 0x24, 0x35, key] = *frame {
        kind = match key {
            0x7f => Kind::AskUpgrade,
            0x7d => Kind::AskDownload,
            0x7e => Kind::AskRestart,
            _ => Kind::Nothing,
        };
    } else {
        let n = unpack(frame, decoded);
        if n >= 7
            && decoded[0] == 0
            && decoded[1] == 0x59
            && le16(decoded, 3) as usize == n - 7
            && decoded[5] == 0
        {
            let sum = decoded[6..n].iter().fold(0u8, |a, &b| a.wrapping_add(b));
            if sum == 0xff {
                kind = Kind::Message(n);
            }
        }
    }
    FRAME_READY.set(false);
    if kind != Kind::Nothing {
        mu_messages.set(mu_messages.read().wrapping_add(1));
    }
    kind
}

/// Asks the host for the package's bytes from `address`, to fill `out`.
fn ask<H: Host>(address: u32, out: &mut [u8], decoded: &mut Decoded) -> bool {
    let size = out.len();
    let a = address.to_le_bytes();
    let request = [0, a[0], a[1], a[2], a[3], size as u8, (size >> 8) as u8, 0];
    for _ in 0..TRIES {
        send_message::<H>(0x30, &request);
        for _ in 0..TRY_MS {
            H::idle();
            let kind = take(decoded);
            // Until something is erased, the key and the soft key still get out.
            if kind == Kind::AskDownload {
                DOWNLOAD_ASKED.set(true);
                return false;
            }
            if !ERASING.read() && H::cancel() {
                return false;
            }
            let Kind::Message(n) = kind else { continue };
            if decoded[2] == 0x11 && n == 7 {
                send_identity::<H>();
            } else if decoded[2] == 0x30
                && n == size + 15
                && le32(decoded, 7) == address
                && le16(decoded, 11) as usize == size
            {
                out.copy_from_slice(&decoded[14..14 + size]);
                return true;
            }
        }
    }
    false
}

fn ask_parts<H: Host>(mut offset: u32, out: &mut [u8], decoded: &mut Decoded) -> bool {
    for part in out.chunks_mut(PACKET_MAX) {
        if !ask::<H>(offset, part, decoded) {
            return false;
        }
        offset += PACKET_MAX as u32;
    }
    true
}

/// flash.bin in the package.
struct Package {
    offset: u32,
    size: u32,
    crc: u16,
}

/// Finds it from the package's header.
fn find_flash<H: Host>(
    sector: &mut Sector,
    decoded: &mut Decoded,
    package: &mut Package,
) -> Outcome {
    let (header, _) = sector.split_first_chunk_mut::<HEADER>().unwrap();
    if !ask_parts::<H>(0, header, decoded) {
        return Outcome::Lost;
    }
    enc(0xffff, &mut header[..0x40]);
    let count = le16(header, 8) as usize;
    if count == 0
        || count > 11
        || crc16(0, &header[2..0x40]) != le16(header, 0)
        || crc16(0, &header[0x40..0x40 + count * ENTRY]) != le16(header, 2)
    {
        return Outcome::BadPackage;
    }
    for entry in header[0x40..].chunks_exact_mut(ENTRY).take(count) {
        enc(0xffff, entry);
        let entry: &[u8; ENTRY] = (&*entry).try_into().unwrap();
        if le16(entry, 0) == 0 {
            *package = Package {
                offset: le32(entry, 8),
                size: le32(entry, 12),
                crc: le16(entry, 4),
            };
            if package.size <= FIRST || package.size > LIMIT || package.size % SECTOR as u32 != 0 {
                return Outcome::BadPackage;
            }
            return Outcome::Done;
        }
    }
    Outcome::BadPackage
}

/// The package's firmware must decrypt with this chip's key, and every byte must arrive intact.
fn check_flash<H: Host>(
    sector: &mut Sector,
    decoded: &mut Decoded,
    crcs: &mut Crcs,
    package: &Package,
    key: u16,
) -> Outcome {
    let line: &mut [u8; 32] = sector.first_chunk_mut().unwrap();
    if !ask::<H>(package.offset + FIRST, line, decoded) {
        return Outcome::Lost;
    }
    enc(key, line);
    if crc16(0, &line[2..]) != le16(line, 0) {
        return Outcome::WrongKey;
    }
    let (mut whole, mut at) = (0, 0);
    while at < package.size {
        if !ask_parts::<H>(package.offset + at, sector, decoded) {
            return Outcome::Lost;
        }
        whole = crc16(whole, sector);
        crcs[at as usize / SECTOR] = crc16(0, sector);
        H::progress(false, at * 100 / package.size);
        at += SECTOR as u32;
    }
    if whole == package.crc {
        Outcome::Done
    } else {
        Outcome::BadPackage
    }
}

fn same<H: Host>(mut at: u32, data: &Sector, decoded: &mut Decoded) -> bool {
    let got: &mut [u8; PACKET_MAX] = decoded.first_chunk_mut().unwrap();
    for part in data.chunks_exact(PACKET_MAX) {
        if !H::read(at, got) || got != part {
            return false;
        }
        at += PACKET_MAX as u32;
    }
    true
}

fn write_sector<H: Host>(
    offset: u32,
    at: u32,
    sector: &mut Sector,
    decoded: &mut Decoded,
    crcs: &Crcs,
    written: &mut bool,
) -> Outcome {
    if !ask_parts::<H>(offset + at, sector, decoded) {
        return Outcome::Lost;
    }
    if crc16(0, sector) != crcs[at as usize / SECTOR] {
        return Outcome::BadPackage;
    }
    if same::<H>(at, sector, decoded) {
        return Outcome::Done;
    }
    *written = true;
    if !H::erase(at) {
        return Outcome::FlashError;
    }
    let mut to = at;
    for part in sector.chunks_exact(PACKET_MAX) {
        if !H::program(to, part) {
            return Outcome::FlashError;
        }
        to += PACKET_MAX as u32;
    }
    if same::<H>(at, sector, decoded) {
        Outcome::Done
    } else {
        Outcome::FlashError
    }
}

fn install<H: Host>(
    key: u16,
    sector: &mut Sector,
    decoded: &mut Decoded,
    written: &mut bool,
) -> Outcome {
    // Safety: used only during an install, which is not entered twice.
    let crcs = unsafe { SECTOR_CRC.get() };
    let mut package = Package {
        offset: 0,
        size: 0,
        crc: 0,
    };
    let mut r = find_flash::<H>(sector, decoded, &mut package);
    if r == Outcome::Done {
        r = check_flash::<H>(sector, decoded, crcs, &package, key);
    }
    if r != Outcome::Done {
        return r;
    }
    // The firmware's first sector goes last: until then, what's in flash can't pass as firmware.
    H::progress(false, 100);
    H::progress(true, 0);
    *written = true;
    ERASING.set(true);
    if !H::erase(FIRST) {
        return Outcome::FlashError;
    }
    let mut at = FIRST + SECTOR as u32;
    while at < package.size {
        if !KEEP.contains(&at) {
            r = write_sector::<H>(package.offset, at, sector, decoded, crcs, written);
            if r != Outcome::Done {
                return r;
            }
            H::progress(true, at * 100 / package.size);
        }
        at += SECTOR as u32;
    }
    r = write_sector::<H>(package.offset, FIRST, sector, decoded, crcs, written);
    if r != Outcome::Done {
        return r;
    }
    H::progress(true, 100);
    // The host's "success" isn't needed: it only has to see that we finished.
    ask::<H>(FINISH, &mut sector[..8], decoded);
    Outcome::Done
}

/// What's answering: a bootloader or a RAM stage, not a firmware or its update loader.
fn send_info<H: Host>(key: u16) {
    let unit = H::unit_info();
    let mut info = [0; 64];
    *info.first_chunk_mut().unwrap() = *b"MBOOT\x01";
    info[6] = unit.kind;
    info[7] = RUNS_APPS;
    info[8] = key as u8;
    info[9] = (key >> 8) as u8;
    put_le32(&mut info, 10, unit.capacity);
    put_le32(&mut info, 14, unit.jedec);
    info[18..26].copy_from_slice(&unit.uid);
    let mut n = 28;
    for &b in unit.version.as_bytes().iter().take(info.len() - 1 - n) {
        info[n] = b;
        n += 1;
    }
    send_message::<H>(0x12, &info[..n + 1]);
}

/// Sends count bytes of raw flash from address, or the CRC-32 the chip computes of them. Both body layouts start with
/// what stock firmware's read uses: provider (0, the flash), address, count.
fn serve_read<H: Host>(cmd: u8, sector: &mut Sector, decoded: &mut Decoded, n: usize) {
    let capacity = H::unit_info().capacity;
    let (address, count) = (le32(decoded, 7), le32(decoded, 10) >> 8);
    if n != 15
        || decoded[6] != 0
        || count == 0
        || address >= capacity
        || count > capacity - address
        || (cmd == 0x23 && count as usize > PACKET_MAX)
    {
        return send_refusal::<H>();
    }
    decoded.copy_within(6..14, 0);
    if cmd == 0x23 {
        let end = 8 + count as usize;
        if !H::read(address, &mut decoded[8..end]) {
            return send_refusal::<H>();
        }
        return send_message::<H>(cmd, &decoded[..end]);
    }
    let (mut crc, mut at) = (u32::MAX, 0);
    while at < count {
        let part = &mut sector[..((count - at) as usize).min(SECTOR)];
        if !H::read(address + at, part) {
            return send_refusal::<H>();
        }
        crc = crc32(crc, part);
        H::idle();
        at += SECTOR as u32;
    }
    put_le32(decoded, 8, !crc);
    send_message::<H>(cmd, &decoded[..12]);
}

/// The trailer comes first, so a wrong app costs one request.
fn load_app<H: Host>(size: usize, decoded: &mut Decoded) -> bool {
    let mut trailer = [0; 8];
    if size < 16 || size > APP_MAX || !ask::<H>(size as u32 - 8, &mut trailer, decoded) {
        return false;
    }
    if le32(&trailer, 0) != APP_MAGIC
        || (trailer[6] != 0 && trailer[6] != APP_BOARD)
        || trailer[7] != APP_FORMAT
    {
        return false;
    }
    let ram = app_ram(size);
    if !ask_parts::<H>(0, ram, decoded) {
        return false;
    }
    // The file's own trailer, not only the one asked for first.
    let (body, end) = ram.split_at(size - 8);
    end == trailer && crc16(0, body) == le16(&trailer, 4)
}

/// The body is "RUN", a zero and the app's size. The answer echoes it, once the app is loaded.
fn run_app<H: Host>(n: usize, decoded: &mut Decoded) {
    let body: [u8; 8] = decoded[6..14].try_into().unwrap();
    if n != 15 || le32(&body, 0) != RUN_MAGIC || !load_app::<H>(le32(&body, 4) as usize, decoded) {
        return send_refusal::<H>();
    }
    send_message::<H>(0x25, &body);
    H::start_app();
    FRAME_READY.set(false);
    COLLECTING.set(false);
}

/// Answers the updater's protocol until a package is installed or the host's cancel() says stop. Whether flash was changed is
/// in `written`.
pub fn run<H: Host>(key: u16, sector: &mut Sector, written: &mut bool) -> Outcome {
    // Safety: the one use of it, for as long as this runs, and this is not entered twice.
    let decoded = unsafe { DECODED.get() };
    FRAME_READY.set(false);
    COLLECTING.set(false);
    ERASING.set(false);
    DOWNLOAD_ASKED.set(false);
    *written = false;
    loop {
        H::idle();
        match take(decoded) {
            Kind::AskUpgrade => {
                let r = install::<H>(key, sector, decoded, written);
                if DOWNLOAD_ASKED.read() {
                    return Outcome::Download;
                }
                // Stopped with the key, before anything was erased.
                return if r == Outcome::Lost && !*written && H::cancel() {
                    Outcome::Cancelled
                } else {
                    r
                };
            }
            Kind::AskDownload => return Outcome::Download,
            Kind::AskRestart => return Outcome::Restart,
            Kind::Message(n) => match decoded[2] {
                0x11 if n == 7 => send_identity::<H>(),
                0x12 if n == 7 => send_info::<H>(key),
                cmd @ (0x23 | 0x24) => serve_read::<H>(cmd, sector, decoded, n),
                0x25 => {
                    run_app::<H>(n, decoded);
                    if DOWNLOAD_ASKED.read() {
                        return Outcome::Download;
                    }
                }
                0x30 => {}
                _ => send_refusal::<H>(),
            },
            Kind::Nothing => {}
        }
        if H::cancel() {
            return Outcome::Cancelled;
        }
    }
}

/// The C interface (include/midi_update.h), for the tests: `sector` is SECTOR bytes of scratch, and the hooks are C's.
#[cfg(not(target_arch = "pi32v2"))]
mod c {
    use super::super::Unit;
    use super::*;
    use crate::text;
    use core::ffi::c_void;

    unsafe extern "C" {
        fn mu_send(data: *const u8, size: u32);
        safe fn mu_idle();
        safe fn mu_cancel() -> bool;
        fn mu_read(address: u32, out: *mut c_void, size: u32) -> bool;
        safe fn mu_erase(address: u32) -> bool;
        fn mu_program(address: u32, data: *const c_void, size: u32) -> bool;
        safe fn mu_progress(writing: bool, percent: u32);
        fn mu_unit_info(unit: *mut Unit);
        safe fn mu_start_app();
    }

    struct Hooks;

    impl Host for Hooks {
        fn send(message: &[u8]) {
            unsafe { mu_send(message.as_ptr(), message.len() as u32) }
        }

        fn idle() {
            mu_idle()
        }

        fn cancel() -> bool {
            mu_cancel()
        }

        fn read(address: u32, out: &mut [u8]) -> bool {
            unsafe { mu_read(address, out.as_mut_ptr().cast(), out.len() as u32) }
        }

        fn erase(address: u32) -> bool {
            mu_erase(address)
        }

        fn program(address: u32, data: &[u8]) -> bool {
            unsafe { mu_program(address, data.as_ptr().cast(), data.len() as u32) }
        }

        fn progress(writing: bool, percent: u32) {
            mu_progress(writing, percent)
        }

        fn unit_info() -> Unit {
            let mut unit = Unit {
                jedec: 0,
                capacity: 0,
                uid: [0; 8],
                kind: 0,
                version: text!(""),
            };
            // Safety: the hook fills it in, with a NUL-terminated string that stays.
            unsafe { mu_unit_info(&mut unit) };
            unit
        }

        fn start_app() {
            mu_start_app()
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn mu_run(key: u16, sector: &mut Sector, written: &mut bool) -> Outcome {
        run::<Hooks>(key, sector, written)
    }
}
