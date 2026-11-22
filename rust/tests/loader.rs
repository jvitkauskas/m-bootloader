// SPDX-License-Identifier: MIT
//! The flash loader (docs/SPEC.md §10), with the test as the ROM, the chip and the screen that it is given: every
//! command of its protocol, and what it asks of those callbacks while it answers.
use crate::Global;
use crate::formats::{
    CFlash, CNor, be16, be32, crc16, crc32, put_be16, put_be32, put_le16, put_le32, range,
};
use crate::loader::{Services, State, loader_init};
use core::ffi::c_void;
use core::mem::MaybeUninit;
use core::ptr::{fn_addr_eq, null};
use core::slice;
use std::fs::File;
use std::io::{ErrorKind, Read};

const FLASH_SIZE: u32 = 0x100000;

/// Something a callback was asked for. Only the kind and the pointer are looked at; the rest is for whoever looks
/// into a failure.
#[derive(Clone, Copy)]
#[allow(dead_code)]
struct Event {
    kind: u8,
    address: u32,
    size: u32,
    pointer: *const c_void,
}

/// What the callbacks count and keep, and what they are told to do.
struct Test {
    sent_size: u32,
    receive_size: u32,
    receive_pos: u32,
    sent_calls: u32,
    receive_calls: u32,
    program_count: u32,
    erase_count: u32,
    mutation_after_receive: u32,
    read_count: u32,
    start_count: u32,
    finish_count: u32,
    program_address: [u32; 32],
    program_size: [u32; 32],
    erase_address: u32,
    erase_size: u32,
    pending_address: u32,
    pending_size: u32,
    pending_pointer: *mut c_void,
    pending: bool,
    read_failure: bool,
    program_failure: bool,
    erase_failure: bool,
    init_success: bool,
    screen_init_count: u32,
    screen_row_count: u32,
    screen_progress_count: u32,
    screen_board: u32,
    screen_row: u32,
    screen_fg: u16,
    screen_bg: u16,
    screen_fill: u16,
    screen_text: [u8; 20],
    screen_text_size: u32,
    screen_percent: u32,
    tests: u32,
    fixture_groups: u32,
    events: [Event; 10000],
    event_count: usize,
}

impl Test {
    fn event(&mut self, kind: u8, address: u32, size: u32, pointer: *const c_void) {
        assert!(self.event_count < self.events.len());
        self.events[self.event_count] = Event {
            kind,
            address,
            size,
            pointer,
        };
        self.event_count += 1;
    }
}

static LOADER: Global<MaybeUninit<State>> = Global::new(MaybeUninit::zeroed());
static FLASH_BYTES: Global<[u8; FLASH_SIZE as usize]> = Global::new([0; FLASH_SIZE as usize]);
static SENT_BYTES: Global<[u8; FLASH_SIZE as usize + 16]> =
    Global::new([0; FLASH_SIZE as usize + 16]);
static RECEIVE_DATA: Global<[u8; 65536]> = Global::new([0; 65536]);
static COMMAND: Global<[u8; 31]> = Global::new([0; 31]);
static ANSWER: Global<[u8; 16]> = Global::new([0; 16]);
// All zeros to start with, which each of its fields can be.
static T: Global<Test> = Global::new(unsafe { core::mem::zeroed() });

// Safety, for these: the test is one thread, and what one of these gives is used at once, or kept only until the
// next call that could ask for it again. The loader is given its state, the command and the answer for as long as
// a command takes, and the callbacks that it calls meanwhile use none of the three through these: they read the
// command with `COMMAND.read()`, and of the answer they take only where it is (`answer_pointer`).
fn state() -> &'static mut State {
    // `loader_init` made it a State, before anything else was done.
    unsafe { LOADER.get().assume_init_mut() }
}

fn flash_bytes() -> &'static mut [u8; FLASH_SIZE as usize] {
    unsafe { FLASH_BYTES.get() }
}

