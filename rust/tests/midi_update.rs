// SPDX-License-Identifier: MIT
//! MIDI mode's protocol and flash rules, against a stand-in for a browser installer and the flash.
//! The stand-in frames its messages as web/fm1ota.js in the installers does.
use super::escape;
use crate::formats::{App, AppState, Flash, Header, app_check, app_mount, crc16, crc32, enc, le16};
use crate::midi_update::{self, Host, Outcome, Unit, mu_rx};
use crate::{Global, text};
use std::fs::File;
use std::io::Read;

const FLASH: usize = 0x100000;
const APP_SIZE: usize = 0x93000;
const PACKAGE_OFFSET: usize = 0x400;
const KEY: u16 = 0x980f;

// What docs/SPEC.md §11.6 says of the protocol, which the test has for itself.
const MU_SECTOR: usize = 0x1000;
const WORK_SIZE: usize = 4096;
/// The first address written, and the end: never the boot area, nor the key area at 0xFF000. On the SMK-37, not its
/// Bluetooth pairing and calibration either, at 0xC0000 to 0xC1FFF: a package's bytes for those are left unwritten.
const MU_FIRST: usize = 0x4000;
const MU_LIMIT: usize = 0xff000;
const MU_KEEP_FROM: usize = if cfg!(smk) { 0xc0000 } else { MU_LIMIT };
const MU_KEEP_TO: usize = if cfg!(smk) { 0xc2000 } else { MU_LIMIT };
/// With its closing zero.
const MU_IDENTITY: &[u8] = if cfg!(smk) {
    b"ota-SMK-37 Pro_900\0"
} else {
    b"ota-FM-1_900\0"
};
/// A RAM app, run with cmd 0x25: at most this many bytes, trailer included.
const MU_APP_MAX: usize = 0x4fd80;
/// What cmd 0x12 tells a host about what's answering, and what it can do.
const MU_STAGE: u8 = 2;
const MU_RUNS_APPS: u8 = 1;

/// The soft key: the message that asks for download mode.
const SOFT: [u8; 6] = [0xf0, 0x22, 0x24, 0x35, 0x7d, 0xf7];

struct State {
    flash: [u8; FLASH],
    original: [u8; FLASH],
    package: [u8; PACKAGE_OFFSET + FLASH],
    package_size: usize,
    // What the stand-ins saw.
    erases: u32,
    programs: u32,
    reads_served: u32,
    order: u32,
    last_program_order: [u32; FLASH / MU_SECTOR],
    first_erase_order: [u32; FLASH / MU_SECTOR],
    identity_seen: bool,
    finish_seen: bool,
    stop_answering: bool,
    cancel_now: bool,
    wrote_outside: bool,
    /// Where in flash.bin, if anywhere.
    corrupt_at_pass2: Option<usize>,
    pass2_seen: u32,
    identity: [u8; 32],
    // A RAM app the stand-in offers, and what happened to it.
    app: [u8; MU_APP_MAX],
    app_size: usize,
    app_requests: u32,
    apps_started: u32,
    app_stop_after: u32,
    app_cancel_after: u32,
    app_first_read_seen: bool,
    app_changes_trailer: bool,
    app_first_read: usize,
    answer_handshake: bool,
    handshake_sent: bool,
    cancel_after: u32,
    soft_after: u32,
    check_percent: u32,
    write_percent: u32,
    // What the host-initiated commands answered: the command of the answer, once there is one.
    answer: [u8; 700],
    asked: [u8; 1200],
    answered: Option<u8>,
    answer_size: usize,
    asked_size: usize,
    // A power cut: at flash operation cut_at, half of it happens, and then nothing else does.
    cut_at: u32,
    flash_ops: u32,
}

static STATE: Global<State> = Global::new(State {
    flash: [0; FLASH],
    original: [0; FLASH],
    package: [0; PACKAGE_OFFSET + FLASH],
    package_size: 0,
    erases: 0,
    programs: 0,
    reads_served: 0,
    order: 0,
    last_program_order: [0; FLASH / MU_SECTOR],
    first_erase_order: [0; FLASH / MU_SECTOR],
    identity_seen: false,
    finish_seen: false,
    stop_answering: false,
    cancel_now: false,
    wrote_outside: false,
    corrupt_at_pass2: None,
    pass2_seen: 0,
    identity: [0; 32],
    app: [0; MU_APP_MAX],
    app_size: 0,
    app_requests: 0,
    apps_started: 0,
    app_stop_after: 0,
    app_cancel_after: 0,
    app_first_read_seen: false,
    app_changes_trailer: false,
    app_first_read: 0,
    answer_handshake: false,
    handshake_sent: false,
    cancel_after: 0,
    soft_after: 0,
    check_percent: 0,
    write_percent: 0,
    answer: [0; 700],
    asked: [0; 1200],
    answered: None,
    answer_size: 0,
    asked_size: 0,
    cut_at: 0,
    flash_ops: 0,
});

/// The test's state. What this gives is not kept across a call of anything that takes it too: the stand-in's hooks
/// do, so a run does, and so do the test's own functions.
fn st() -> &'static mut State {
    // Safety: one thread at a time uses it, and (as said) through one reference at a time.
    unsafe { STATE.get() }
}

