// SPDX-License-Identifier: MIT
//! The flash formats (rust/src/formats.rs) on a model of the flash, which counts what is done to it and can be made
//! to fail: checksums and ciphers, the two banks, LZ4, update requests, mounting, the calibration guard, and the
//! private images of real units where they are present (testdata/).
use crate::Global;
use crate::formats::{
    App, AppState, Flash, Guard, Header, Nor, Record, Request, Updater, Work, app_check,
    app_mount as app_mount_header, app_read, be16, be32, chipkey, crc16, crc32, enc, guard,
    header_read, le16, le32, lz4_block, put_be16, put_be32, put_le16, put_le32, range,
    request_scan, request_valid, top_find, uart_request, updater_decode, usb_begin, usb_stream,
};
use std::io::{ErrorKind, Read};

const CAPACITY: u32 = 0x10_0000;
// The test's own copies, so that a wrong value in the crate fails here and is not followed: the work buffer's size,
// where a staged updater is loaded (docs/SPEC.md §2.1), and the size of M-VAVE's, which is the most one may be.
const WORK_SIZE: usize = 4096;
const UPDATE_LOAD: u32 = 0x01c0_a800;
const UPDATE_MAX: u32 = 0x5b1c;
static IMAGE: Global<[u8; CAPACITY as usize]> = Global::new([0; CAPACITY as usize]);
static SAVED: Global<[u8; CAPACITY as usize]> = Global::new([0; CAPACITY as usize]);
static WORKSPACE: Global<Work> = Global::new([0; WORK_SIZE]);
static UPDATER_OUTPUT: Global<[u8; UPDATE_MAX as usize]> = Global::new([0; UPDATE_MAX as usize]);
static ASSERTIONS: Global<u32> = Global::new(0);
static FIXTURE_GROUPS: Global<u32> = Global::new(0);

struct Model {
    programs: u32,
    erases: u32,
    operations: u32,
    fail_at: u32,
    reads: u32,
    bad_read: bool,
    first_program: u32,
    last_program: u32,
    erase_address: [u32; 256],
    erase_size: [u32; 256],
}

impl Model {
    const NEW: Model = Model {
        programs: 0,
        erases: 0,
        operations: 0,
        fail_at: 0,
        reads: 0,
        bad_read: false,
        first_program: 0,
        last_program: 0,
        erase_address: [0; 256],
        erase_size: [0; 256],
    };
}

static MODEL: Global<Model> = Global::new(Model::NEW);

// Safety, for these: the test is one thread, and no reference that one of these gives is held while another to the
// same static is made (so never across a call that reaches the flash, for the image and the model).
fn image() -> &'static mut [u8; CAPACITY as usize] {
    unsafe { IMAGE.get() }
}

fn saved() -> &'static mut [u8; CAPACITY as usize] {
    unsafe { SAVED.get() }
}

fn workspace() -> &'static mut Work {
    unsafe { WORKSPACE.get() }
}

fn updater_output() -> &'static mut [u8; UPDATE_MAX as usize] {
    unsafe { UPDATER_OUTPUT.get() }
}

fn model() -> &'static mut Model {
    unsafe { MODEL.get() }
}