fn sent_bytes() -> &'static mut [u8; FLASH_SIZE as usize + 16] {
    unsafe { SENT_BYTES.get() }
}

fn receive_data() -> &'static mut [u8; 65536] {
    unsafe { RECEIVE_DATA.get() }
}

fn command() -> &'static mut [u8; 31] {
    unsafe { COMMAND.get() }
}

fn answer() -> &'static mut [u8; 16] {
    unsafe { ANSWER.get() }
}

fn t() -> &'static mut Test {
    unsafe { T.get() }
}

/// `size` bytes of the flash, from `address`.
fn flash(address: u32, size: u32) -> &'static mut [u8] {
    &mut flash_bytes()[address as usize..][..size as usize]
}

/// Where the flash is: what the chip's callbacks are to be called with.
fn flash_pointer() -> *mut c_void {
    flash_bytes().as_mut_ptr().cast()
}

/// Where the answer is, that `dispatch` gives the loader.
fn answer_pointer() -> *const c_void {
    (&raw const ANSWER).cast()
}

/// # Safety
/// `data` is `size` bytes.
unsafe extern "C" fn send_callback(data: *const c_void, size: u32) -> i32 {
    let (t, sent_bytes) = (t(), sent_bytes());
    assert!(size != 0 && size <= 512);
    assert!(size <= sent_bytes.len() as u32 - t.sent_size);
    if be16(&COMMAND.read(), 15) != 0xfd05 {
        assert!(data == answer_pointer());
    }
    // §10.1 ROM buffer.
    if t.pending {
        assert!(data != t.pending_pointer.cast_const());
    }
    let bytes = unsafe { slice::from_raw_parts(data.cast::<u8>(), size as usize) };
    sent_bytes[t.sent_size as usize..][..size as usize].copy_from_slice(bytes);
    t.sent_size += size;
    t.sent_calls += 1;
    t.event(b'S', 0, size, data);
    // Return convention intentionally unspecified.
    -123
}

/// # Safety
/// `data` is room for `size` bytes.
unsafe extern "C" fn receive_callback(data: *mut c_void, size: u32) -> i32 {
    let t = t();
    assert!(size != 0 && size <= 512);
    assert!(size <= t.receive_size - t.receive_pos);
    let bytes = unsafe { slice::from_raw_parts_mut(data.cast::<u8>(), size as usize) };
    bytes.copy_from_slice(&receive_data()[t.receive_pos as usize..][..size as usize]);
    t.receive_pos += size;
    t.receive_calls += 1;
    t.event(b'R', 0, size, data);
    -123
}

/// # Safety
/// `dst` is room for `size` bytes.
unsafe extern "C" fn read_callback(
    ctx: *mut c_void,
    address: u32,
    dst: *mut c_void,
    size: u32,
) -> bool {
    let t = t();
    assert!(ctx == flash_pointer());
    assert!(range(address, size, FLASH_SIZE));
    assert!(!t.pending);
    t.read_count += 1;
    if t.read_failure {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts_mut(dst.cast::<u8>(), size as usize) };
    bytes.copy_from_slice(flash(address, size));
    true
}

/// # Safety
/// `dst` is room for `size` bytes, until `read_finish` has returned.
unsafe extern "C" fn read_start(address: u32, dst: *mut c_void, size: u32) -> bool {
    let t = t();
    assert!(!t.pending);
    assert!(size != 0 && size <= 4096);
    assert!(range(address, size, FLASH_SIZE));
    if t.read_failure {
        return false;
    }
    t.pending = true;
    t.pending_address = address;
    t.pending_size = size;
    t.pending_pointer = dst;
    t.start_count += 1;
    t.event(b'A', address, size, dst);
    true
}

extern "C" fn read_finish() -> bool {
    let t = t();
    assert!(t.pending);
    // Safety: what `read_start` was given.
    let bytes = unsafe {
        slice::from_raw_parts_mut(t.pending_pointer.cast::<u8>(), t.pending_size as usize)
    };
    bytes.copy_from_slice(flash(t.pending_address, t.pending_size));
    t.event(b'F', t.pending_address, t.pending_size, t.pending_pointer);
    t.pending = false;
    t.finish_count += 1;
    true
}

