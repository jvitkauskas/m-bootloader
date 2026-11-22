// SPDX-License-Identifier: MIT
//! The flash loader's life on the simulated chip (docs/SPEC.md §10): execute actual slot A/B entry and registered
//! command handlers against the same independent MMIO/NOR model as the complete boot simulation.
use super::sim::{
    NOR_MAX, nor_size, sim, sim_flush, sim_nor, sim_p33_get, sim_p33_set, sim_reg_get, sim_reset,
};
use crate::Global;
use crate::formats::{be16, be32, crc16, crc32, put_be16, put_be32, put_le16, put_le32};
use crate::loader::{Services, State, slot_a, slot_b};
use crate::platform::{flash_dma_finish, flash_dma_start, flash_erase, flash_program, flash_read};
use core::ffi::c_void;
use core::ptr::fn_addr_eq;

/// What the loader answers to FC40 from its third byte: "MFLR", and its version.
const IDENTITY: &[u8; 7] = b"MFLR1.4";

/// The ROM's slot for a plug-in's handler.
static REGISTRATION: Global<u32> = Global::new(0);
static COMMAND: Global<[u8; 31]> = Global::new([0; 31]);
static ANSWER: Global<[u8; 16]> = Global::new([0; 16]);
static OUTPUT: Global<[u8; 65536]> = Global::new([0; 65536]);
static INPUT: Global<[u8; 4096]> = Global::new([0; 4096]);
static OUTPUT_SIZE: Global<usize> = Global::new(0);
static INPUT_SIZE: Global<usize> = Global::new(0);
static INPUT_POSITION: Global<usize> = Global::new(0);
static CALLBACK_SET: Global<u32> = Global::new(0);
static CHECKS: Global<u32> = Global::new(0);
static INSTALLS: Global<u32> = Global::new(0);
static CURRENT: Global<&'static str> = Global::new("");

// Safety, for these three: the test is one thread, and the loader has no reference to them. (It has one to the
// answer while a command runs, so that has no such function.)
fn command() -> &'static mut [u8; 31] {
    unsafe { COMMAND.get() }
}

fn output() -> &'static mut [u8; 65536] {
    unsafe { OUTPUT.get() }
}

fn input() -> &'static mut [u8; 4096] {
    unsafe { INPUT.get() }
}

/// A check, which is counted: the last line says how many there were. (One that fails in a callback or under the
/// loader's entry ends the program there, as a panic can't pass a function that has C's calling convention.)
macro_rules! check {
    ($c:expr) => {{
        CHECKS.set(CHECKS.read() + 1);
        assert!($c, "{}: {}", CURRENT.read(), stringify!($c));
    }};
}

// What the bootloader has from its tests on the simulated chip (rust/tests/boot.rs). It is built here too, and
// nothing starts it.
pub fn boot_sim_reset() -> ! {
    panic!("loader must never request reset");
}

pub fn sd_probe(_lsb_mhz: u32) -> bool {
    unreachable!()
}

pub fn sd_stage2(_lsb_mhz: u32) -> Option<crate::hw::Text> {
    unreachable!()
}

pub unsafe fn boot_sim_call_app(_entry: u32, _object: *mut u8) {
    unreachable!()
}

pub unsafe fn boot_sim_call_updater(_entry: u32, _object: *mut u8, _end: u32) {
    unreachable!()
}

fn send_callback(data: *const c_void, size: u32, set: u32) -> i32 {
    sim_flush();
    let size = size as usize;
    check!(size != 0 && size <= 512 && size <= output().len() - OUTPUT_SIZE.read());
    if be16(&COMMAND.read(), 15) != 0xfd05 {
        check!(data.cast::<u8>() == (&raw const ANSWER).cast::<u8>());
    }
    // Safety: what the loader sends, which is `size` bytes.
    let data = unsafe { core::slice::from_raw_parts(data.cast::<u8>(), size) };
    output()[OUTPUT_SIZE.read()..][..size].copy_from_slice(data);
    OUTPUT_SIZE.set(OUTPUT_SIZE.read() + size);
    CALLBACK_SET.set(set);
    -123
}

