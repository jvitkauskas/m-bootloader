// SPDX-License-Identifier: MIT
//! Flash formats, checksums, ciphers, updater decoding and calibration rules. Portable: nothing here touches hardware.
pub const MAX_FILES: usize = 8;
pub const WORK_SIZE: usize = 4096;
pub const UPDATE_LOAD: u32 = 0x01c0_a800;
pub const UPDATE_MAX: u32 = 0x5b1c;
pub const BOOT_AREA: usize = 0x4000;
/// What's left of the configuration: the chip-key record, which firmware installers read, and the settings 1.3 and
/// older require, so that they start if installed over this version (§5.3).
pub const CONFIG_KEY_SIZE: usize = 34;
pub const CONFIG_SIZE: usize = 96;
pub const CONFIG_AT: usize = 0x3fa0;
pub const BOOT_MAX: usize = CONFIG_AT - 0xa0;
const XIP: u32 = 0x0200_0000;
pub type Line = [u8; 32];
pub type Work = [u8; WORK_SIZE];
pub type Request = [u8; 80];

/// A flash that can be read.
pub trait Flash {
    /// Its size in bytes.
    fn capacity(&self) -> u32;

    /// `dst.len()` bytes from `address`, which `read` has checked are inside the flash, and at least one.
    fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool;

    fn read(&self, address: u32, dst: &mut [u8]) -> bool {
        range(address, dst.len() as u32, self.capacity())
            && (dst.is_empty() || self.read_inside(address, dst))
    }

    /// A line of the top-level directory, which is stored under the fixed key.
    fn read_top(&self, address: u32, raw: &mut Line) -> bool {
        if !self.read(address, raw) {
            return false;
        }
        enc(0xffff, raw);
        true
    }
}

/// A flash that can be written too.
pub trait Nor: Flash {
    /// Within one page of 256 bytes.
    fn program(&self, address: u32, src: &[u8]) -> bool;

    /// A sector of 4096 bytes, or a block of 65536.
    fn erase(&self, address: u32, size: u32) -> bool;
}

/// The flash as the flash loader's table of services has it (rust/src/loader.rs): functions with C's calling
/// convention and a context, which its tests fill in too.
#[cfg(any(not(target_arch = "pi32v2"), loader_a, loader_b))]
mod c_flash {
    use super::{Flash, Nor};
    use core::ffi::c_void;

    #[repr(C)]
    pub struct CFlash {
        pub ctx: *mut c_void,
        pub capacity: u32,
        pub read: unsafe extern "C" fn(*mut c_void, u32, *mut c_void, u32) -> bool,
    }

    #[repr(C)]
    pub struct CNor {
        pub flash: CFlash,
        pub program: unsafe extern "C" fn(*mut c_void, u32, *const c_void, u32) -> bool,
        pub erase: unsafe extern "C" fn(*mut c_void, u32, u32) -> bool,
    }

    // Safety, for these: whoever made the struct gave it functions that take what they are given here.
    impl Flash for CFlash {
        fn capacity(&self) -> u32 {
            self.capacity
        }

        fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool {
            unsafe { (self.read)(self.ctx, address, dst.as_mut_ptr().cast(), dst.len() as u32) }
        }
    }

    impl Flash for CNor {
        fn capacity(&self) -> u32 {
            self.flash.capacity
        }

        fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool {
            self.flash.read_inside(address, dst)
        }
    }

    impl Nor for CNor {
        fn program(&self, address: u32, src: &[u8]) -> bool {
            unsafe {
                (self.program)(
                    self.flash.ctx,
                    address,
                    src.as_ptr().cast(),
                    src.len() as u32,
                )
            }
        }

        fn erase(&self, address: u32, size: u32) -> bool {
            unsafe { (self.erase)(self.flash.ctx, address, size) }
        }
    }
}
#[cfg(any(not(target_arch = "pi32v2"), loader_a, loader_b))]
pub use c_flash::{CFlash, CNor};