/// What the stand-in has for the unit: its own, so that what is queued can come from the state.
struct Reply {
    bytes: [u8; 1024],
    size: usize,
}

static REPLY: Global<Reply> = Global::new(Reply {
    bytes: [0; 1024],
    size: 0,
});

fn reply() -> &'static mut Reply {
    // Safety: used by queue_bytes() and the stand-in's idle(), neither inside the other, and to empty it.
    unsafe { REPLY.get() }
}

/// The sector of scratch that a run is given.
static SECTOR: Global<[u8; MU_SECTOR]> = Global::new([0; MU_SECTOR]);

/// Where the crate loads a RAM app, in this test (`app_ram()` in rust/src/midi_update.rs).
static APP_RAM: Global<[u8; MU_APP_MAX]> = Global::new([0; MU_APP_MAX]);

pub fn app_ram() -> *mut u8 {
    // Safety: only the address is taken.
    unsafe { APP_RAM.get() }.as_mut_ptr()
}

fn pack7(input: &[u8], out: &mut [u8]) -> usize {
    let (mut acc, mut bits, mut o) = (0u32, 0, 0);
    for &b in input {
        acc |= (b as u32) << bits;
        bits += 8;
        while bits >= 7 {
            out[o] = (acc & 0x7f) as u8;
            o += 1;
            acc >>= 7;
            bits -= 7;
        }
    }
    if bits != 0 {
        out[o] = (acc & 0x7f) as u8;
        o += 1;
    }
    o
}

fn unpack7(input: &[u8], out: &mut [u8]) -> usize {
    let (mut acc, mut bits, mut o) = (0u32, 0, 0);
    for &b in input {
        acc |= ((b & 0x7f) as u32) << bits;
        bits += 7;
        while bits >= 8 {
            out[o] = acc as u8;
            o += 1;
            acc >>= 8;
            bits -= 8;
        }
    }
    o
}

fn queue_bytes(data: &[u8]) {
    let reply = reply();
    for &b in data {
        reply.bytes[reply.size] = b;
        reply.size += 1;
    }
}

fn respond(address: u32, data: &[u8]) {
    let (mut body, mut framed) = ([0; 1024], [0; 1200]);
    let len = data.len();
    let total = len + 8;
    let head = [
        0,
        0x59,
        0x30,
        total as u8,
        (total >> 8) as u8,
        0,
        0,
        address as u8,
        (address >> 8) as u8,
        (address >> 16) as u8,
        (address >> 24) as u8,
        len as u8,
        (len >> 8) as u8,
        0,
    ];
    body[..14].copy_from_slice(&head);
    body[14..14 + len].copy_from_slice(data);
    let sum: u32 = body[6..14 + len].iter().map(|&b| b as u32).sum();
    body[14 + len] = !sum as u8;
    let n = pack7(&body[..15 + len], &mut framed[1..]);
    framed[0] = 0xf0;
    framed[n + 1] = 0xf7;
    queue_bytes(&framed[..n + 2]);
}

/// The stand-in for an installer and the flash: what the protocol is run with.
struct StandIn;

impl Host for StandIn {
    /// The device's message, as parseRequest and parseIdentity in fm1ota.js read it.
    fn send(data: &[u8]) {
        let s = st();
        let size = data.len();
        let mut u = [0; 700];
        assert!(data[0] == 0xf0 && data[size - 1] == 0xf7);
        let n = unpack7(&data[1..size - 1], &mut u);
        assert!(u[0] == 0 && u[1] == 0x59);
        if u[2] == 0x11 {
            assert!(n == 34);
            let sum: u32 = u[6..33].iter().map(|&b| b as u32).sum();
            assert!(!sum as u8 == u[33]);
            s.identity[..27].copy_from_slice(&u[6..33]);
            s.identity_seen = true;
            return;
        }
        if u[2] != 0x30 {
            let len = u[3] as usize | (u[4] as usize) << 8 | (u[5] as usize) << 16;
            assert!(len + 7 == n);
            let sum: u32 = u[6..n - 1].iter().map(|&b| b as u32).sum();
            assert!(!sum as u8 == u[n - 1]);
            s.answered = Some(u[2]);
            s.answer_size = len;
            s.answer[..len].copy_from_slice(&u[6..6 + len]);
            return;
        }
        assert!(n == 15);
        let sum: u32 = u[6..14].iter().map(|&b| b as u32).sum();
        assert!(!sum as u8 == u[14]);
        let address = u32::from_le_bytes([u[7], u[8], u[9], u[10]]);
        let len = u[11] as usize | (u[12] as usize) << 8 | (u[13] as usize) << 16;
        let at = address as usize;
        if s.stop_answering {
            return;
        }
        if s.app_size != 0 {
            assert!(len <= 512 && at + len <= s.app_size);
            if !s.app_first_read_seen {
                s.app_first_read_seen = true;
                s.app_first_read = at;
            }
            s.app_requests += 1;
            if s.app_stop_after != 0 && s.app_requests > s.app_stop_after {
                return;
            }
            if s.app_cancel_after != 0 && s.app_requests == s.app_cancel_after {
                queue_bytes(&SOFT);
                return;
            }
            respond(address, &s.app[at..at + len]);
            if s.app_changes_trailer && s.app_requests == 1 {
                s.app[s.app_size - 1] ^= 1;
            }
            return;
        }
        if address == 0xf000_0000 {
            s.finish_seen = true;
            respond(address, b"success\0");
            return;
        }
        assert!(len <= 512 && at + len <= s.package_size);
        s.reads_served += 1;
        if s.cancel_after != 0 && s.reads_served == s.cancel_after {
            s.cancel_now = true;
        }
        if s.soft_after != 0 && s.reads_served == s.soft_after {
            queue_bytes(&SOFT);
            return;
        }
        let mut chunk = [0; 512];
        chunk[..len].copy_from_slice(&s.package[at..at + len]);
        // A package that changes after the check: seen once it's being read for the second time.
        if s.corrupt_at_pass2
            .is_some_and(|corrupt| at == PACKAGE_OFFSET + corrupt)
        {
            s.pass2_seen += 1;
            if s.pass2_seen == 2 {
                chunk[3] ^= 1;
            }
        }
        respond(address, &chunk[..len]);
    }