extern "C" fn send_one(data: *const c_void, size: u32) -> i32 {
    send_callback(data, size, 1)
}

extern "C" fn send_two(data: *const c_void, size: u32) -> i32 {
    send_callback(data, size, 2)
}

fn receive_callback(data: *mut c_void, size: u32, set: u32) -> i32 {
    let size = size as usize;
    check!(size != 0 && size <= 512 && size <= INPUT_SIZE.read() - INPUT_POSITION.read());
    // Safety: where the loader takes `size` bytes.
    let data = unsafe { core::slice::from_raw_parts_mut(data.cast::<u8>(), size) };
    data.copy_from_slice(&input()[INPUT_POSITION.read()..][..size]);
    INPUT_POSITION.set(INPUT_POSITION.read() + size);
    CALLBACK_SET.set(set);
    -123
}

extern "C" fn receive_one(data: *mut c_void, size: u32) -> i32 {
    receive_callback(data, size, 1)
}

extern "C" fn receive_two(data: *mut c_void, size: u32) -> i32 {
    receive_callback(data, size, 2)
}

fn new_command(opcode: u16, inward: bool, count: u32) {
    let command = command();
    *command = [0; 31];
    put_le32(command, 0, 0x4342_5355);
    put_le32(command, 8, count);
    command[12] = if inward { 128 } else { 0 };
    command[14] = 16;
    put_be16(command, 15, opcode);
    // Safety: no command is running, so nothing else has the answer.
    unsafe { ANSWER.get() }.fill(0xa5);
    OUTPUT_SIZE.set(0);
    INPUT_SIZE.set(0);
    INPUT_POSITION.set(0);
    CALLBACK_SET.set(0);
}

fn dispatch() {
    let registration = REGISTRATION.read();
    check!(registration != 0);
    // Safety: what the entry registered is its handler, which takes a command and the room for an answer.
    let handler = unsafe {
        core::mem::transmute::<usize, extern "C" fn(&[u8; 31], &mut [u8; 16]) -> i32>(
            registration as usize,
        )
    };
    let command = COMMAND.read();
    // Safety: the answer is the handler's until it returns: the callbacks only take its address.
    check!(handler(&command, unsafe { ANSWER.get() }) == 1);
    sim_flush();
    check!(sim().err == 0);
}

fn identity(entry: u32, set: u32, mhz: u8) {
    new_command(0xfc40, true, 16);
    dispatch();
    check!(OUTPUT_SIZE.read() == 16 && CALLBACK_SET.read() == set);
    check!(output()[2..9] == *IDENTITY);
    check!(be32(output(), 10) == entry && output()[14] == 15 && output()[15] == mhz);
}

fn clean_state(state: &State, entry: u32, mhz: u8) {
    check!(state.screen == 0 && state.error == 0);
    check!(state.io.entry == entry && state.io.cpu_mhz == mhz);
    check!(state.io.nor.flash.capacity == 0x10_0000);
    check!(fn_addr_eq(
        state.io.nor.flash.read,
        flash_read as unsafe extern "C" fn(_, _, _, _) -> _
    ));
    check!(
        fn_addr_eq(
            state.io.nor.program,
            flash_program as unsafe extern "C" fn(_, _, _, _) -> _
        ) && fn_addr_eq(
            state.io.nor.erase,
            flash_erase as extern "C" fn(_, _, _) -> _
        )
    );
    check!(
        state
            .io
            .read_start
            .is_some_and(|f| fn_addr_eq(f, flash_dma_start as unsafe extern "C" fn(_, _, _) -> _))
            && state
                .io
                .read_finish
                .is_some_and(|f| fn_addr_eq(f, flash_dma_finish as extern "C" fn() -> _))
    );
    for b in 0..2 {
        for n in 0..4096 {
            check!(state.buffer[b][n] == 0);
        }
    }
    for n in 0..256 {
        let byte = n as u8;
        check!(state.crc_table[n] == crc32(0, &[byte]));
    }
}