/// The flash that the code under test is given: the image, through the model it has a reference to.
struct ModelFlash(&'static Global<Model>);

impl Flash for ModelFlash {
    fn capacity(&self) -> u32 {
        CAPACITY
    }

    fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool {
        let m = unsafe { self.0.get() };
        m.reads += 1;
        assert!(range(address, dst.len() as u32, CAPACITY));
        if m.bad_read {
            return false;
        }
        dst.copy_from_slice(&image()[address as usize..][..dst.len()]);
        true
    }
}

impl Nor for ModelFlash {
    fn program(&self, address: u32, src: &[u8]) -> bool {
        let m = unsafe { self.0.get() };
        let size = src.len() as u32;
        assert!(size != 0 && size <= 256 && size <= 256 - (address & 255));
        assert!(range(address, size, CAPACITY));
        m.operations += 1;
        if m.operations == m.fail_at {
            return false;
        }
        if m.programs == 0 {
            m.first_program = address;
        }
        m.last_program = address;
        m.programs += 1;
        for (i, &byte) in src.iter().enumerate() {
            let at = address as usize + i;
            assert!(image()[at] & byte == byte);
            image()[at] &= byte;
        }
        true
    }

    fn erase(&self, address: u32, size: u32) -> bool {
        let m = unsafe { self.0.get() };
        assert!(size == 4096 || size == 65536);
        assert!(address & (size - 1) == 0);
        assert!(range(address, size, CAPACITY));
        m.operations += 1;
        if m.operations == m.fail_at {
            return false;
        }
        assert!(m.erases < 256);
        m.erase_address[m.erases as usize] = address;
        m.erase_size[m.erases as usize] = size;
        m.erases += 1;
        image()[address as usize..][..size as usize].fill(255);
        true
    }
}

static FLASH: ModelFlash = ModelFlash(&MODEL);
static NOR: ModelFlash = ModelFlash(&MODEL);

fn reset_model() {
    *model() = Model::NEW;
}

/// A mount by one who has no use for the flash header.
fn app_mount(f: &impl Flash, key: u16, app: &mut App) -> bool {
    let mut header = Header::EMPTY;
    app_mount_header(f, key, app, &mut header)
}

fn load_fixture(name: &'static str) -> bool {
    static MISSING: Global<Vec<&'static str>> = Global::new(Vec::new());
    let path = format!("testdata/{name}");
    assert!(!path.is_empty());
    let file = match std::fs::File::open(&path) {
        Err(e) if e.kind() == ErrorKind::NotFound => {
            let missing = unsafe { MISSING.get() };
            if missing.contains(&name) {
                return false;
            }
            assert!(missing.len() < 4);
            missing.push(name);
            println!("SKIP: {name} not present (core fixture groups)");
            return false;
        }
        file => file,
    };
    assert!(file.is_ok());
    let mut file = file.unwrap();
    assert!(file.read_exact(image()).is_ok());
    assert!(matches!(file.read(&mut [0]), Ok(0)));
    reset_model();
    true
}

fn expect_hex(data: &[u8], hex: &str) {
    for (i, &byte) in data.iter().enumerate() {
        let expected = u32::from_str_radix(&hex[i * 2..i * 2 + 2], 16);
        assert!(expected.is_ok());
        assert!(byte as u32 == expected.unwrap());
    }
}

fn test_primitives() {
    let (mut bytes, mut unaligned) = ([0u8; 64], [0u8; 9]);
    assert!(crc16(0, b"123456789") == 0x31c3);
    assert!(crc16(crc16(0, b"1234"), b"56789") == 0x31c3);
    assert!(crc32(u32::MAX, b"123456789") ^ u32::MAX == 0xcbf4_3926);
    let r = crc32(u32::MAX, b"1234");
    assert!(crc32(r, b"56789") ^ u32::MAX == 0xcbf4_3926);
    bytes.fill(0);
    let mut s = enc(0xffff, &mut bytes[..11]);
    enc(s, &mut bytes[11..32]);
    expect_hex(
        &bytes[..32],
        "ffdf9f1f1f3e7cf8f0c1a367cebd5b970f1e3c78d183274e9c1913264c983060",
    );
    enc(0xffff, &mut bytes[..32]);
    assert!(bytes[..32] == bytes[32..]);
    bytes.fill(0);
    enc(0x980f, &mut bytes[..32]);
    expect_hex(
        &bytes[..32],
        "0f3f7efcd9b264c8b162c48810012346ad5a950b162c79d3a66dfbf6ecf9d3a6",
    );
    bytes.fill(0);
    let mut index = 0;
    s = usb_begin(u32::MAX);
    s = usb_stream(s, &mut index, &mut bytes[..7]);
    usb_stream(s, &mut index, &mut bytes[7..32]);
    expect_hex(
        &bytes[..32],
        "2fa481cc4ec15deaeea2b6a6298ad4f70384ebe1cf8a8a14bde6dd1cd0f8463b",
    );
    index = 0;
    usb_stream(usb_begin(u32::MAX), &mut index, &mut bytes[..32]);
    assert!(bytes[..32] == bytes[32..]);
    put_le16(&mut unaligned, 1, 0xabcd);
    assert!(le16(&unaligned, 1) == 0xabcd);
    put_be16(&mut unaligned, 1, 0xabcd);
    assert!(be16(&unaligned, 1) == 0xabcd);
    put_le32(&mut unaligned, 1, 0x89ab_cdef);
    assert!(le32(&unaligned, 1) == 0x89ab_cdef);
    put_be32(&mut unaligned, 1, 0x89ab_cdef);
    assert!(be32(&unaligned, 1) == 0x89ab_cdef);
    assert!(range(CAPACITY, 0, CAPACITY));
    assert!(!range(u32::MAX, 2, CAPACITY));
    assert!(!range(CAPACITY, 1, CAPACITY));
    assert!(!range(1, u32::MAX, CAPACITY));
    reset_model();
    assert!(FLASH.read(CAPACITY, &mut bytes[..0]));
    assert!(model().reads == 0);
    assert!(!FLASH.read(CAPACITY, &mut bytes[..1]));
    assert!(model().reads == 0);
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

#[allow(clippy::too_many_arguments)]
fn encode_record(
    address: u32,
    relative: u32,
    app: bool,
    key: u16,
    name: &[u8],
    data_address: u32,
    size: u32,
    attr: u8,
    crc: u16,
) {
    let mut raw = [0; 32];
    put_le16(&mut raw, 2, crc);
    put_le32(&mut raw, 4, data_address);
    put_le32(&mut raw, 8, size);
    raw[12] = attr;
    let n = name.len().min(16);
    raw[16..16 + n].copy_from_slice(&name[..n]);
    let crc = crc16(0, &raw[2..]);
    put_le16(&mut raw, 0, crc);
    let state = if app {
        key ^ (relative >> 2) as u16
    } else {
        0xffff
    };
    enc(state, &mut raw);
    image()[address as usize..][..32].copy_from_slice(&raw);
}

fn synthetic_bank(base: u32, key: u16, fill: u8) {
    let mut data = [fill; 32];
    encode_record(
        base,
        0,
        true,
        key,
        b"app_area_head",
        0x0200_0120,
        0x1000,
        0x83,
        0xffff,
    );
    encode_record(
        base + 32,
        32,
        true,
        key,
        b"app.bin",
        0x120,
        32,
        0x82,
        crc16(0, &data),
    );
    enc(key ^ (0x120 >> 2), &mut data);
    image()[base as usize + 0x120..][..32].copy_from_slice(&data);
}

fn synthetic_info(address: u32, version: u16, valid: bool) {
    let mut raw = [0; 32];
    put_le16(&mut raw, 6, version);
    let crc = crc16(0, &raw[2..]);
    put_le16(&mut raw, 0, crc);
    if !valid {
        raw[0] ^= 1;
    }
    enc(0xffff, &mut raw);
    image()[address as usize..][..32].copy_from_slice(&raw);
}

fn test_banks() {
    let mut app = App::EMPTY;
    let mut raw = [0; 32];
    image().fill(255);
    reset_model();
    raw.fill(0);
    put_le32(&mut raw, 8, CAPACITY - 4096);
    raw[13] = 16;
    let crc = crc16(0, &raw[2..]);
    put_le16(&mut raw, 0, crc);
    enc(0xffff, &mut raw);
    image()[..32].copy_from_slice(&raw);
    encode_record(32, 0, false, 0, b"uboot.boot", 0xa0, 16, 0, 0);
    encode_record(64, 0, false, 0, b"app_dir_head", 0x10000, 0x3000, 0x81, 0);
    encode_record(96, 0, false, 0, b"app_dir_head2", 0x20000, 0x3020, 0x81, 0);
    synthetic_bank(0x10000, 0x980f, 1);
    // From §5.1 and §5.4: app_area_head, app.bin, and plaintext 0..31, encrypted at offsets 0, 32 and 0x120.
    // CRCs from CRC16/XMODEM (Python's binascii.crc_hqx), and the ENC stream worked out independently.
    image()[0x20000..][..32].copy_from_slice(&[
        0x55, 0x04, 0x81, 0x03, 0xf9, 0xb3, 0x64, 0xca, 0xb1, 0x72, 0xc4, 0x88, 0x93, 0x01, 0x23,
        0x46, 0xcc, 0x2a, 0xe5, 0x54, 0x77, 0x5e, 0x1c, 0xb2, 0xf9, 0x05, 0x9e, 0x97, 0x88, 0xf9,
        0xd3, 0xa6,
    ]);
    image()[0x20020..][..32].copy_from_slice(&[
        0x3e, 0x49, 0xa1, 0x6e, 0x79, 0xb3, 0x64, 0xc8, 0x91, 0x62, 0xc4, 0x88, 0x92, 0x20, 0x61,
        0xc2, 0xc4, 0x1b, 0x87, 0xe1, 0xfc, 0x74, 0x75, 0x17, 0x0f, 0x1e, 0x1d, 0x3a, 0x74, 0xc9,
        0xb3, 0x66,
    ]);
    image()[0x20120..][..32].copy_from_slice(&[
        0x47, 0xae, 0x5c, 0xbf, 0x5d, 0xb7, 0x62, 0xcf, 0xb9, 0x6b, 0xef, 0xc1, 0x98, 0x25, 0x5e,
        0xaf, 0x71, 0xf2, 0xd4, 0xbe, 0x4e, 0xa1, 0x7e, 0xe6, 0xdb, 0x9f, 0x37, 0x41, 0xa8, 0x75,
        0xce, 0xbf,
    ]);
    synthetic_info(0x3000, 10, true);
    synthetic_info(0x3020, 11, true);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x20000);
    assert!(app.entry == 0x0200_0120 && app.file_count == 1 && app.files[0].address == 0x120);
    assert!(app_check(&FLASH, &mut app, workspace()) == AppState::Ok);
    synthetic_info(0x3020, 10, true);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x10000);
    synthetic_info(0x3020, 9, true);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x10000);
    synthetic_info(0x3020, 11, false);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x10000);
    synthetic_info(0x3000, 10, false);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x20000);
    image()[0x20000] ^= 1;
    assert!(!app_mount(&FLASH, 0x980f, &mut app) && app.base == 0);
    synthetic_info(0x3000, 10, true);
    synthetic_info(0x3020, 65535, true);
    synthetic_bank(0x20000, 0x980f, 3);
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.base == 0x20000);
    encode_record(0x20020, 32, true, 0x980f, b"app.bin", u32::MAX, 16, 0x82, 0);
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.state == AppState::Incomplete);
    synthetic_bank(0x20000, 0x980f, 3);
    encode_record(0x20020, 32, true, 0x980f, b"app.bin", 0x120, 0, 0x82, 0);
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.state == AppState::NotFound);
    synthetic_bank(0x20000, 0x980f, 3);
    encode_record(
        0x20000,
        0,
        true,
        0x980f,
        b"app_area_head",
        0x0400_0120,
        0x1000,
        0x83,
        0,
    );
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn test_lz4() {
    let mut out = [0; 32];
    let first = [0x40, b'A', b'B', b'C', b'D'];
    let history = [0x00, 4, 0, 0x10, b'!'];
    let overlap = [0x10, b'x', 1, 0, 0x10, b'!'];
    let (invalid, truncated) = ([0x00, 0, 0], [0xf0, 255]);
    assert!(lz4_block(&first, &mut out, 0, 4));
    assert!(lz4_block(&history, &mut out, 4, 5));
    assert!(out[..9] == *b"ABCDABCD!");
    assert!(lz4_block(&overlap, &mut out, 0, 6));
    assert!(out[..6] == *b"xxxxx!");
    assert!(!lz4_block(&invalid, &mut out, 0, 4));
    assert!(!lz4_block(&truncated, &mut out, 0, 4));
    assert!(!lz4_block(&first, &mut out, 0, 3));
    assert!(!lz4_block(&first, &mut out, u32::MAX as usize, 4));
    assert!(!lz4_block(&history, &mut out, 0, 5));
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn test_fixture(
    name: &'static str,
    image_crc32: u32,
    app_length: u32,
    app_crc: u16,
    cfg_address: u32,
) {
    let (mut h, mut config, mut app) = (Header::EMPTY, Record::EMPTY, App::EMPTY);
    if !load_fixture(name) {
        return;
    }
    saved().copy_from_slice(image());
    assert!(crc32(u32::MAX, image()) ^ u32::MAX == image_crc32);
    for i in 0xa0..0x38d0 {
        assert!(image()[i] == 255);
    }
    assert!(header_read(&FLASH, &mut h));
    assert!(le16(&h.bytes, 0) == 0xdca1 && h.alignment == 16);
    assert!(top_find(
        &FLASH,
        b"isd_config.ini",
        &mut config,
        &mut [0; 32]
    ));
    assert!(config.address == 0x38d0 && config.length == 699);
    assert!(FLASH.read(config.address, &mut workspace()[..config.length as usize]));
    assert!(crc16(0, &workspace()[..config.length as usize]) == config.data_crc);
    let key = chipkey(workspace().first_chunk().unwrap());
    assert!(key == Some(0x980f));
    let key = key.unwrap();
    assert!(app_mount(&FLASH, key, &mut app));
    assert!(app.base == 0x4000 && app.entry == 0x0200_0120);
    assert!(
        app.file_count == 2 && app.app_length == app_length && app.files[0].data_crc == app_crc
    );
    assert!(
        app.files[1].address == cfg_address
            && app.files[1].length == 383
            && app.files[1].data_crc == 0x2e75
    );
    assert!(app_check(&FLASH, &mut app, workspace()) == AppState::Ok);
    assert!(app_read(
        &FLASH,
        app.base,
        key,
        0x11f,
        &mut workspace()[..37]
    ));
    assert!(!app_read(
        &FLASH,
        app.base,
        key,
        u32::MAX,
        &mut workspace()[..2]
    ));
    image()[(app.base + cfg_address + 12) as usize] ^= 1;
    assert!(app_check(&FLASH, &mut app, workspace()) == AppState::BadCrc);
    image()[(app.base + cfg_address + 12) as usize] ^= 1;
    image()[(app.base + app.files[0].address + app_length - 32) as usize..][..32].fill(255);
    assert!(app_check(&FLASH, &mut app, workspace()) == AppState::Incomplete);
    image().copy_from_slice(saved());
    ASSERTIONS.set(ASSERTIONS.read() + 1);
    FIXTURE_GROUPS.set(FIXTURE_GROUPS.read() + 1);
}

fn test_fixture_updater() {
    let mut request: Request = [0; 80];
    let mut count = 0;
    let mut u = Updater {
        load: 0,
        size: 0,
        stream_end: 0,
        crc: 0,
    };
    if !load_fixture("smk37-stock.bin") {
        return;
    }
    saved().copy_from_slice(image());
    assert!(request_scan(&FLASH, 16, &mut request, Some(&mut count)) && count >= 1);
    assert!(request_valid(&request));
    assert!(le16(&request, 2) == 0x5a0d && le32(&request, 72) == 0xb9000);
    let first = request;
    assert!(updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    assert!(
        u.load == UPDATE_LOAD
            && u.size == 23324
            && u.crc == 0xb295
            && u.stream_end == 0xb9000 + 19969
    );
    assert!(crc16(0, &updater_output()[..u.size as usize]) == 0xb295);
    request[3] ^= 1;
    assert!(!request_valid(&request));
    request[3] ^= 1;
    request[2..4].fill(0);
    let crc = crc16(0, &request[2..]);
    put_le16(&mut request, 0, crc);
    assert!(!request_valid(&request));
    request = first;
    image()[0xb9000] ^= 1;
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    image().copy_from_slice(saved());
    image()[0xb9020] ^= 1;
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    image().copy_from_slice(saved());
    put_le32(image(), 0xb9040, u32::MAX);
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    image().copy_from_slice(saved());
    put_le32(image(), 0xb9044, u32::MAX);
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    image().copy_from_slice(saved());
    put_le32(image(), 0xb9028, UPDATE_LOAD + 4);
    let crc = crc16(0, &image()[0xb9022..][..30]);
    put_le16(image(), 0xb9020, crc);
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    image().copy_from_slice(saved());
    assert!(!updater_decode(
        &FLASH,
        &request,
        &mut updater_output()[..10],
        workspace(),
        &mut u
    ));
    put_le32(&mut request, 72, u32::MAX);
    let crc = crc16(0, &request[2..]);
    put_le16(&mut request, 0, crc);
    assert!(!updater_decode(
        &FLASH,
        &request,
        updater_output(),
        workspace(),
        &mut u
    ));
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn test_requests() {
    let (mut request, mut first): (Request, Request) = ([0; 80], [0; 80]);
    let mut uart = [0; 19];
    let mut count = 0;
    reset_model();
    put_le16(&mut first, 2, 0x5a0d);
    put_le16(&mut first, 6, 0x5441);
    put_le32(&mut first, 72, 0xb9000);
    let crc = crc16(0, &first[2..]);
    put_le16(&mut first, 0, crc);
    assert!(request_valid(&first));
    request.copy_from_slice(&first);
    request[3] ^= 1;
    assert!(!request_valid(&request));
    put_le16(&mut request, 2, 0);
    let crc = crc16(0, &request[2..]);
    put_le16(&mut request, 0, crc);
    assert!(!request_valid(&request));
    image().fill(255);
    image()[0xfef00..][..80].copy_from_slice(&first);
    assert!(request_scan(&FLASH, 16, &mut request, None));
    assert!(request == first);
    image()[0xfdf00..][..80].copy_from_slice(&first);
    // Older valid record must not win.
    put_le16(image(), 0xfef02, 0);
    let crc = crc16(0, &image()[0xfef02..][..78]);
    put_le16(image(), 0xfef00, crc);
    assert!(!request_scan(&FLASH, 16, &mut request, None));
    assert!(le16(&request, 2) == 0);
    assert!(!request_scan(&FLASH, 16, &mut request, Some(&mut count)));
    assert!(count == 2);
    assert!(le16(&request, 2) == 0);
    image()[0xfdf00..][..80].fill(255);
    image()[0xfef00..][..80].fill(255);
    image()[0xfff00..][..80].copy_from_slice(&first);
    assert!(!request_scan(&FLASH, 16, &mut request, None));
    image().fill(255);
    image()[0xfd00..][..80].copy_from_slice(&first);
    assert!(request_scan(&FLASH, 1, &mut request, None));
    uart.fill(0);
    uart[..11].copy_from_slice(b"UARTUPDATE\0");
    assert!(uart_request(&uart));
    uart[10] = b'!';
    assert!(!uart_request(&uart));
    uart.copy_from_slice(b"UART_UPDATE_CUSTOM\0");
    assert!(uart_request(&uart));
    uart[..4].fill(0);
    assert!(!uart_request(&uart));
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn test_mount_header_refresh() {
    let (mut initial, mut app) = (Header::EMPTY, App::EMPTY);
    if !load_fixture("fm1-stock.bin") {
        return;
    }
    assert!(header_read(&FLASH, &mut initial));
    let mut fresh = Header {
        bytes: [0xa5; 32],
        alignment: 0xa5,
        filesystem_size: 0xa5a5_a5a5,
    };
    assert!(app_mount_header(&FLASH, 0x980f, &mut app, &mut fresh));
    assert!(fresh.bytes == initial.bytes);
    assert!(fresh.alignment == initial.alignment);
    assert!(fresh.filesystem_size == initial.filesystem_size);
    let mut changed = initial.bytes;
    changed[4] ^= 0x5a;
    changed[13] = 8;
    let crc = crc16(0, &changed[2..]);
    put_le16(&mut changed, 0, crc);
    let mut encoded = changed;
    enc(0xffff, &mut encoded);
    image()[..32].copy_from_slice(&encoded);
    assert!(app_mount_header(&FLASH, 0x980f, &mut app, &mut fresh));
    assert!(fresh.bytes == changed);
    assert!(fresh.alignment == 8);
    assert!(fresh.bytes != initial.bytes);
    assert!(app.base == 0x4000 && app.entry == 0x0200_0120);
    image()[0] ^= 1;
    assert!(!app_mount_header(&FLASH, 0x980f, &mut app, &mut fresh));
    assert!(fresh.alignment == 0);
    assert!(app.base == 0 && app.entry == 0);
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn test_mount_without_app() {
    let mut app = App::EMPTY;
    if !load_fixture("smk37-stock.bin") {
        return;
    }
    saved().copy_from_slice(image());
    encode_record(
        0x4020,
        32,
        true,
        0x980f,
        b"other.bin",
        0x120,
        619704,
        0x82,
        0xf0da,
    );
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.base == 0 && app.entry == 0 && app.state == AppState::NotFound);
    image().copy_from_slice(saved());
    encode_record(0x4020, 32, true, 0x980f, b"app.bin", 0x120, 0, 0x82, 0);
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.base == 0 && app.entry == 0);
    image().copy_from_slice(saved());
    image()[0x4000] ^= 1;
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.base == 0 && app.entry == 0);
    image().copy_from_slice(saved());
    encode_record(0x4020, 32, true, 0x980f, b"app.bin", u32::MAX, 32, 0x82, 0);
    assert!(!app_mount(&FLASH, 0x980f, &mut app));
    assert!(app.base == 0 && app.entry == 0);
    image().copy_from_slice(saved());
    // A bad record CRC ends the table: the files before it still mount.
    image()[0x4040] ^= 1;
    assert!(app_mount(&FLASH, 0x980f, &mut app) && app.file_count == 1);
    image().copy_from_slice(saved());
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

fn guard_fixture() {
    image().fill(255);
    for i in 0..0x22c {
        image()[0xc1000 + i] = (i * 37 + 5) as u8;
    }
    image()[0xc1224..][..8].copy_from_slice(b"trimdat\0");
    image()[0xfff00] = 0x12;
    image()[0xfff40] = 0x34;
    reset_model();
}

fn test_guard() {
    let mut record = [0; 0x22c];
    guard_fixture();
    record.copy_from_slice(&image()[0xc1000..][..0x22c]);
    assert!(guard(&NOR, false, workspace()) == Guard::Unchanged && model().operations == 0);
    assert!(guard(&NOR, true, workspace()) == Guard::BackedUp);
    assert!(
        model().erases == 0 && model().first_program == 0xff010 && model().last_program == 0xff000
    );
    assert!(image()[0xff000..][..8] == *b"TRIMCOPY");
    assert!(le32(image(), 0xff008) == 0xc1000);
    assert!(le16(image(), 0xff00c) == 0x22c && le16(image(), 0xff00e) == crc16(0, &record));
    assert!(image()[0xff010..][..0x22c] == record);
    assert!(image()[0xfff00] == 0x12 && image()[0xfff40] == 0x34);
    saved().copy_from_slice(image());
    reset_model();
    image()[0xc1000] ^= 1;
    assert!(guard(&NOR, true, workspace()) == Guard::Unchanged && model().operations == 0);
    image()[0xc1224] = 0;
    assert!(guard(&NOR, true, workspace()) == Guard::Restored);
    assert!(
        model().erases == 1 && model().erase_address[0] == 0xc1000 && model().erase_size[0] == 4096
    );
    assert!(image()[0xc1000..][..0x22c] == record);
    assert!(image()[0xff000..][..4096] == saved()[0xff000..][..4096]);
    for failure in 1..=4 {
        image().copy_from_slice(saved());
        image()[0xc1224] = 0;
        reset_model();
        model().fail_at = failure;
        assert!(guard(&NOR, true, workspace()) == Guard::Error);
        assert!(image()[0xff000..][..4096] == saved()[0xff000..][..4096]);
        reset_model();
        assert!(guard(&NOR, true, workspace()) == Guard::Restored);
        assert!(image()[0xc1000..][..0x22c] == record);
    }
    for failure in 1..=4 {
        guard_fixture();
        model().fail_at = failure;
        assert!(guard(&NOR, true, workspace()) == Guard::Error);
        reset_model();
        if failure == 1 {
            assert!(guard(&NOR, true, workspace()) == Guard::BackedUp);
        } else {
            assert!(guard(&NOR, true, workspace()) == Guard::Unchanged && model().operations == 0);
        }
    }
    guard_fixture();
    image()[0xff000] = 0;
    assert!(guard(&NOR, true, workspace()) == Guard::Unchanged && model().operations == 0);
    image().copy_from_slice(saved());
    image()[0xc1224] = 0;
    image()[0xff00e] ^= 1;
    reset_model();
    assert!(guard(&NOR, true, workspace()) == Guard::Unchanged && model().operations == 0);
    guard_fixture();
    model().bad_read = true;
    assert!(guard(&NOR, true, workspace()) == Guard::Error && model().operations == 0);
    ASSERTIONS.set(ASSERTIONS.read() + 1);
}

#[test]
fn formats() {
    test_primitives();
    test_banks();
    test_lz4();
    test_fixture("fm1-stock.bin", 0xf984_96a0, 581564, 0x5d48, 0x8e0dc);
    test_fixture("smk37-stock.bin", 0x8e57_bc96, 619704, 0xf0da, 0x975d8);
    test_fixture("smk37-snoop37.bin", 0x6475_373c, 619520, 0x6283, 0x97520);
    test_fixture("smk37-baudgirl.bin", 0x1eab_fb0a, 748520, 0xfbdb, 0xb6d08);
    test_fixture_updater();
    test_requests();
    test_mount_header_refresh();
    test_mount_without_app();
    test_guard();
    println!(
        "core: {} groups passed ({} private images checked, malformed inputs, NOR faults)",
        ASSERTIONS.read(),
        FIXTURE_GROUPS.read()
    );
}