    fn idle() {
        // One message at a time, as the real stream arrives.
        let reply = reply();
        if reply.size != 0 {
            for &b in &reply.bytes[..reply.size] {
                mu_rx(b);
            }
            reply.size = 0;
        }
    }

    /// Called on every pass of the run's loop, so it can send from inside.
    fn cancel() -> bool {
        const HS: [u8; 10] = [0xf0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7f, 0xf7];
        let s = st();
        if s.answer_handshake {
            s.answer_handshake = false;
            queue_bytes(&HS);
            s.handshake_sent = true;
            return false;
        }
        if s.handshake_sent && s.identity_seen {
            s.handshake_sent = false;
            return true;
        }
        if s.asked_size != 0 {
            queue_bytes(&s.asked[..s.asked_size]);
            s.asked_size = 0;
            return false;
        }
        if s.answered.is_some() {
            return true;
        }
        s.cancel_now
    }

    fn read(address: u32, out: &mut [u8]) -> bool {
        let (address, size) = (address as usize, out.len());
        assert!(address + size <= FLASH);
        out.copy_from_slice(&st().flash[address..address + size]);
        true
    }

    fn erase(address: u32) -> bool {
        let s = st();
        let address = address as usize;
        assert!(address.is_multiple_of(MU_SECTOR));
        note_write(s, address, MU_SECTOR);
        maybe_cut(s, address, None, MU_SECTOR);
        s.flash[address..address + MU_SECTOR].fill(0xff);
        s.erases += 1;
        if s.first_erase_order[address / MU_SECTOR] == 0 {
            s.order += 1;
            s.first_erase_order[address / MU_SECTOR] = s.order;
        }
        true
    }

    fn program(address: u32, data: &[u8]) -> bool {
        let s = st();
        let (address, size) = (address as usize, data.len());
        note_write(s, address, size);
        maybe_cut(s, address, Some(data), size);
        for (to, &b) in s.flash[address..address + size].iter_mut().zip(data) {
            *to &= b;
        }
        s.programs += 1;
        s.order += 1;
        s.last_program_order[address / MU_SECTOR] = s.order;
        true
    }

    fn progress(writing: bool, percent: u32) {
        let s = st();
        assert!(percent <= 100);
        // The check finishes before writing starts, and nothing is erased until it has.
        if writing {
            assert!(s.check_percent == 100);
            s.write_percent = percent;
        } else {
            assert!(s.erases == 0 && s.write_percent == 0);
            s.check_percent = percent;
        }
    }

    fn unit_info() -> Unit {
        Unit {
            jedec: 0x856014,
            capacity: FLASH as u32,
            uid: [1, 2, 3, 4, 5, 6, 7, 8],
            kind: MU_STAGE,
            version: text!("test"),
        }
    }

    fn start_app() {
        let s = st();
        s.apps_started += 1;
        // It really has what was offered, and a host hears before USB stops.
        // Safety: the run has loaded it, and has no reference to it left.
        let ram = unsafe { APP_RAM.get() };
        assert!(s.answered == Some(0x25) && ram[..s.app_size] == s.app[..s.app_size]);
    }
}

/// A run of the protocol with the stand-in as its host, and the test's sector of scratch. Whether flash was changed
/// is in `written`.
fn mu_run(key: u16, written: &mut bool) -> Outcome {
    // Safety: the hooks don't call it, and one run goes on at a time: one that a power cut ended uses nothing again.
    unsafe { midi_update::run::<StandIn>(key, SECTOR.get(), written) }
}

fn note_write(s: &mut State, address: usize, size: usize) {
    if address < MU_FIRST
        || address + size > MU_LIMIT
        || (address + size > MU_KEEP_FROM && address < MU_KEEP_TO)
    {
        s.wrote_outside = true;
    }
}