/// An address as the ROM gives one to the loader and takes one from it: in 32 bits. This program has to be linked
/// to lie that low (`-C relocation-model=static`), and this says so if it was not. The entry's handler is in the
/// same program as what is asked about here, so it lies as low as they do.
fn address(a: usize) -> u32 {
    u32::try_from(a).unwrap_or_else(|_| {
        panic!(
            "{a:#x} does not fit the 32 bits the loader has for an address: \
             build this test with -C relocation-model=static"
        )
    })
}

fn install(a: bool, set: u32, dirt: u8) {
    let state = if a {
        slot_a::loader_test_state_a()
    } else {
        slot_b::loader_test_state_b()
    };
    // Safety: room for a State, whatever it holds: the entry makes one of it.
    unsafe { state.cast::<u8>().write_bytes(dirt, size_of::<State>()) };
    let send: extern "C" fn(*const c_void, u32) -> i32 = if set == 1 { send_one } else { send_two };
    let receive: extern "C" fn(*mut c_void, u32) -> i32 =
        if set == 1 { receive_one } else { receive_two };
    let params = [
        address(send as usize),
        address(receive as usize),
        address((&raw const REGISTRATION).expose_provenance()),
    ];
    REGISTRATION.set(0xdead_beef);
    // Safety: as the ROM calls it, with its two callbacks and its slot for the handler.
    check!(
        unsafe {
            if a {
                slot_a::loader_test_install_a(&params)
            } else {
                slot_b::loader_test_install_b(&params)
            }
        } == 0
    );
    sim_flush();
    check!(REGISTRATION.read() != 0 && REGISTRATION.read() != 0xdead_beef);
    check!(sim().err == 0);
    let entry = if a { 0x01c2_0000 } else { 0x01c2_8000 };
    // Safety: the entry has made it a State, and no command is running.
    clean_state(unsafe { &*state }, entry, 96);
    identity(entry, set, 96);
    INSTALLS.set(INSTALLS.read() + 1);
}

fn status_and_read(set: u32) {
    new_command(0xfc46, true, 16);
    dispatch();
    check!(output()[2] == 0 && CALLBACK_SET.read() == set);
    new_command(0xfd05, true, 8193);
    put_be32(command(), 17, 0x123);
    put_be16(command(), 21, 8193);
    dispatch();
    check!(OUTPUT_SIZE.read() == 8193 && CALLBACK_SET.read() == set);
    check!(output()[..8193] == sim_nor()[0x123..][..8193]);
    new_command(0xfc41, true, 16);
    put_be32(command(), 17, 0x123);
    put_be32(command(), 21, 8193);
    dispatch();
    check!(be32(output(), 2) == crc32(u32::MAX, &sim_nor()[0x123..][..8193]) ^ u32::MAX);
}

fn fresh_out_callback(set: u32) {
    sim_nor()[0x4000..][..4096].fill(0xff);
    new_command(0xfb04, false, 513);
    INPUT_SIZE.set(513);
    for n in 0..INPUT_SIZE.read() {
        input()[n] = (17 * n + 3) as u8;
    }
    put_be32(command(), 17, 0x400d);
    put_be16(command(), 21, 513);
    put_le16(command(), 24, crc16(0, &input()[..513]));
    dispatch();
    check!(OUTPUT_SIZE.read() == 0 && INPUT_POSITION.read() == 513 && CALLBACK_SET.read() == set);
    check!(sim_nor()[0x400d..][..513] == input()[..513]);
}