/// # Safety
/// `src` is `size` bytes.
unsafe extern "C" fn program_callback(
    ctx: *mut c_void,
    address: u32,
    src: *const c_void,
    size: u32,
) -> bool {
    let t = t();
    assert!(ctx == flash_pointer());
    assert!(range(address, size, FLASH_SIZE));
    assert!(size != 0 && size <= 256 - (address & 255));
    assert!(t.receive_pos == t.receive_size);
    assert!(t.program_count < 32);
    t.program_address[t.program_count as usize] = address;
    t.program_size[t.program_count as usize] = size;
    t.program_count += 1;
    t.mutation_after_receive = t.receive_pos;
    if t.program_failure {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(src.cast::<u8>(), size as usize) };
    for (cell, &byte) in flash(address, size).iter_mut().zip(bytes) {
        assert!(*cell & byte == byte);
        *cell &= byte;
    }
    t.event(b'P', address, size, src);
    true
}

extern "C" fn erase_callback(ctx: *mut c_void, address: u32, size: u32) -> bool {
    let t = t();
    assert!(ctx == flash_pointer());
    assert!(size == 4096 || size == 65536);
    assert!(address & (size - 1) == 0);
    assert!(range(address, size, FLASH_SIZE));
    t.erase_count += 1;
    t.erase_address = address;
    t.erase_size = size;
    if t.erase_failure {
        return false;
    }
    flash(address, size).fill(255);
    t.event(b'E', address, size, null());
    true
}

extern "C" fn jedec_callback() -> u32 {
    0xeb6014
}

extern "C" fn key_callback() -> u16 {
    0x980f
}

extern "C" fn uid_callback(uid: &mut [u8; 8]) {
    *uid = [0x81, 0, 0x23, 0x45, 0x67, 0x89, 0xab, 0xff];
}

extern "C" fn init_callback(board: u32) -> bool {
    let t = t();
    assert!(board == 1 || board == 2);
    t.screen_init_count += 1;
    t.screen_board = board;
    t.init_success
}

/// # Safety
/// `text` is `size` bytes.
unsafe extern "C" fn row_callback(row: u32, fg: u16, bg: u16, text: *const u8, size: u32) {
    let t = t();
    assert!(row <= 14 && size <= 20);
    t.screen_row = row;
    t.screen_fg = fg;
    t.screen_bg = bg;
    t.screen_text_size = size;
    let text = unsafe { slice::from_raw_parts(text, size as usize) };
    t.screen_text[..size as usize].copy_from_slice(text);
    t.screen_row_count += 1;
}

extern "C" fn progress_callback(percent: u32, color: u16) {
    let t = t();
    assert!(percent <= 100);
    t.screen_percent = percent;
    t.screen_fill = color;
    t.screen_progress_count += 1;
}

/// The chip as the loader is given it: a struct of callbacks, which `nor_ctx` reads too. Nothing else here names
/// `CFlash` or `CNor`, or a field of theirs.
fn nor() -> CNor {
    CNor {
        flash: CFlash {
            ctx: flash_pointer(),
            capacity: FLASH_SIZE,
            read: read_callback,
        },
        program: program_callback,
        erase: erase_callback,
    }
}

/// What the loader calls the chip's callbacks with.
fn nor_ctx(io: &Services) -> *mut c_void {
    io.nor.flash.ctx
}

fn services() -> Services {
    Services {
        nor: nor(),
        send: send_callback,
        receive: receive_callback,
        read_start: Some(read_start),
        read_finish: Some(read_finish),
        jedec: jedec_callback,
        key: key_callback,
        uid: uid_callback,
        screen_init: Some(init_callback),
        screen_row: Some(row_callback),
        screen_progress: Some(progress_callback),
        entry: 0x01c2_0000,
        cpu_mhz: 96,
    }
}