/// A power cut: at flash operation cut_at, half of it happens, and then nothing else does.
fn maybe_cut(s: &mut State, address: usize, data: Option<&[u8]>, size: usize) {
    if s.cut_at == 0 {
        return;
    }
    s.flash_ops += 1;
    if s.flash_ops != s.cut_at {
        return;
    }
    for i in 0..size / 2 {
        match data {
            Some(data) => s.flash[address + i] &= data[i],
            None => s.flash[address + i] = 0xff,
        }
    }
    escape::leave(1)
}

/// An M-UPGRADE package: its header, then flash.bin with the firmware encrypted for the key.
fn make_package(key: u16, app_size: usize) {
    let s = st();
    s.package.fill(0);
    let (h, f) = s.package.split_at_mut(PACKAGE_OFFSET);
    for (i, b) in f[..app_size].iter_mut().enumerate() {
        *b = (i * 31 + (i >> 9)) as u8;
    }
    let mut first = [0; 32];
    for (i, b) in first.iter_mut().enumerate() {
        *b = (i * 7) as u8;
    }
    let c = crc16(0, &first[2..]);
    first[0] = c as u8;
    first[1] = (c >> 8) as u8;
    enc(key, &mut first);
    f[MU_FIRST..MU_FIRST + 32].copy_from_slice(&first);
    let mut entry = [0; 0x50];
    let d = crc16(0, &f[..app_size]);
    entry[4] = d as u8;
    entry[5] = (d >> 8) as u8;
    entry[8] = (PACKAGE_OFFSET & 0xff) as u8;
    entry[9] = (PACKAGE_OFFSET >> 8) as u8;
    entry[12] = app_size as u8;
    entry[13] = (app_size >> 8) as u8;
    entry[14] = (app_size >> 16) as u8;
    h[..0x40].fill(0);
    h[8] = 1;
    // The entries' CRC is over them as stored, encrypted.
    enc(0xffff, &mut entry);
    h[0x40..0x90].copy_from_slice(&entry);
    let ce = crc16(0, &entry);
    h[2] = ce as u8;
    h[3] = (ce >> 8) as u8;
    let ch = crc16(0, &h[2..0x40]);
    h[0] = ch as u8;
    h[1] = (ch >> 8) as u8;
    enc(0xffff, &mut h[..0x40]);
    s.package_size = PACKAGE_OFFSET + app_size;
}

fn reset_all() {
    let s = st();
    for (i, b) in s.original.iter_mut().enumerate() {
        *b = (i * 13 + (i >> 12) + 0x5a) as u8;
    }
    s.flash.copy_from_slice(&s.original);
    (s.erases, s.programs, s.reads_served, s.order, s.pass2_seen) = (0, 0, 0, 0, 0);
    s.last_program_order.fill(0);
    s.first_erase_order.fill(0);
    s.identity_seen = false;
    s.finish_seen = false;
    s.stop_answering = false;
    s.cancel_now = false;
    s.wrote_outside = false;
    s.corrupt_at_pass2 = None;
    (s.cancel_after, s.soft_after) = (0, 0);
    (s.check_percent, s.write_percent) = (0, 0);
    reply().size = 0;
}

fn ask_upgrade() {
    const UPGRADE: [u8; 6] = [0xf0, 0x22, 0x24, 0x35, 0x7f, 0xf7];
    queue_bytes(&UPGRADE);
}

fn untouched() -> bool {
    let s = st();
    s.erases == 0 && s.programs == 0 && s.flash == s.original
}

fn outside_same(size: usize) -> bool {
    let s = st();
    s.flash[..MU_FIRST] == s.original[..MU_FIRST] && s.flash[size..] == s.original[size..]
}

/// The test's flash, for the bootloader's own checks.
struct CoreFlash;

impl Flash for CoreFlash {
    fn capacity(&self) -> u32 {
        FLASH as u32
    }

    fn read_inside(&self, address: u32, out: &mut [u8]) -> bool {
        let (address, size) = (address as usize, out.len());
        if address > FLASH || size > FLASH - address {
            return false;
        }
        out.copy_from_slice(&st().flash[address..address + size]);
        true
    }
}

/// Whether the bootloader would start what's in flash, by its own checks.
fn would_start(key: u16) -> bool {
    let (mut work, mut app, mut header) = ([0; WORK_SIZE], App::EMPTY, Header::EMPTY);
    let f = CoreFlash;
    app_mount(&f, key, &mut app, &mut header) && app_check(&f, &mut app, &mut work) == AppState::Ok
}