#[repr(C)]
pub struct Config {
    pub oscillator_hz: u32,
    pub system_hz: u32,
    pub width: u8,
    pub spi_divider: u8,
    pub read_mode: u8,
    pub hsb_divider: u8,
    pub lsb_divider: u8,
    pub oscillator_tag: u8,
    pub high_current: u8,
    pub one_pin: u8,
    pub reset_pin: u8,
    pub reset_level: u8,
    pub reset_hold: u8,
}

impl Config {
    /// The values every known configuration has.
    pub const DEFAULTS: Config = Config {
        oscillator_hz: 24_000_000,
        system_hz: 360_000_000,
        width: 4,
        spi_divider: 1,
        read_mode: 4,
        hsb_divider: 1,
        lsb_divider: 2,
        oscillator_tag: 0,
        high_current: 0,
        one_pin: 0,
        reset_pin: 17,
        reset_level: 0,
        reset_hold: 8,
    };
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Record {
    pub address: u32,
    pub length: u32,
    pub data_crc: u16,
    pub index: u16,
    pub attribute: u8,
    pub name: [u8; 17],
}

#[repr(C)]
pub struct Header {
    pub bytes: Line,
    pub alignment: u8,
    pub filesystem_size: u32,
}

impl Header {
    pub const EMPTY: Header = Header {
        bytes: [0; 32],
        alignment: 0,
        filesystem_size: 0,
    };
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AppState {
    NotFound,
    Ok,
    BadCrc,
    Incomplete,
}

#[repr(C)]
pub struct App {
    pub base: u32,
    pub entry: u32,
    pub extent: u32,
    pub app_length: u32,
    pub key: u16,
    pub file_count: u8,
    pub state: AppState,
    pub files: [Record; MAX_FILES],
}

impl App {
    pub const EMPTY: App = App {
        base: 0,
        entry: 0,
        extent: 0,
        app_length: 0,
        key: 0,
        file_count: 0,
        state: AppState::NotFound,
        files: [Record::EMPTY; MAX_FILES],
    };
}

#[repr(C)]
pub struct Updater {
    pub load: u32,
    pub size: u32,
    pub stream_end: u32,
    pub crc: u16,
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Guard {
    Unchanged,
    BackedUp,
    Restored,
    Error,
}

// As C lays them out: C code and the tests share them.
const _: () =
    assert!(size_of::<Config>() == 20 && size_of::<Record>() == 32 && size_of::<Header>() == 40);
const _: () = assert!(size_of::<App>() == 280 && size_of::<Updater>() == 16);

/// Fields at constant offsets of an array: checked when compiled, with nothing to call.
#[inline(always)]
pub fn le16<const N: usize>(b: &[u8; N], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

#[inline(always)]
pub fn le32<const N: usize>(b: &[u8; N], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

#[inline(always)]
pub fn put_le16<const N: usize>(b: &mut [u8; N], at: usize, v: u16) {
    b[at] = v as u8;
    b[at + 1] = (v >> 8) as u8;
}

#[inline(always)]
pub fn put_le32<const N: usize>(b: &mut [u8; N], at: usize, v: u32) {
    put_le16(b, at, v as u16);
    put_le16(b, at + 2, (v >> 16) as u16);
}

#[inline(always)]
pub fn be16<const N: usize>(b: &[u8; N], at: usize) -> u16 {
    u16::from_be_bytes([b[at], b[at + 1]])
}

#[inline(always)]
pub fn be32<const N: usize>(b: &[u8; N], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

#[inline(always)]
pub fn put_be16<const N: usize>(b: &mut [u8; N], at: usize, v: u16) {
    b[at] = (v >> 8) as u8;
    b[at + 1] = v as u8;
}

#[inline(always)]
pub fn put_be32<const N: usize>(b: &mut [u8; N], at: usize, v: u32) {
    put_be16(b, at, (v >> 16) as u16);
    put_be16(b, at + 2, v as u16);
}

pub fn range(address: u32, size: u32, limit: u32) -> bool {
    address <= limit && size <= limit - address
}

pub fn crc16(mut r: u16, data: &[u8]) -> u16 {
    for &b in data {
        r ^= (b as u16) << 8;
        for _ in 0..8 {
            r = r << 1 ^ if r & 0x8000 != 0 { 0x1021 } else { 0 };
        }
    }
    r
}

/// Raw IEEE streaming state: begin `u32::MAX`, finish XOR `u32::MAX`.
pub fn crc32(mut r: u32, data: &[u8]) -> u32 {
    for &b in data {
        r ^= b as u32;
        for _ in 0..8 {
            r = r >> 1 ^ if r & 1 != 0 { 0xedb8_8320 } else { 0 };
        }
    }
    r
}

pub fn enc(mut state: u16, data: &mut [u8]) -> u16 {
    for b in data {
        *b ^= state as u8;
        state = state << 1 ^ if state & 0x8000 != 0 { 0x1021 } else { 0 };
    }
    state
}

pub fn usb_begin(key: u32) -> u16 {
    crc16(key as u16, &((key >> 16) as u16).to_le_bytes())
}

pub fn usb_stream(mut state: u16, index: &mut u32, data: &mut [u8]) -> u16 {
    const ENTROPY: [u8; 16] = [
        0xc3, 0xcf, 0xc0, 0xe8, 0xce, 0xd2, 0xb0, 0xae, 0xc4, 0xe3, 0xa3, 0xac, 0xd3, 0xf1, 0xc1,
        0xd6,
    ];
    for b in data {
        state = crc16(state, &[ENTROPY[(*index & 15) as usize]]);
        *index = index.wrapping_add(1);
        *b ^= state as u8;
    }
    state
}

/// A line's own checksum: its first two bytes, over the other thirty.
fn line_ok(raw: &Line) -> bool {
    crc16(0, &raw[2..]) == le16(raw, 0)
}

pub fn app_read(f: &impl Flash, base: u32, key: u16, mut offset: u32, mut dst: &mut [u8]) -> bool {
    if base > f.capacity() || !range(offset, dst.len() as u32, f.capacity() - base) {
        return false;
    }
    while !dst.is_empty() {
        let within = (offset & 31) as usize;
        if within == 0 && dst.len() >= 32 {
            let n = (dst.len() & !31).min(WORK_SIZE);
            let (now, rest) = dst.split_at_mut(n);
            if !f.read(base + offset, now) {
                return false;
            }
            for line in now.chunks_exact_mut(32) {
                enc(key ^ (offset >> 2) as u16, line);
                offset += 32;
            }
            dst = rest;
            continue;
        }
        let q = offset & !31;
        let mut line = [0; 32];
        if !f.read(base + q, &mut line) {
            return false;
        }
        enc(key ^ (q >> 2) as u16, &mut line);
        let n = (32 - within).min(dst.len());
        let (now, rest) = dst.split_at_mut(n);
        now.copy_from_slice(&line[within..within + n]);
        offset += n as u32;
        dst = rest;
    }
    true
}

pub fn chipkey(prefix: &[u8; 34]) -> Option<u16> {
    if crc16(0, &prefix[..32]) != le16(prefix, 32) {
        return None;
    }
    let mut sum = prefix[..16].iter().fold(0u8, |a, &b| a.wrapping_add(b));
    if sum >= 0xe0 {
        sum = 0xaa;
    } else if sum <= 0x10 {
        sum = 0x55;
    }
    let mut result = 0;
    for i in 0..16 {
        if prefix[16 + i] ^ prefix[15 - i] < sum {
            result |= 1 << i;
        }
    }
    Some(result)
}

impl Record {
    pub const EMPTY: Record = Record {
        address: 0,
        length: 0,
        data_crc: 0,
        index: 0,
        attribute: 0,
        name: [0; 17],
    };

    /// In place: a record is 32 bytes, and returning one by value costs two copies of it.
    pub fn parse(&mut self, raw: &Line) -> bool {
        if !line_ok(raw) {
            return false;
        }
        self.data_crc = le16(raw, 2);
        self.address = le32(raw, 4);
        self.length = le32(raw, 8);
        self.attribute = raw[12];
        self.index = le16(raw, 14);
        let (name, end) = self.name.split_at_mut(16);
        name.copy_from_slice(&raw[16..]);
        end[0] = 0;
        true
    }

    /// The name is NUL-terminated, in 17 bytes.
    pub fn is(&self, name: &[u8]) -> bool {
        self.name
            .strip_prefix(name)
            .is_some_and(|rest| rest.first() == Some(&0))
    }
}

pub fn header_read(f: &impl Flash, h: &mut Header) -> bool {
    h.alignment = 0;
    if !f.read_top(0, &mut h.bytes) || !line_ok(&h.bytes) {
        return false;
    }
    h.filesystem_size = le32(&h.bytes, 8);
    if h.filesystem_size > f.capacity() {
        return false;
    }
    h.alignment = h.bytes[13];
    true
}

pub fn top_find(f: &impl Flash, name: &[u8], r: &mut Record, raw: &mut Line) -> bool {
    let (mut offset, mut end) = (32, f.capacity());
    while offset < 32 * 65 && range(offset, 32, end) {
        if !f.read_top(offset, raw) || !r.parse(raw) {
            return false;
        }
        if r.address >= offset + 32 && r.address < end {
            end = r.address;
        }
        if r.is(name) {
            return true;
        }
        offset += 32;
    }
    false
}

fn info_version(f: &impl Flash, address: u32, raw: &mut Line) -> Option<u16> {
    (f.read_top(address, raw) && line_ok(raw)).then(|| le16(raw, 6))
}

fn sentinel(length: u32) -> bool {
    length == 0 || length == u32::MAX
}

/// The directory to use, of the two the flash may hold: the second if the first's version can't be read or is older.
fn directory(f: &impl Flash, selected: &mut Record, raw: &mut Line) -> bool {
    if !top_find(f, b"app_dir_head", selected, raw) {
        return false;
    }
    if !sentinel(selected.length) {
        let primary = info_version(f, selected.length, raw);
        let mut secondary = Record::EMPTY;
        if top_find(f, b"app_dir_head2", &mut secondary, raw) {
            let newer = |p| {
                !sentinel(secondary.length)
                    && info_version(f, secondary.length, raw).is_some_and(|s| s > p)
            };
            if primary.is_none_or(newer) {
                *selected = secondary;
            }
        }
    }
    true
}

/// The files, and whether there is one app among them. None if the directory is unsound.
fn files(f: &impl Flash, app: &mut App, room: u32, raw: &mut Line) -> Option<bool> {
    let mut record = Record::EMPTY;
    let (mut offset, mut first_file, mut have_app) = (32, u32::MAX, false);
    for _ in 0..MAX_FILES {
        if !range(offset, 32, first_file) {
            break;
        }
        if !app_read(f, app.base, app.key, offset, raw) {
            return None;
        }
        if !record.parse(raw) {
            break;
        }
        offset += 32;
        if record.attribute != 0x82 {
            continue;
        }
        if record.address < offset {
            return None;
        }
        if !range(record.address, record.length, room) {
            app.state = AppState::Incomplete;
            return None;
        }
        first_file = first_file.min(record.address);
        *app.files.get_mut(app.file_count as usize)? = record;
        app.file_count += 1;
        if record.is(b"app.bin") && record.length != 0 {
            let entry_offset = app.entry.wrapping_sub(XIP);
            if have_app {
                return None;
            }
            if app.entry == 0
                || entry_offset < record.address
                || entry_offset - record.address >= record.length
            {
                app.entry = 0;
            }
            have_app = true;
            app.app_length = record.length;
        }
    }
    if first_file < offset {
        return None;
    }
    Some(have_app)
}

/// Mount validates metadata, ranges and the entry; `app_check` validates every file's data. `header` gets the freshly
/// decoded flash header even if no usable app is found.
pub fn app_mount(f: &impl Flash, key: u16, app: &mut App, header: &mut Header) -> bool {
    *app = App::EMPTY;
    app.key = key;
    if !header_read(f, header) {
        return false;
    }
    let (mut raw, mut selected, mut area) = ([0; 32], Record::EMPTY, Record::EMPTY);
    if !directory(f, &mut selected, &mut raw)
        || selected.address & 31 != 0
        || !app_read(f, selected.address, key, 0, &mut raw)
    {
        return false;
    }
    if !area.parse(&raw) || area.attribute != 0x83 || !area.is(b"app_area_head") {
        return false;
    }
    let room = f.capacity() - selected.address;
    app.base = selected.address;
    app.extent = area.length;
    if area.address >= XIP && area.address & 1 == 0 && area.address - XIP < room {
        app.entry = area.address;
    }
    if files(f, app, room, &mut raw) == Some(true) && app.entry != 0 {
        app.state = AppState::Ok;
        return true;
    }
    app.base = 0;
    app.entry = 0;
    false
}

fn erased_tail(f: &impl Flash, end: u32, scratch: &mut Line) -> bool {
    f.read(end - 32, scratch) && scratch.iter().all(|&b| b == 255)
}

fn check(f: &impl Flash, app: &App, work: &mut Work) -> AppState {
    if app.base == 0 || app.base > f.capacity() || app.entry == 0 {
        return AppState::NotFound;
    }
    let Some(files) = app
        .files
        .get(..app.file_count as usize)
        .filter(|files| !files.is_empty())
    else {
        return AppState::NotFound;
    };
    let mut state = AppState::Ok;
    for r in files {
        if !range(r.address, r.length, f.capacity() - app.base) {
            return AppState::Incomplete;
        }
        let (mut pos, mut crc) = (0, 0);
        while pos < r.length {
            let part = &mut work[..(r.length - pos).min(WORK_SIZE as u32) as usize];
            if !app_read(f, app.base, app.key, r.address + pos, part) {
                return AppState::NotFound;
            }
            crc = crc16(crc, part);
            pos += part.len() as u32;
        }
        if crc != r.data_crc {
            let start = app.base + r.address;
            let end = start + r.length;
            let scratch = work.first_chunk_mut().unwrap();
            if state != AppState::Incomplete {
                state = AppState::BadCrc;
            }
            // An erased sector end inside the file: an install that stopped.
            let mut boundary = (start & !4095) + 4096;
            while boundary <= end {
                if boundary - start >= 32 && erased_tail(f, boundary, scratch) {
                    state = AppState::Incomplete;
                }
                if end - boundary < 4096 {
                    break;
                }
                boundary += 4096;
            }
            if r.length >= 32 && erased_tail(f, end, scratch) {
                state = AppState::Incomplete;
            }
        }
    }
    state
}

pub fn app_check(f: &impl Flash, app: &mut App, work: &mut Work) -> AppState {
    app.state = check(f, app, work);
    app.state
}

pub fn request_valid(request: &Request) -> bool {
    let crc = crc16(0, &request[2..]);
    crc != 0 && le16(request, 0) == crc && le16(request, 2) != 0
}

pub fn uart_request(retained: &[u8; 19]) -> bool {
    retained[..11] == *b"UARTUPDATE\0" || retained == b"UART_UPDATE_CUSTOM\0"
}

fn request_step(alignment: u8) -> u32 {
    if alignment != 0 && alignment <= 16 {
        alignment as u32 * 256
    } else {
        4096
    }
}

fn request_mark(raw: &Request) -> bool {
    le16(raw, 6) == 0x5441
}

/// Selects the first magic/CRC match, then checks its nonzero CRC and type. With `count`, counts all magic/CRC matches
/// independently of validity.
pub fn request_scan(
    f: &impl Flash,
    alignment: u8,
    request: &mut Request,
    count: Option<&mut u32>,
) -> bool {
    let mut raw = [0; 80];
    let step = request_step(alignment);
    let mut matches = 0;
    if f.capacity() >= step {
        let mut boundary = f.capacity() - step;
        while boundary > 4096 && f.read(boundary - 256, &mut raw) {
            if request_mark(&raw) && crc16(0, &raw[2..]) == le16(&raw, 0) {
                if matches == 0 {
                    *request = raw;
                }
                matches += 1;
                if count.is_none() {
                    break;
                }
            }
            if boundary < step {
                break;
            }
            boundary -= step;
        }
    }
    if let Some(count) = count {
        *count = matches;
    }
    matches != 0 && request_valid(request)
}

fn lz4_length(input: &[u8], pos: &mut usize, length: &mut usize) -> Option<()> {
    loop {
        let extra = *input.get(*pos)?;
        *pos += 1;
        *length = length.checked_add(extra as usize)?;
        if extra != 255 {
            return Some(());
        }
    }
}

/// One block, onto the `produced` bytes that `out` already holds. Some(true) once it has added exactly `block`.
fn lz4(input: &[u8], out: &mut [u8], mut produced: usize, block: usize) -> Option<bool> {
    if input.is_empty() || block == 0 || produced > out.len() || block > out.len() - produced {
        return None;
    }
    let end = produced + block;
    let mut p = 0;
    while p < input.len() {
        let token = input[p];
        p += 1;
        let mut literal = (token >> 4) as usize;
        if literal == 15 {
            lz4_length(input, &mut p, &mut literal)?;
        }
        if literal > end - produced {
            return None;
        }
        out[produced..produced + literal].copy_from_slice(input.get(p..p.checked_add(literal)?)?);
        p += literal;
        produced += literal;
        if p == input.len() {
            return Some(produced == end);
        }
        let &[low, high] = input.get(p..p + 2)? else {
            return None;
        };
        let distance = u16::from_le_bytes([low, high]) as usize;
        p += 2;
        if distance == 0 || distance > produced {
            return None;
        }
        let mut matched = (token & 15) as usize + 4;
        if token & 15 == 15 {
            lz4_length(input, &mut p, &mut matched)?;
        }
        if matched > end - produced {
            return None;
        }
        for _ in 0..matched {
            out[produced] = out[produced - distance];
            produced += 1;
        }
    }
    None
}

pub fn lz4_block(input: &[u8], out: &mut [u8], produced: usize, block: usize) -> bool {
    lz4(input, out, produced, block) == Some(true)
}

pub fn updater_decode(
    f: &impl Flash,
    request: &Request,
    out: &mut [u8],
    work: &mut Work,
    u: &mut Updater,
) -> bool {
    let (mut raw, mut outer) = ([0; 32], Record::EMPTY);
    if !request_valid(request) || !f.read(le32(request, 72), &mut raw) || !outer.parse(&raw) {
        return false;
    }
    let (stream, length) = (outer.address, outer.length);
    if length < 64
        || !range(stream, length, f.capacity())
        || !f.read(stream + 32, &mut raw)
        || !line_ok(&raw)
    {
        return false;
    }
    let total = le32(&raw, 4);
    if le32(&raw, 8) != UPDATE_LOAD
        || total == 0
        || total > UPDATE_MAX
        || total as usize > out.len()
    {
        return false;
    }
    *u = Updater {
        load: UPDATE_LOAD,
        size: total,
        stream_end: stream + length,
        crc: le16(&raw, 2),
    };
    let (mut pos, mut produced) = (64, 0);
    while pos < length {
        if length - pos < 8 || !f.read(stream + pos, &mut raw[..8]) {
            return false;
        }
        let (compressed, output) = (le32(&raw, 0), le32(&raw, 4));
        pos += 8;
        if compressed == 0
            || compressed > length - pos
            || output == 0
            || !range(produced, output, total)
        {
            return false;
        }
        let Some(block) = work.get_mut(..compressed as usize) else {
            return false;
        };
        if !f.read(stream + pos, block)
            || !lz4_block(block, out, produced as usize, output as usize)
        {
            return false;
        }
        pos += compressed;
        produced += output;
    }
    produced == total && crc16(0, &out[..total as usize]) == u.crc
}

fn program_pages(nor: &impl Nor, mut address: u32, mut src: &[u8]) -> bool {
    while !src.is_empty() {
        let (now, rest) = src.split_at((256 - (address & 255) as usize).min(src.len()));
        if !nor.program(address, now) {
            return false;
        }
        address += now.len() as u32;
        src = rest;
    }
    true
}

fn verify_bytes(f: &impl Flash, mut address: u32, expected: &[u8]) -> bool {
    let mut raw = [0; 32];
    for part in expected.chunks(32) {
        let got = &mut raw[..part.len()];
        if !f.read(address, got) || got != part {
            return false;
        }
        address += 32;
    }
    true
}

const TRIM_AT: u32 = 0xc1000;
const TRIM_SIZE: usize = 0x22c;
const TRIM_NAME: usize = 0x224;

/// The SMK-37's calibration guard (§9.3): keeps a copy of the calibration at the top of the flash, and puts it back.
pub fn guard(nor: &impl Nor, smk: bool, work: &mut Work) -> Guard {
    let f = nor;
    if !smk {
        return Guard::Unchanged;
    }
    if f.capacity() < 0xc3000 || f.capacity() & 4095 != 0 {
        return Guard::Error;
    }
    let base = f.capacity() - 4096;
    let (slot, rest): (&mut [u8; 16 + TRIM_SIZE], _) = work.split_first_chunk_mut().unwrap();
    let source: &mut [u8; TRIM_SIZE] = rest.first_chunk_mut().unwrap();
    if (base < TRIM_AT + 4096 && TRIM_AT < base + 4096)
        || !f.read(base, slot)
        || !f.read(TRIM_AT, source)
    {
        return Guard::Error;
    }
    let source_valid = source[TRIM_NAME..] == *b"trimdat\0";
    let (head, saved) = slot.split_at_mut(16);
    let head: &mut [u8; 16] = head.try_into().unwrap();
    let valid = head[..8] == *b"TRIMCOPY"
        && le32(head, 8) == TRIM_AT
        && le16(head, 12) == TRIM_SIZE as u16
        && le16(head, 14) == crc16(0, saved)
        && saved[TRIM_NAME..] == *b"trimdat\0";
    if valid {
        if source_valid {
            return Guard::Unchanged;
        }
        if !nor.erase(TRIM_AT, 4096)
            || !program_pages(nor, TRIM_AT, saved)
            || !verify_bytes(f, TRIM_AT, saved)
        {
            return Guard::Error;
        }
        return Guard::Restored;
    }
    let blank = head.iter().chain(saved.iter()).all(|&b| b == 255);
    if !blank || !source_valid {
        return Guard::Unchanged;
    }
    *head.first_chunk_mut().unwrap() = *b"TRIMCOPY";
    put_le32(head, 8, TRIM_AT);
    put_le16(head, 12, TRIM_SIZE as u16);
    put_le16(head, 14, crc16(0, source));
    if !program_pages(nor, base + 16, source)
        || !verify_bytes(f, base + 16, source)
        || !program_pages(nor, base, head)
        || !verify_bytes(f, base, head)
    {
        return Guard::Error;
    }
    Guard::BackedUp
}

/// Invalidates, by programming their CRC to zero, the flash requests (scanned as `request_scan`) whose staged stream
/// ends at `stream_end`.
pub fn request_retire(nor: &impl Nor, alignment: u8, stream_end: u32) -> u32 {
    let (mut raw, mut outer, mut r) = ([0; 80], [0; 32], Record::EMPTY);
    let (f, step) = (nor, request_step(alignment));
    let mut retired = 0;
    if f.capacity() < step {
        return 0;
    }
    let mut boundary = f.capacity() - step;
    while boundary > 4096 && f.read(boundary - 256, &mut raw) {
        if request_mark(&raw)
            && request_valid(&raw)
            && f.read(le32(&raw, 72), &mut outer)
            && r.parse(&outer)
            && r.address.wrapping_add(r.length) == stream_end
            && nor.program(boundary - 256, &[0, 0])
        {
            retired += 1;
        }
        if boundary < step {
            break;
        }
        boundary -= step;
    }
    retired
}

fn boot_allocation_safe(address: u32, length: u32, capacity: u32) -> bool {
    address < capacity
        && length <= capacity - address
        && !(address < BOOT_AREA as u32 && address.wrapping_add(length) > 0xa0)
}

/// The settings 1.2 and 1.3 require: value length, name, NUL, value (§5.3).
const CONFIG_SETTINGS: [u8; CONFIG_SIZE - CONFIG_KEY_SIZE] =
    *b"\x04OSC\0OSC0\x04OSC_FREQ\0\x00\x36\x6e\x01\x04SYS_CLK\0\x00\x2a\x75\x15\x04HSB_DIV\0\x01\0\0\0\x04LSB_DIV\0\x02\0\0\0";

/// A directory line of the boot area, decoded.
fn area_line(area: &[u8; BOOT_AREA], at: usize) -> Line {
    let mut line: Line = area[at..at + 32].try_into().unwrap();
    enc(0xffff, &mut line);
    line
}

/// Rewrites a stored boot area to hold `container` as its bootloader, keeping the flash header and every other record
/// (§11.2); false leaves it as found.
pub fn boot_install(area: &mut [u8; BOOT_AREA], container: &[u8], capacity: u32) -> bool {
    let size = container.len();
    let Some((start, payload)) = container.split_first_chunk::<16>() else {
        return false;
    };
    if size < 0x80
        || size > BOOT_MAX
        || le16(start, 0) != 1
        || le16(start, 2) as usize != size - 16
        || le32(start, 4) != 0x01c0_2000
        || le32(start, 8) != 16
        || crc16(0, &start[..14]) != le16(start, 14)
        || crc16(0, payload) != le16(start, 12)
    {
        return false;
    }
    let (head, mut record, mut r) = (area_line(area, 0), area_line(area, 32), Record::EMPTY);
    if !line_ok(&head)
        || !r.parse(&record)
        || !r.is(b"uboot.boot")
        || r.address != 0xa0
        || r.length < 16
        || r.length > BOOT_MAX as u32
    {
        return false;
    }
    let (mut config, mut blob, mut have_config) = ([0; 32], [0; CONFIG_SIZE], false);
    // A0 is the first data allocation, so only three more records fit.
    for offset in [64, 96, 128] {
        let other = area_line(area, offset);
        if !r.parse(&other) {
            return false;
        }
        if r.is(b"isd_config.ini") {
            // Only its chip-key record is kept, so that has to be good.
            if have_config
                || r.address < 0x38d0
                || r.length < CONFIG_KEY_SIZE as u32
                || r.length > BOOT_AREA as u32
                || r.address > BOOT_AREA as u32 - r.length
            {
                return false;
            }
            let (key, settings) = blob.split_at_mut(CONFIG_KEY_SIZE);
            key.copy_from_slice(&area[r.address as usize..][..CONFIG_KEY_SIZE]);
            settings.copy_from_slice(&CONFIG_SETTINGS);
            if crc16(0, &blob[..32]) != le16(&blob, 32) {
                return false;
            }
            config = other;
            have_config = true;
            continue;
        }
        let sentinel = sentinel(r.length);
        if r.is(b"app_dir_head") || r.is(b"app_dir_head2") {
            // Directory lengths are sentinels or bank-info addresses.
            if !boot_allocation_safe(r.address, 32, capacity)
                || (!sentinel && !boot_allocation_safe(r.length, 32, capacity))
            {
                return false;
            }
        } else if !boot_allocation_safe(r.address, if sentinel { 32 } else { r.length }, capacity) {
            return false;
        }
    }
    if !have_config {
        return false;
    }
    let seal = |line: &mut Line, crc: u16, address: usize, length: usize| {
        put_le16(line, 2, crc);
        put_le32(line, 4, address as u32);
        put_le32(line, 8, length as u32);
        put_le16(line, 0, crc16(0, &line[2..]));
        enc(0xffff, line);
    };
    seal(&mut record, crc16(0, container), 0xa0, size);
    seal(&mut config, crc16(0, &blob), CONFIG_AT, CONFIG_SIZE);
    area[32..64].copy_from_slice(&record);
    for offset in [64, 96, 128] {
        if r.parse(&area_line(area, offset)) && r.is(b"isd_config.ini") {
            area[offset..offset + 32].copy_from_slice(&config);
        }
    }
    area[0xa0..].fill(0xff);
    area[CONFIG_AT..].copy_from_slice(&blob);
    let boot = &mut area[0xa0..0xa0 + size];
    boot.copy_from_slice(container);
    let (start, payload) = boot.split_at_mut(16);
    enc(0xffff, start);
    enc(0xffff, payload);
    true
}