/// `loader_init`, of the test's loader.
fn init(io: &Services) {
    // Safety: room for a State.
    unsafe { loader_init(LOADER.get().as_mut_ptr(), io) }
}

fn reset_transfer() {
    let t = t();
    assert!(!t.pending);
    (t.sent_size, t.receive_size, t.receive_pos) = (0, 0, 0);
    (t.sent_calls, t.receive_calls) = (0, 0);
    (t.program_count, t.erase_count, t.mutation_after_receive) = (0, 0, 0);
    (t.read_count, t.start_count, t.finish_count) = (0, 0, 0);
    t.event_count = 0;
    (t.read_failure, t.program_failure, t.erase_failure) = (false, false, false);
    *answer() = [0xa5; 16];
}

fn new_command(op: u16, inward: bool, transfer: u32) {
    reset_transfer();
    let command = command();
    *command = [0; 31];
    put_le32(command, 0, 0x4342_5355);
    put_le32(command, 4, 0x1234_5678);
    put_le32(command, 8, transfer);
    command[12] = if inward { 0x80 } else { 0 };
    command[14] = 16;
    command[15..].fill(255);
    put_be16(command, 15, op);
}

fn dispatch() {
    assert!(state().dispatch(command(), answer()) == 1);
    assert!(!t().pending);
    assert!(t().receive_pos == t().receive_size);
    let (command, answer) = (command(), answer());
    if be16(command, 15) == 0xfb04 || be16(command, 15) == 0xfc44 {
        assert!(answer[0] == command[15] && answer[1] == command[16]);
        for &byte in &answer[2..] {
            assert!(byte == 0);
        }
    }
}

fn reply(op: u16) {
    new_command(op, true, 16);
    dispatch();
    assert!(t().sent_size == 16 && t().sent_calls == 1 && t().receive_calls == 0);
    assert!(sent_bytes()[0] == (op >> 8) as u8 && sent_bytes()[1] == op as u8);
}

fn take_status() -> u8 {
    reply(0xfc46);
    sent_bytes()[2]
}

fn expected_reply(expected: &[u8; 16]) {
    assert!(sent_bytes()[..16] == *expected);
}