/// Cuts power at every flash operation in turn (every step-th, for a big image). Each time the boot and
/// key areas are intact, a part-written firmware can't be started, and installing again finishes it.
fn power_cuts(size: usize, key: u16, step: u32, real: bool) -> u32 {
    let old = st().flash.to_vec();
    let mut want = vec![0; FLASH];
    want[..size].copy_from_slice(&st().package[PACKAGE_OFFSET..PACKAGE_OFFSET + size]);
    let mut written = false;
    let mut tried = 0;
    let mut at = 1;
    loop {
        let s = st();
        s.flash.copy_from_slice(&old);
        s.cut_at = at;
        s.flash_ops = 0;
        (s.cancel_after, s.soft_after) = (0, 0);
        (s.check_percent, s.write_percent) = (0, 0);
        (s.erases, s.programs) = (0, 0);
        s.finish_seen = false;
        reply().size = 0;
        ask_upgrade();
        // The run is on a thread of its own, which the cut stops where it is: what it is given is the thread's.
        let run = escape::run(move || {
            let mut written = false;
            mu_run(key, &mut written)
        });
        if let Ok(r) = run {
            assert!(r == Outcome::Done);
            st().cut_at = 0;
            break; // the install finished before this cut
        }
        let s = st();
        s.cut_at = 0;
        tried += 1;
        assert!(s.flash[..MU_FIRST] == old[..MU_FIRST] && s.flash[MU_LIMIT..] == old[MU_LIMIT..]);
        if real {
            assert!(!would_start(key));
        } else {
            assert!(
                s.flash[MU_FIRST..MU_FIRST + MU_SECTOR] != want[MU_FIRST..MU_FIRST + MU_SECTOR]
            );
        }
        // Every so often, go on and finish it, from the flash as the cut left it.
        if tried % (if real { 40 } else { 1 }) == 0 {
            let s = st();
            s.finish_seen = false;
            (s.check_percent, s.write_percent, s.erases, s.programs) = (0, 0, 0, 0);
            reply().size = 0;
            ask_upgrade();
            assert!(mu_run(key, &mut written) == Outcome::Done && st().finish_seen);
            let s = st();
            assert!(s.flash[MU_FIRST..size] == want[MU_FIRST..size]);
            assert!(s.flash[..MU_FIRST] == old[..MU_FIRST]);
            if real {
                assert!(would_start(key));
            }
        }
        at += step;
    }
    tried
}

/// M-VAVE's packages aren't public: this part runs where they're in testdata.
fn load_fwsc(
    path: &str,
    out: &mut [u8],
    flash_offset: &mut usize,
    flash_size: &mut usize,
) -> usize {
    static RAW: Global<[u8; 0x200000]> = Global::new([0; 0x200000]);
    // Safety: used only here.
    let raw = unsafe { RAW.get() };
    let Ok(mut f) = File::open(path) else {
        return 0;
    };
    // What there is of it, as far as the buffer goes.
    let mut size = 0;
    while size < raw.len() {
        match f.read(&mut raw[size..]) {
            Ok(0) | Err(_) => break,
            Ok(n) => size += n,
        }
    }
    let (mut n, mut o) = (0, 0);
    while n < 20 || raw[n * 48 + 47] == 0x7d {
        n += 1;
    }
    for i in 0..n {
        out[o..o + 47].copy_from_slice(&raw[i * 48..i * 48 + 47]);
        o += 47;
    }
    out[o..o + size - n * 48].copy_from_slice(&raw[n * 48..size]);
    o += size - n * 48;
    let (mut head, mut entry) = ([0; 0x40], [0; 0x50]);
    head.copy_from_slice(&out[..0x40]);
    enc(0xffff, &mut head);
    for i in 0..(head[8] as usize | (head[9] as usize) << 8) {
        entry.copy_from_slice(&out[0x40 + i * 0x50..0x90 + i * 0x50]);
        enc(0xffff, &mut entry);
        if le16(&entry, 0) == 0 {
            *flash_offset =
                entry[8] as usize | (entry[9] as usize) << 8 | (entry[10] as usize) << 16;
            *flash_size =
                entry[12] as usize | (entry[13] as usize) << 8 | (entry[14] as usize) << 16;
        }
    }
    o
}

/// A host's request, framed as it would be, and the run it gets.
fn host_asks(cmd: u8, body: &[u8]) -> Outcome {
    let mut plain = [0; 64];
    let mut written = false;
    let size = body.len();
    let head = [0, 0x59, cmd, size as u8, 0, 0];
    plain[..6].copy_from_slice(&head);
    plain[6..6 + size].copy_from_slice(body);
    let sum: u32 = body.iter().map(|&b| b as u32).sum();
    plain[6 + size] = !sum as u8;
    let s = st();
    s.asked[0] = 0xf0;
    let n = pack7(&plain[..7 + size], &mut s.asked[1..]);
    s.asked[n + 1] = 0xf7;
    s.asked_size = n + 2;
    s.answered = None;
    let r = mu_run(KEY, &mut written);
    let s = st();
    assert!(!written && s.erases == 0 && s.programs == 0);
    r
}

fn make_app(size: usize, board: u8, format: u8) {
    let s = st();
    for (i, b) in s.app[..size - 8].iter_mut().enumerate() {
        *b = (i * 7 + (i >> 9)) as u8;
    }
    let crc = crc16(0, &s.app[..size - 8]);
    s.app[size - 8..size - 4].copy_from_slice(b"APP1");
    s.app[size - 4] = crc as u8;
    s.app[size - 3] = (crc >> 8) as u8;
    s.app[size - 2] = board;
    s.app[size - 1] = format;
    s.app_size = size;
    s.app_requests = 0;
    s.apps_started = 0;
    (s.app_stop_after, s.app_cancel_after) = (0, 0);
    s.app_first_read_seen = false;
    s.app_changes_trailer = false;
}