/// Whether two states are the same in every field. (C compared their bytes. Here that would take in the bytes
/// between the fields, which hold nothing, and which Rust does not let a program read.)
fn same(a: &State, b: &State) -> bool {
    let State {
        io: x,
        crc_table,
        buffer,
        error,
        screen,
    } = a;
    let Services {
        nor,
        send,
        receive,
        read_start,
        read_finish,
        jedec,
        key,
        uid,
        screen_init,
        screen_row,
        screen_progress,
        entry,
        cpu_mhz,
    } = x;
    let y = &b.io;
    nor.flash.ctx == y.nor.flash.ctx
        && nor.flash.capacity == y.nor.flash.capacity
        && fn_addr_eq(nor.flash.read, y.nor.flash.read)
        && fn_addr_eq(nor.program, y.nor.program)
        && fn_addr_eq(nor.erase, y.nor.erase)
        && fn_addr_eq(*send, y.send)
        && fn_addr_eq(*receive, y.receive)
        && read_start.map(|f| f as usize) == y.read_start.map(|f| f as usize)
        && read_finish.map(|f| f as usize) == y.read_finish.map(|f| f as usize)
        && fn_addr_eq(*jedec, y.jedec)
        && fn_addr_eq(*key, y.key)
        && fn_addr_eq(*uid, y.uid)
        && screen_init.map(|f| f as usize) == y.screen_init.map(|f| f as usize)
        && screen_row.map(|f| f as usize) == y.screen_row.map(|f| f as usize)
        && screen_progress.map(|f| f as usize) == y.screen_progress.map(|f| f as usize)
        && *entry == y.entry
        && *cpu_mhz == y.cpu_mhz
        && *crc_table == b.crc_table
        && *buffer == b.buffer
        && *error == b.error
        && *screen == b.screen
}

#[test]
fn loader_lifecycle() {
    CURRENT.set("power-on");
    sim_reset();
    sim_nor().fill(0xff);
    *nor_size() = NOR_MAX;
    for n in 0..NOR_MAX as usize {
        sim_nor()[n] = (n * 47 ^ (n >> 8)) as u8;
    }
    // B must retain ROM's watchdog policy across every installation.
    sim_p33_set(0x80, 0x31);
    sim_p33_set(9, 0xa5);
    sim_p33_set(6, 0xf8);
    sim_p33_set(5, 4);
    CURRENT.set("slot A dirty upload at ROM 48 MHz");
    install(true, 1, 0xa5);
    check!(sim_reg_get(0x10014) & 255 == 0x40);
    check!(sim_reg_get(0x10008) & 0x30700 == 0x100);
    status_and_read(1);
    fresh_out_callback(1);
    let first_handler = REGISTRATION.read();
    // Seed live dirty state in A, and show that installing the other slot leaves it. (This shows that B's entry
    // and commands write B's state and not A's. On the chip an entry also clears its own image's BSS, which is not
    // compiled in on this computer, so of that clear this shows nothing.)
    let a = slot_a::loader_test_state_a();
    // Safety, here and for B below: the entry has made it a State, and no command is running.
    let keep_a = unsafe {
        (*a).error = 7;
        (*a).screen = 1;
        (*a).buffer = [[0x5a; 4096]; 2];
        (*a).crc_table[17] ^= 0x1234_5678;
        a.read()
    };
    CURRENT.set("slot B after A, chip already 96 MHz");
    install(false, 2, 0x3c);
    check!(REGISTRATION.read() != first_handler);
    check!(same(unsafe { &*a }, &keep_a));
    status_and_read(2);
    fresh_out_callback(2);
    let b = slot_b::loader_test_state_b();
    let keep_b = unsafe {
        (*b).error = 2;
        (*b).screen = 1;
        (*b).buffer = [[0xc3; 4096]; 2];
        b.read()
    };
    CURRENT.set("slot A reinstalled with fresh callback set");
    install(true, 2, 0xff);
    check!(REGISTRATION.read() == first_handler);
    check!(same(unsafe { &*b }, &keep_b));
    status_and_read(2);
    fresh_out_callback(2);
    CURRENT.set("slot B reinstalled with old callback set");
    install(false, 1, 0x69);
    status_and_read(1);
    fresh_out_callback(1);
    CURRENT.set("same slot B installed again without clock reset");
    install(false, 2, 0x7e);
    status_and_read(2);
    check!(sim_p33_get(0x80) == 0x31);
    check!(sim_p33_get(9) == 0xa5 && sim_p33_get(6) == 0xf8 && sim_p33_get(5) == 4);
    check!(!sim().pll_seen && sim().ncmd == 0 && sim().phy_writes == 0);
    println!(
        "loader lifecycle: {} dirty installations, {} checks, two slots/ROM handlers/96 MHz passed",
        INSTALLS.read(),
        CHECKS.read()
    );
}