fn test_identity() {
    let key = [0xfc, 9, 0, 0, 0, 0, 0x3c, 0x20, 0, 0, 0, 0, 0, 0, 0, 0];
    let device = [0xfc, 10, 3, 0, 0x14, 0x60, 0xeb, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    let buffer = [0xfc, 0x14, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    let identity = [
        0xfc, 0x40, b'M', b'F', b'L', b'R', b'1', b'.', b'4', 0, 1, 0xc2, 0, 0, 15, 96,
    ];
    reply(0xfc09);
    expected_reply(&key);
    reply(0xfc0a);
    expected_reply(&device);
    reply(0xfc14);
    expected_reply(&buffer);
    reply(0xfc40);
    expected_reply(&identity);
    reply(0xfc0b);
    assert!(sent_bytes()[2] == 0xeb && sent_bytes()[3] == 0x60 && sent_bytes()[4] == 0x14);
    reply(0xfc42);
    assert!(sent_bytes()[2] == 0x81 && sent_bytes()[3] == 0 && sent_bytes()[9] == 255);
    assert!(take_status() == 0);
    assert!(take_status() == 0);
    for i in 0..=16 {
        new_command(0xfc40, true, i);
        dispatch();
        assert!(t().sent_size == i && t().sent_calls == u32::from(i != 0));
    }
    t().tests += 1;
}

fn fixture(name: &str, crc: u32) {
    let f = File::open(format!("testdata/{name}"));
    if f.as_ref()
        .is_err_and(|error| error.kind() == ErrorKind::NotFound)
    {
        println!("SKIP: {name} not present (loader raw CRC)");
        return;
    }
    assert!(f.is_ok());
    let mut f = f.unwrap();
    // All of the flash, and then the end of the file.
    assert!(f.read_exact(flash_bytes()).is_ok() && matches!(f.read(&mut [0]), Ok(0)));
    drop(f);
    new_command(0xfc41, true, 16);
    put_be32(command(), 17, 0);
    put_be32(command(), 21, FLASH_SIZE);
    dispatch();
    assert!(t().sent_size == 16 && be32(sent_bytes(), 2) == crc);
    assert!(t().start_count == 256 && t().finish_count == 256);
    t().fixture_groups += 1;
}

fn expect_overlap() {
    let t = t();
    let events = &t.events[..t.event_count];
    let mut saw_overlap = false;
    for i in 0..events.len().saturating_sub(3) {
        if events[i].kind == b'F' && events[i + 1].kind == b'A' && events[i + 2].kind == b'S' {
            assert!(events[i].pointer == events[i + 2].pointer);
            assert!(events[i].pointer != events[i + 1].pointer);
            saw_overlap = true;
        }
    }
    assert!(saw_overlap);
}

fn test_streaming() {
    for (i, byte) in flash_bytes().iter_mut().enumerate() {
        *byte = ((i * 47) ^ (i >> 8)) as u8;
    }
    for n in [0, 1, 31, 32, 511, 512, 513, 4096, 4097, 16384, 65535] {
        let address = 0x4003;
        new_command(0xfd05, true, n);
        put_be32(command(), 17, address);
        put_be16(command(), 21, n as u16);
        dispatch();
        assert!(t().sent_size == n && sent_bytes()[..n as usize] == *flash(address, n));
        assert!(t().sent_calls == (n + 511) / 512);
        assert!(t().start_count == (n + 4095) / 4096);
        if n > 4096 {
            expect_overlap();
        }
        new_command(0xfc0e, true, 16);
        put_be32(command(), 17, address);
        put_be16(command(), 21, n as u16);
        dispatch();
        assert!(t().sent_size == 16 && be16(sent_bytes(), 2) == crc16(0, flash(address, n)));
        new_command(0xfc41, true, 16);
        put_be32(command(), 17, address);
        put_be32(command(), 21, n);
        dispatch();
        assert!(
            t().sent_size == 16
                && be32(sent_bytes(), 2) == crc32(u32::MAX, flash(address, n)) ^ u32::MAX
        );
        if n == 0 {
            assert!(t().start_count == 0 && t().finish_count == 0);
        }
    }
    state().io.read_start = None;
    state().io.read_finish = None;
    new_command(0xfd05, true, 16384);
    put_be32(command(), 17, 7);
    put_be16(command(), 21, 16384);
    dispatch();
    assert!(
        t().start_count == 0
            && t().read_count == 4
            && t().sent_size == 16384
            && sent_bytes()[..16384] == *flash(7, 16384)
    );
    state().io.read_start = Some(read_start);
    state().io.read_finish = Some(read_finish);
    fixture("fm1-stock.bin", 0xf984_96a0);
    fixture("smk37-stock.bin", 0x8e57_bc96);
    fixture("smk37-snoop37.bin", 0x6475_373c);
    fixture("smk37-baudgirl.bin", 0x1eab_fb0a);
    t().tests += 1;
}

fn prepare_write(address: u32, length: u32) {
    new_command(0xfb04, false, length);
    t().receive_size = length;
    let (command, receive_data) = (command(), receive_data());
    assert!(length as usize <= receive_data.len());
    for i in 0..length {
        receive_data[i as usize] = (i * 7 + 19) as u8;
    }
    put_be32(command, 17, address);
    put_be16(command, 21, length as u16);
    command[23] = 0;
    put_le16(command, 24, crc16(0, &receive_data[..length as usize]));
}

fn test_programming() {
    for alignment in 0..2 {
        for n in [0, 1, 255, 256, 257, 4096] {
            let (address, mut done) = (0x4000 + alignment * 13, 0);
            flash(0x3000, 0x4000).fill(255);
            prepare_write(address, n);
            dispatch();
            assert!(
                t().sent_size == 0
                    && t().erase_count == 0
                    && t().receive_pos == n
                    && *flash(address, n) == receive_data()[..n as usize]
            );
            assert!(t().mutation_after_receive == n || n == 0);
            for i in 0..t().program_count as usize {
                assert!(t().program_address[i] == address + done);
                assert!(t().program_size[i] == (n - done).min(256 - ((address + done) & 255)));
                done += t().program_size[i];
            }
            assert!(done == n);
            assert!(
                flash_bytes()[address as usize - 1] == 255
                    && flash_bytes()[(address + n) as usize] == 255
            );
            assert!(take_status() == 0);
        }
    }
    // Exact mixed-endian ASCII vector from §10.2.
    prepare_write(0x4000, 9);
    receive_data()[..9].copy_from_slice(b"123456789");
    put_le16(command(), 24, 0x31c3);
    let write_cdb = [
        0xfb, 4, 0, 0, 0x40, 0, 0, 9, 0, 0xc3, 0x31, 255, 255, 255, 255, 255,
    ];
    assert!(command()[15..] == write_cdb);
    flash(0x4000, 256).fill(255);
    dispatch();
    assert!(*flash(0x4000, 9) == *b"123456789");
    prepare_write(0x4000, 4097);
    dispatch();
    assert!(t().program_count == 0 && t().erase_count == 0 && t().receive_pos == 4097);
    assert!(state().error == 1);
    reply(0xfc14);
    assert!(state().error == 1);
    assert!(take_status() == 1);
    assert!(take_status() == 0);
    prepare_write(0x4000, 65535);
    dispatch();
    assert!(t().program_count == 0 && t().receive_calls == 128);
    assert!(take_status() == 1);
    prepare_write(0x4000, 4096);
    command()[24] ^= 1;
    dispatch();
    assert!(t().program_count == 0 && t().erase_count == 0 && state().error == 2);
    reply(0xfc40);
    assert!(state().error == 2);
    assert!(take_status() == 2);
    assert!(take_status() == 0);
    prepare_write(0x4000, 256);
    command()[24] ^= 1;
    dispatch();
    assert!(state().error == 2);
    prepare_write(0x6000, 256);
    flash(0x6000, 256).fill(255);
    dispatch();
    assert!(state().error == 2 && t().program_count == 1);
    assert!(take_status() == 2);
    t().tests += 1;
}

fn test_rejections() {
    for (i, op) in [0xfb00, 0xfb01].into_iter().enumerate() {
        new_command(op, true, 16);
        put_be32(command(), 17, 1);
        dispatch();
        assert!(t().erase_count == 0);
        assert!(take_status() == 3);
        new_command(op, true, 16);
        put_be32(command(), 17, FLASH_SIZE);
        dispatch();
        assert!(t().erase_count == 0);
        assert!(take_status() == 3);
        new_command(op, true, 16);
        put_be32(command(), 17, 0x10000);
        dispatch();
        assert!(t().erase_count == 1);
        assert!(
            t().erase_address == 0x10000 && t().erase_size == if i != 0 { 4096 } else { 65536 }
        );
        assert!(t().sent_size == 16);
        assert!(take_status() == 0);
    }
    prepare_write(FLASH_SIZE - 1, 2);
    dispatch();
    assert!(t().program_count == 0);
    assert!(take_status() == 3);
    prepare_write(u32::MAX, 2);
    dispatch();
    assert!(t().program_count == 0);
    assert!(take_status() == 3);
    new_command(0xfd05, true, 2);
    put_be32(command(), 17, u32::MAX);
    put_be16(command(), 21, 2);
    dispatch();
    assert!(t().start_count == 0 && t().read_count == 0);
    assert!(take_status() == 3);
    new_command(0xfc41, true, 16);
    put_be32(command(), 17, 0x4000);
    put_be32(command(), 21, u32::MAX);
    dispatch();
    assert!(t().start_count == 0);
    assert!(take_status() == 3);
    new_command(0xfc0e, true, 16);
    put_be32(command(), 17, FLASH_SIZE);
    put_be16(command(), 21, 1);
    dispatch();
    assert!(t().start_count == 0);
    assert!(take_status() == 3);
    prepare_write(0x4000, 8);
    put_be16(command(), 21, 7);
    dispatch();
    assert!(t().program_count == 0 && t().receive_pos == 8);
    assert!(take_status() == 7);
    prepare_write(0x4000, 8);
    command()[14] = 15;
    dispatch();
    assert!(t().program_count == 0 && t().receive_pos == 8);
    assert!(take_status() == 7);
    new_command(0xfb04, true, 8);
    put_be16(command(), 21, 8);
    dispatch();
    assert!(t().program_count == 0 && t().receive_calls == 0);
    assert!(take_status() == 7);
    new_command(0xfd05, false, 8);
    t().receive_size = 8;
    put_be16(command(), 21, 8);
    dispatch();
    assert!(t().receive_pos == 8 && t().start_count == 0);
    assert!(take_status() == 7);
    new_command(0xfd05, true, 8);
    put_be16(command(), 21, 7);
    dispatch();
    assert!(t().start_count == 0);
    assert!(take_status() == 7);
    new_command(0xfc40, true, 17);
    dispatch();
    assert!(t().sent_size == 16);
    assert!(take_status() == 7);
    for forbidden in [0xfb02, 0xfc12] {
        new_command(forbidden, true, 16);
        dispatch();
        assert!(t().erase_count == 0 && t().program_count == 0);
        assert!(take_status() == 5);
        new_command(forbidden, false, 65535);
        t().receive_size = 65535;
        dispatch();
        assert!(t().receive_pos == 65535 && t().erase_count == 0 && t().program_count == 0);
        assert!(take_status() == 5);
    }
    for deferred in [0xfb06, 0xfd07, 0xfb08, 0x1234] {
        new_command(deferred, true, 16);
        assert!(state().dispatch(command(), answer()) == 0);
        assert!(
            t().sent_calls == 0
                && t().receive_calls == 0
                && t().erase_count == 0
                && t().program_count == 0
        );
        for &byte in answer().iter() {
            assert!(byte == 0xa5);
        }
    }
    new_command(0xfd05, true, 8);
    put_be32(command(), 17, 0);
    put_be16(command(), 21, 8);
    t().read_failure = true;
    dispatch();
    assert!(t().sent_calls == 0);
    assert!(take_status() == 4);
    prepare_write(0x7000, 256);
    flash(0x7000, 256).fill(255);
    t().program_failure = true;
    dispatch();
    assert!(take_status() == 4);
    new_command(0xfb01, true, 16);
    put_be32(command(), 17, 0x7000);
    t().erase_failure = true;
    dispatch();
    assert!(take_status() == 4);
    t().tests += 1;
}

fn prepare_text(row: u32, size: u32) {
    new_command(0xfc44, false, size);
    t().receive_size = size;
    for i in 0..size {
        receive_data()[i as usize] = b'A' + (i % 26) as u8;
    }
    command()[17] = row as u8;
    put_be16(command(), 18, 0xf81f);
    put_be16(command(), 20, 0x07e0);
    put_be16(command(), 22, size as u16);
}

fn init_screen(board: u32) {
    new_command(0xfc43, true, 16);
    command()[17] = board as u8;
    dispatch();
}

fn test_display() {
    t().init_success = true;
    (
        t().screen_init_count,
        t().screen_row_count,
        t().screen_progress_count,
    ) = (0, 0, 0);
    assert!(state().screen == 0);
    prepare_text(0, 20);
    dispatch();
    assert!(t().screen_row_count == 0 && t().receive_pos == 20);
    init_screen(0);
    assert!(t().screen_init_count == 0 && sent_bytes()[2] == 0);
    init_screen(255);
    assert!(t().screen_init_count == 0 && sent_bytes()[2] == 0);
    init_screen(1);
    assert!(
        t().screen_init_count == 1
            && t().screen_board == 1
            && state().screen != 0
            && sent_bytes()[2] == 1
    );
    prepare_text(14, 20);
    dispatch();
    assert!(
        t().screen_row_count == 1
            && t().screen_row == 14
            && t().screen_fg == 0xf81f
            && t().screen_bg == 0x07e0
    );
    assert!(t().screen_text_size == 20 && t().screen_text == receive_data()[..20]);
    prepare_text(0, 0);
    dispatch();
    assert!(t().screen_row_count == 2 && t().receive_calls == 0 && t().screen_text_size == 0);
    prepare_text(15, 20);
    dispatch();
    assert!(t().screen_row_count == 2 && t().receive_pos == 20);
    assert!(take_status() == 6);
    prepare_text(2, 65535);
    dispatch();
    assert!(t().screen_row_count == 2 && t().receive_pos == 65535);
    assert!(take_status() == 6);
    prepare_text(2, 21);
    dispatch();
    assert!(t().screen_row_count == 2 && t().receive_pos == 21);
    assert!(take_status() == 6);
    prepare_text(2, 8);
    put_be16(command(), 22, 7);
    dispatch();
    assert!(t().screen_row_count == 2 && t().receive_pos == 8);
    assert!(take_status() == 7);
    init_screen(2);
    assert!(t().screen_init_count == 2 && t().screen_board == 2 && state().screen != 0);
    for percent in [0, 100, 255] {
        new_command(0xfc45, true, 16);
        command()[17] = percent as u8;
        put_be16(command(), 18, 0x1234);
        dispatch();
        assert!(t().screen_percent == percent.min(100) && t().screen_fill == 0x1234);
    }
    assert!(t().screen_progress_count == 3);
    init_screen(9);
    assert!(state().screen == 0 && t().screen_init_count == 2);
    reply(0xfc45);
    assert!(t().screen_progress_count == 3);
    t().init_success = false;
    init_screen(1);
    assert!(state().screen == 0 && sent_bytes()[2] == 0);
    reply(0xfc40);
    assert!(sent_bytes()[14] == 15);
    t().tests += 1;
}

fn test_reinstallation() {
    // Safety: bytes that are no State, for `loader_init` to make one of.
    unsafe { LOADER.get().as_mut_ptr().write_bytes(0xa5, 1) };
    init(&services());
    assert!(state().error == 0 && state().screen == 0 && nor_ctx(&state().io) == flash_pointer());
    assert!(
        fn_addr_eq(
            state().io.send,
            send_callback as unsafe extern "C" fn(_, _) -> _
        ) && fn_addr_eq(
            state().io.receive,
            receive_callback as unsafe extern "C" fn(_, _) -> _
        )
    );
    for b in 0..2 {
        for i in 0..4096 {
            assert!(state().buffer[b][i] == 0);
        }
    }
    for i in 0..256 {
        let byte = i as u8;
        assert!(state().crc_table[i] == crc32(0, &[byte]));
    }
    reply(0xfc40);
    assert!(be32(sent_bytes(), 10) == services().entry && sent_bytes()[15] == 96);
    t().init_success = true;
    init_screen(2);
    state().error = 2;
    let mut other = services();
    other.entry = 0x01c2_8000;
    other.cpu_mhz = 0;
    init(&other);
    assert!(state().screen == 0 && state().error == 0);
    reply(0xfc40);
    assert!(be32(sent_bytes(), 10) == other.entry && sent_bytes()[15] == 0);
    init(&services());
    t().tests += 1;
}

#[test]
fn loader() {
    init(&services());
    test_identity();
    test_streaming();
    test_programming();
    test_rejections();
    test_display();
    test_reinstallation();
    println!(
        "loader: {} groups passed (ROM callbacks, raw NOR, every opcode, {} fixture CRCs)",
        t().tests,
        t().fixture_groups
    );
}