fn offer_app(magic: &[u8; 3], size: usize) -> Outcome {
    let m = [
        magic[0],
        magic[1],
        magic[2],
        0,
        size as u8,
        (size >> 8) as u8,
        (size >> 16) as u8,
        (size >> 24) as u8,
    ];
    st().answered = None;
    host_asks(0x25, &m)
}

fn test_apps() {
    reset_all();
    let (mine, other) = if cfg!(smk) { (2, 1) } else { (1, 2) };
    // It says it can.
    assert!(host_asks(0x12, &[]) == Outcome::Cancelled);
    assert!(st().answered == Some(0x12) && st().answer[7] == MU_RUNS_APPS);
    // Any size, for this board or any, reads the trailer first and runs the app.
    for size in [16, 17, 512, 513, 520, 0x8000, MU_APP_MAX] {
        for any in 0..2 {
            make_app(size, if any != 0 { 0 } else { mine }, 1);
            assert!(offer_app(b"RUN", size) == Outcome::Cancelled);
            let s = st();
            assert!(s.apps_started == 1 && s.app_first_read == size - 8);
            assert!(s.answer_size == 8 && s.answer[..4] == *b"RUN\0");
            assert!(s.app_requests as usize == 1 + (size + 511) / 512);
        }
    }
    // A wrong trailer costs that one request, and starts nothing.
    make_app(0x1000, other, 1);
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().answer[0] == 1
    );
    assert!(st().app_requests == 1);
    make_app(0x1000, mine, 2);
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 1
    );
    make_app(0x1000, mine, 1);
    st().app[0x1000 - 8] = b'S';
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 1
    );
    // A wrong CRC is only seen at the end.
    make_app(0x1000, mine, 1);
    st().app[0x123] ^= 1;
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().answer[0] == 1
    );
    // A trailer that isn't the one the file ends in.
    make_app(0x1000, mine, 1);
    st().app_changes_trailer = true;
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().answer[0] == 1
    );
    // A file with its CRC from outside this code, so the test can't share a mistake with it.
    make_app(16, 0, 1);
    st().app[..8].copy_from_slice(b"12345678");
    st().app[12] = 20;
    st().app[13] = 144;
    assert!(
        offer_app(b"RUN", 16) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
    );
    make_app(16, 0, 1);
    st().app[..8].copy_from_slice(b"12345678");
    st().app[12] = 21;
    st().app[13] = 144;
    assert!(
        offer_app(b"RUN", 16) == Outcome::Cancelled
            && st().apps_started == 1
            && st().answered == Some(0x25)
    );
    // Sizes outside the limits, and a request that isn't one, aren't asked about.
    make_app(0x1000, mine, 1);
    assert!(
        offer_app(b"RUN", 15) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 0
    );
    assert!(
        offer_app(b"RUN", MU_APP_MAX + 1) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 0
    );
    assert!(
        offer_app(b"RUX", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 0
    );
    let longer = [b'R', b'U', b'N', 0, 0, 0x10, 0, 0, 0];
    st().answered = None;
    assert!(
        host_asks(0x25, &longer) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
            && st().app_requests == 0
    );
    // A host that stops partway starts nothing.
    make_app(0x1000, mine, 1);
    st().app_stop_after = 3;
    assert!(
        offer_app(b"RUN", 0x1000) == Outcome::Cancelled
            && st().apps_started == 0
            && st().answered == Some(0)
    );
    // Download mode still gets out while loading.
    make_app(0x1000, mine, 1);
    st().app_cancel_after = 3;
    assert!(offer_app(b"RUN", 0x1000) == Outcome::Download && st().apps_started == 0);
    // Nothing here goes near the flash.
    let s = st();
    assert!(s.erases == 0 && s.programs == 0 && s.flash == s.original);
    s.app_size = 0;
    s.answered = None;
}

fn read_request(cmd: u8, provider: u8, address: u32, count: u32) {
    let m = [
        provider,
        address as u8,
        (address >> 8) as u8,
        (address >> 16) as u8,
        (address >> 24) as u8,
        count as u8,
        (count >> 8) as u8,
        (count >> 16) as u8,
    ];
    assert!(host_asks(cmd, &m) == Outcome::Cancelled);
}

fn test_host_commands() {
    reset_all();
    for (i, b) in st().flash.iter_mut().enumerate() {
        *b = (i * 13 + (i >> 8) + (i >> 16)) as u8;
    }
    // What's answering.
    assert!(host_asks(0x12, &[]) == Outcome::Cancelled);
    let (answered, answer) = (st().answered, &st().answer);
    assert!(
        answered == Some(0x12)
            && answer[..5] == *b"MBOOT"
            && answer[5] == 1
            && answer[6] == MU_STAGE
    );
    assert!(le16(answer, 8) == KEY && answer[28..33] == *b"test\0");
    assert!(answer[10] == 0 && answer[11] == 0 && answer[12] == 0x10);
    assert!(
        answer[14] == 0x14
            && answer[15] == 0x60
            && answer[16] == 0x85
            && answer[18] == 1
            && answer[25] == 8
    );
    // Raw flash, the boot area and the key area too, each with what was asked echoed.
    let places = [
        (0, 512),
        (0x3f00, 512),
        (0xff000, 512),
        (FLASH - 1, 1),
        (FLASH - 512, 512),
        (0x12345, 7),
    ];
    for (address, count) in places {
        read_request(0x23, 0, address as u32, count as u32);
        let s = st();
        assert!(s.answered == Some(0x23) && s.answer_size == 8 + count);
        assert!(s.answer[0] == 0 && s.answer[1] == address as u8 && s.answer[5] == count as u8);
        assert!(s.answer[8..8 + count] == s.flash[address..address + count]);
    }
    // Refused, with the common error.
    const END: u32 = FLASH as u32;
    let bad = [
        (0x23, 0, 0),
        (0x23, 0, 513),
        (0x23, END, 1),
        (0x23, END - 1, 2),
        (0x23, 0xffff_ff00, 512),
        (0x24, 0, 0),
        (0x24, END, 1),
        (0x24, 1, END),
        (0x24, 0xffff_fff0, 32),
    ];
    for (cmd, address, count) in bad {
        read_request(cmd, 0, address, count);
        let s = st();
        assert!(s.answered == Some(0) && s.answer_size == 1 && s.answer[0] == 1);
    }
    read_request(0x23, 1, 0, 16);
    assert!(st().answered == Some(0) && st().answer[0] == 1);
    // A body of another length isn't taken for a request.
    let longer = [0, 0, 0, 0, 0, 16, 0, 0, 0];
    assert!(host_asks(0x23, &longer) == Outcome::Cancelled);
    assert!(st().answered == Some(0) && st().answer[0] == 1);
    // The chip's CRC-32 of a range, across sectors.
    read_request(0x24, 0, 0, END);
    assert!(st().answered == Some(0x24) && st().answer_size == 12);
    let s = st();
    assert!(
        u32::from_le_bytes([s.answer[8], s.answer[9], s.answer[10], s.answer[11]])
            == crc32(u32::MAX, &s.flash) ^ u32::MAX
    );
    read_request(0x24, 0, 0x1234, 0x2345);
    let s = st();
    assert!(
        u32::from_le_bytes([s.answer[8], s.answer[9], s.answer[10], s.answer[11]])
            == crc32(u32::MAX, &s.flash[0x1234..0x1234 + 0x2345]) ^ u32::MAX
    );
    // The commands that erase and write on stock firmware are refused here.
    let erase = [0, 0, 0x40, 0, 0];
    let program = [0, 0, 0x40, 0, 0, 4, 0, 0, 1, 2, 3, 4];
    assert!(
        host_asks(0x21, &erase) == Outcome::Cancelled
            && st().answered == Some(0)
            && st().answer[0] == 1
    );
    assert!(
        host_asks(0x22, &program) == Outcome::Cancelled
            && st().answered == Some(0)
            && st().answer[0] == 1
    );
    // A restart.
    let mut written = false;
    let restart = [0xf0, 0x22, 0x24, 0x35, 0x7e, 0xf7];
    let s = st();
    s.answered = None;
    s.asked[..restart.len()].copy_from_slice(&restart);
    s.asked_size = restart.len();
    assert!(mu_run(KEY, &mut written) == Outcome::Restart && !written && st().erases == 0);
    st().answered = None;
}

#[test]
fn midi_update() {
    let mut written = false;
    test_host_commands();
    test_apps();
    make_package(KEY, APP_SIZE);
    // A clean install: only [0x4000, size) changes, and the first sector is finished last.
    reset_all();
    ask_upgrade();
    let r = mu_run(KEY, &mut written);
    let s = st();
    assert!(
        r == Outcome::Done
            && written
            && s.finish_seen
            && !s.wrote_outside
            && s.write_percent == 100
    );
    assert!(
        s.flash[MU_FIRST..APP_SIZE]
            == s.package[PACKAGE_OFFSET + MU_FIRST..PACKAGE_OFFSET + APP_SIZE]
    );
    assert!(outside_same(APP_SIZE));
    let first_at = MU_FIRST / MU_SECTOR;
    let s = st();
    assert!(s.first_erase_order[first_at] == 1);
    for sector in first_at + 1..APP_SIZE / MU_SECTOR {
        assert!(s.last_program_order[sector] < s.last_program_order[first_at]);
    }
    // Installing it again rewrites only the first sector, erased up front and again to write it.
    (s.erases, s.programs, s.check_percent, s.write_percent) = (0, 0, 0, 0);
    s.finish_seen = false;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::Done && st().finish_seen);
    let s = st();
    assert!(
        s.erases == 2
            && s.flash[MU_FIRST..APP_SIZE]
                == s.package[PACKAGE_OFFSET + MU_FIRST..PACKAGE_OFFSET + APP_SIZE]
    );
    // The wrong chip key: nothing erased.
    reset_all();
    make_package(0x1234, APP_SIZE);
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::WrongKey && !written && untouched());
    // Damage anywhere, found by the whole-package check before anything is erased.
    make_package(KEY, APP_SIZE);
    reset_all();
    st().package[PACKAGE_OFFSET + 0x20000] ^= 1;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && !written && untouched());
    // A bad header.
    make_package(KEY, APP_SIZE);
    reset_all();
    st().package[3] ^= 1;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && untouched());
    // Sizes that would reach what it must not write, or that aren't whole sectors.
    make_package(KEY, MU_LIMIT + MU_SECTOR);
    reset_all();
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && untouched());
    make_package(KEY, APP_SIZE + 1);
    reset_all();
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && untouched());
    make_package(KEY, MU_FIRST);
    reset_all();
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && untouched());
    // The largest package allowed stays below what must be kept.
    make_package(KEY, MU_LIMIT);
    reset_all();
    ask_upgrade();
    assert!(
        mu_run(KEY, &mut written) == Outcome::Done && !st().wrote_outside && outside_same(MU_LIMIT)
    );
    if cfg!(smk) {
        // The SMK-37's Bluetooth pairing and calibration stay out of reach, even in a package covering them.
        let s = st();
        assert!(s.flash[MU_KEEP_FROM..MU_KEEP_TO] == s.original[MU_KEEP_FROM..MU_KEEP_TO]);
        assert!(
            s.flash[MU_KEEP_TO..MU_LIMIT]
                == s.package[PACKAGE_OFFSET + MU_KEEP_TO..PACKAGE_OFFSET + MU_LIMIT]
        );
    }
    // A sector that arrives different the second time isn't written.
    make_package(KEY, APP_SIZE);
    reset_all();
    st().corrupt_at_pass2 = Some(0x20000);
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::BadPackage && written);
    let s = st();
    assert!(
        s.flash[0x20000..0x20000 + MU_SECTOR]
            != s.package[PACKAGE_OFFSET + 0x20000..PACKAGE_OFFSET + 0x20000 + MU_SECTOR]
    );
    assert!(s.flash[..MU_FIRST] == s.original[..MU_FIRST]);
    // A host that stops answering.
    reset_all();
    st().stop_answering = true;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::Lost && !written && untouched());
    // The soft key.
    reset_all();
    queue_bytes(&SOFT);
    assert!(mu_run(KEY, &mut written) == Outcome::Download && untouched());
    // Junk, and a too-long frame, are ignored.
    reset_all();
    let mut junk = [0x11; 900];
    junk[0] = 0xf0;
    junk[899] = 0xf7;
    queue_bytes(&junk);
    let bad_sum = [0xf0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x41, 0x7f, 0xf7];
    queue_bytes(&bad_sum);
    st().cancel_now = true;
    assert!(mu_run(KEY, &mut written) == Outcome::Cancelled && !st().identity_seen && untouched());
    // Before anything is erased, the key and the soft key still get out.
    make_package(KEY, APP_SIZE);
    reset_all();
    st().cancel_after = 100;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::Cancelled && !written && untouched());
    reset_all();
    st().soft_after = 100;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::Download && !written && untouched());
    // Once it's erasing, the key does nothing: stopping there would leave no firmware.
    reset_all();
    st().cancel_after = (APP_SIZE / 512) as u32 + 50;
    ask_upgrade();
    assert!(mu_run(KEY, &mut written) == Outcome::Done && written);
    let s = st();
    assert!(
        s.flash[MU_FIRST..APP_SIZE]
            == s.package[PACKAGE_OFFSET + MU_FIRST..PACKAGE_OFFSET + APP_SIZE]
    );
    // The handshake installers send (fm1ota.js HS_QUERY) gets the identity they look for.
    reset_all();
    st().answer_handshake = true;
    assert!(mu_run(KEY, &mut written) == Outcome::Cancelled && st().identity_seen);
    assert!(st().identity[..MU_IDENTITY.len()] == *MU_IDENTITY);
    // Power cuts, on a small package.
    make_package(KEY, 0x20000);
    reset_all();
    let cuts = power_cuts(0x20000, KEY, 1, false);
    // And with real firmware, from one firmware to another.
    let mut old_image = vec![0; 0x200000];
    let (mut old_offset, mut old_size, mut new_offset, mut new_size) = (0, 0, 0, 0);
    let have_old = load_fwsc(
        "testdata/fm1-015.fwsc",
        &mut old_image,
        &mut old_offset,
        &mut old_size,
    );
    let have_new = load_fwsc(
        "testdata/smk37-016.fwsc",
        &mut st().package,
        &mut new_offset,
        &mut new_size,
    );
    if have_old != 0 && have_new != 0 {
        st().package_size = have_new;
        assert!(new_offset == PACKAGE_OFFSET);
        reset_all();
        st().flash[..old_size].copy_from_slice(&old_image[old_offset..old_offset + old_size]);
        assert!(would_start(KEY));
        let real = power_cuts(new_size, KEY, 1, true);
        println!("midi update: {real} power cuts on real firmware");
    } else {
        println!("SKIP: M-VAVE's packages not present (power cuts on real firmware)");
    }
    println!("midi update: install, refusals, limits, order and {cuts} power cuts passed");
}
