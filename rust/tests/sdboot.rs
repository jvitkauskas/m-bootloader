// SPDX-License-Identifier: MIT
//! rust/src/sdboot.rs against a simulated SPI card holding generated FAT32 images.
use crate::Global;
use crate::formats::crc16;
use crate::hw::{Text, bit};
// The two halves of a session, which the tests run by themselves.
use crate::sdboot::sd_probe;
use crate::sdboot::test::{sd_test_init as init, sd_test_load as load};
use core::ffi::{c_int, c_void};

const SPI1: usize = 0x11d00;
// Where stage 2 is loaded and the most it may be (docs/SPEC.md §11.5): the test's own copies, so that a wrong value
// in the crate fails here and is not followed.
const STAGE2_ADDRESS: usize = 0x01c2_8000;
const STAGE2_MAX: u32 = 0x8000;
const NO_MENU: &str = "NO /MBOOT/MENU.BIN";
const SECTORS: u32 = 4096;
const RESERVED: u32 = 4;
const FAT_SECTORS: u32 = 8;
const SPC: u32 = 2;
const ENTRIES: u32 = SPC * 512 / 32;
const PARTITION: u32 = 8;
/// The bytes of a cluster.
const CLUSTER: usize = (SPC * 512) as usize;

// What <sys/mman.h> has, on Linux.
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_PRIVATE: c_int = 2;
const MAP_ANONYMOUS: c_int = 0x20;
/// At that address or not at all: nothing that is already there is replaced.
const MAP_FIXED_NOREPLACE: c_int = 0x10_0000;

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
}

// The card's image: `BOOT` and `FAT` are where its boot sector and its FAT start in it.
static DISK: Global<[u8; (SECTORS * 512) as usize]> = Global::new([0; (SECTORS * 512) as usize]);
static BOOT: Global<usize> = Global::new(0);
static FAT: Global<usize> = Global::new(0);
static DATA_START: Global<u32> = Global::new(0);
static NEXT_FREE: Global<u32> = Global::new(0);
static HIGH_LBA: Global<u32> = Global::new(0);

/// SPI1's registers, and the card behind them.
struct Bus {
    con: u32,
    baud: u32,
    data: u32,
    bus_state: u32,
    selected: bool,
    present: bool,
    read_fails: bool,
    command: [u8; 6],
    reply: [u8; 600],
    command_len: usize,
    reply_pos: usize,
    reply_len: usize,
}

static BUS: Global<Bus> = Global::new(Bus {
    con: 0,
    baud: 0,
    data: 0,
    bus_state: 0,
    selected: false,
    present: false,
    read_fails: false,
    command: [0; 6],
    reply: [0; 600],
    command_len: 0,
    reply_pos: 0,
    reply_len: 0,
});

/// The image. (As with `bus`: for one statement or one function, and not kept over a call that uses it too.)
fn disk() -> &'static mut [u8; (SECTORS * 512) as usize] {
    // Safety: one thread, and no other reference to it is alive.
    unsafe { DISK.get() }
}

fn bus() -> &'static mut Bus {
    // Safety: one thread, and no other reference to it is alive.
    unsafe { BUS.get() }
}

impl Bus {
    fn answer(&mut self, b: u8) {
        self.reply[self.reply_len] = b;
        self.reply_len += 1;
    }

    fn card_command(&mut self) {
        let c = self.command[0] & 0x3f;
        let mut arg = u32::from_be_bytes(self.command[1..5].try_into().unwrap());
        self.reply_pos = 0;
        self.reply_len = 0;
        if c == 0 || c == 55 {
            self.answer(1);
        } else if c == 8 {
            self.answer(1);
            for i in 0..4 {
                self.answer(match i {
                    2 => 1,
                    3 => 0xaa,
                    _ => 0,
                });
            }
        } else if c == 41 {
            self.answer(0);
        } else if c == 58 {
            self.answer(0);
            self.answer(0x40); // high capacity: addressed by sector
            for _ in 0..3 {
                self.answer(0);
            }
        } else if c == 17 {
            // Keeps one far cluster of a big card in the image.
            let high_lba = HIGH_LBA.read();
            if high_lba != 0 && arg >= high_lba {
                arg = arg.wrapping_sub(high_lba).wrapping_add(SECTORS - SPC);
            }
            if self.read_fails || arg >= SECTORS {
                self.answer(4);
                return;
            }
            self.answer(0);
            self.answer(0xfe);
            self.reply[self.reply_len..][..512]
                .copy_from_slice(&disk()[arg as usize * 512..][..512]);
            self.reply_len += 512;
            self.answer(0);
            self.answer(0);
        } else {
            self.answer(4);
        }
    }

    fn card_byte(&mut self, input: u8) -> u8 {
        if !self.present || !self.selected {
            return 0xff;
        }
        if self.command_len != 0 || input & 0xc0 == 0x40 {
            self.command[self.command_len] = input;
            self.command_len += 1;
            if self.command_len == 6 {
                self.command_len = 0;
                self.card_command();
            }
            return 0xff;
        }
        if self.reply_pos < self.reply_len {
            self.reply_pos += 1;
            self.reply[self.reply_pos - 1]
        } else {
            0xff
        }
    }
}

/// x() always goes: control, data write, control until done, data read.
pub fn platform_register(address: usize) -> *mut u32 {
    let bus = bus();
    if address == SPI1 {
        if bus.bus_state == 1 {
            bus.data = bus.card_byte(bus.data as u8) as u32;
            bus.con |= bit(15);
            bus.bus_state = 2;
        }
        return &raw mut bus.con;
    }
    if address == SPI1 + 8 {
        bus.bus_state = if bus.bus_state == 0 { 1 } else { 0 };
        return &raw mut bus.data;
    }
    assert!(address == SPI1 + 4);
    &raw mut bus.baud
}

// SPEC §11.5, stated here so that a wrong pin in the code can't pass.
fn is_cs(port: u32, pin: u32) -> bool {
    if cfg!(smk) {
        port == 2 && pin == 8
    } else {
        port == 2 && pin == 7
    }
}

fn is_do(port: u32, pin: u32) -> bool {
    if cfg!(smk) {
        port == 1 && pin == 5
    } else {
        port == 2 && pin == 8
    }
}

// Stage 0's functions, which the crate takes from this test (`import_early`) the way stage 1 takes them from
// stage 0: by their names, through the linker.

#[unsafe(no_mangle)]
pub extern "C" fn gpio_level(port: u32, pin: u32, high: bool) {
    assert!(is_cs(port, pin) || is_do(port, pin));
    if is_cs(port, pin) {
        let bus = bus();
        bus.selected = high;
        bus.reply_pos = 0;
        bus.reply_len = 0;
        bus.command_len = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn gpio_output(port: u32, pin: u32, _high: bool) {
    assert!(is_do(port, pin));
}

#[unsafe(no_mangle)]
pub extern "C" fn gpio_input(port: u32, pin: u32, up: bool, down: bool) {
    assert!(is_do(port, pin));
    assert!(up && !down);
}

#[unsafe(no_mangle)]
pub extern "C" fn hal_delay_us(_usec: u32) -> bool {
    true
}

fn put16(p: &mut [u8], v: u16) {
    p[0] = v as u8;
    p[1] = (v >> 8) as u8;
}

fn put32(p: &mut [u8], v: u32) {
    put16(p, v as u16);
    put16(&mut p[2..], (v >> 16) as u16);
}

fn set_fat(c: u32, v: u32) {
    put32(&mut disk()[FAT.read() + c as usize * 4..], v);
}

/// The image from where cluster `c` starts.
fn cluster(c: u32) -> &'static mut [u8] {
    &mut disk()[((DATA_START.read() + (c - 2) * SPC) * 512) as usize..]
}

fn allocate(after: u32) -> u32 {
    let c = NEXT_FREE.read();
    NEXT_FREE.set(c + 1);
    set_fat(c, 0x0fff_ffff);
    if after != 0 {
        set_fat(after, c);
    }
    c
}

/// An empty card: base 0 has no partition table.
fn format(base: u32) {
    let disk = disk();
    disk.fill(0);
    if base != 0 {
        put32(&mut disk[454..], base);
        put16(&mut disk[510..], 0xaa55);
    }
    BOOT.set(base as usize * 512);
    let boot = &mut disk[BOOT.read()..];
    put16(&mut boot[11..], 512);
    boot[13] = SPC as u8;
    put16(&mut boot[14..], RESERVED as u16);
    boot[16] = 2;
    put32(&mut boot[36..], FAT_SECTORS);
    put32(&mut boot[44..], 2);
    put16(&mut boot[510..], 0xaa55);
    FAT.set(BOOT.read() + RESERVED as usize * 512);
    DATA_START.set(base + RESERVED + 2 * FAT_SECTORS);
    NEXT_FREE.set(2);
    allocate(0);
}

fn entry(dir: u32, index: u32, name: &[u8; 11], attr: u8, start: u32, size: u32) {
    let e = &mut cluster(dir)[index as usize * 32..];
    e[..11].copy_from_slice(name);
    e[11] = attr;
    put16(&mut e[20..], (start >> 16) as u16);
    put16(&mut e[26..], start as u16);
    put32(&mut e[28..], size);
}

fn folder(parent: u32, index: u32, name: &[u8; 11]) -> u32 {
    let c = allocate(0);
    entry(parent, index, name, 0x10, c, 0);
    c
}

fn image(out: &mut [u8]) {
    let size = out.len();
    for i in 0..size - 8 {
        out[i] = (i * 7 + 3) as u8;
    }
    out[size - 8..size - 4].copy_from_slice(b"STG2");
    let crc = crc16(0, &out[..size - 8]);
    put16(&mut out[size - 4..], crc);
    out[size - 2] = 0;
    out[size - 1] = 0;
}

/// A file, in clusters taking every other one when fragmented.
fn file(dir: u32, index: u32, name: &[u8; 11], bytes: &[u8], fragmented: bool) {
    let size = bytes.len();
    let (mut first, mut c) = (0, 0);
    let mut done = 0;
    while done < size || first == 0 {
        c = allocate(c);
        if first == 0 {
            first = c;
        }
        if done < size {
            let n = (size - done).min(CLUSTER);
            cluster(c)[..n].copy_from_slice(&bytes[done..done + n]);
        }
        if fragmented {
            allocate(0);
        }
        done += CLUSTER;
    }
    entry(dir, index, name, 0x20, first, size as u32);
}

/// Where stage 2 is loaded, which `sdboot` maps first.
fn ram() -> &'static mut [u8] {
    // Safety: mapped for the test, which has one thread and keeps no other reference to it.
    unsafe { core::slice::from_raw_parts_mut(STAGE2_ADDRESS as *mut u8, STAGE2_MAX as usize) }
}

fn run() -> Option<Text> {
    ram().fill(0);
    bus().present = true;
    assert!(init());
    load()
}

fn same(a: Option<Text>, b: &str) -> bool {
    a.is_some_and(|a| a.as_bytes() == b.as_bytes())
}

fn loaded(bytes: &[u8]) -> bool {
    ram()[..bytes.len()] == *bytes
}

fn good_card(base: u32, menu: &[u8]) {
    format(base);
    let mboot = folder(2, 0, b"MBOOT      ");
    file(mboot, 0, b"MENU    BIN", menu, false);
}

#[test]
fn sdboot() {
    // Safety: a new mapping, of memory that nothing has.
    let ram = unsafe {
        mmap(
            STAGE2_ADDRESS as *mut c_void,
            STAGE2_MAX as usize,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
            -1,
            0,
        )
    };
    assert!(ram == STAGE2_ADDRESS as *mut c_void);
    let mut bytes = [0; 9000];
    image(&mut bytes);
    let menu = &bytes;
    // Its CRC16/XMODEM, from Python's binascii.crc_hqx(bytes, 0) and not from the crate, which `image` uses.
    assert!(menu[9000 - 4..9000 - 2] == [0xaf, 0x35]);

    // No card: DO stays high, and nothing answers.
    bus().present = false;
    assert!(!sd_probe(60));
    bus().present = true;
    good_card(PARTITION, menu);
    assert!(sd_probe(60));

    // In a partition and without a partition table.
    assert!(run().is_none() && loaded(menu));
    good_card(0, menu);
    assert!(run().is_none() && loaded(menu));

    // Scattered: a fragmented file, and the folder past the root directory's first cluster.
    format(PARTITION);
    let more = allocate(2);
    for i in 0..ENTRIES {
        entry(2, i, b"OTHER   BIN", 0x20, 0, 0);
    }
    let mut mboot = folder(more, 1, b"MBOOT      ");
    entry(more, 0, b"OTHER   BIN", 0x20, 0, 0);
    file(mboot, 0, b"MENU    BIN", menu, true);
    assert!(run().is_none() && loaded(menu));

    // A cluster number with high bits, as on a big card.
    let mut small = [0; CLUSTER];
    image(&mut small);
    format(PARTITION);
    mboot = folder(2, 0, b"MBOOT      ");
    HIGH_LBA.set(DATA_START.read() + 0x10000 * SPC);
    disk()[(SECTORS - SPC) as usize * 512..][..small.len()].copy_from_slice(&small);
    entry(mboot, 0, b"MENU    BIN", 0x20, 0x10002, small.len() as u32);
    assert!(run().is_none() && loaded(&small));
    HIGH_LBA.set(0);

    // Things that look like it: long-name and deleted entries, and the other kind of entry with the name.
    format(PARTITION);
    entry(2, 0, b"MBOOT      ", 0x0f, 0, 0);
    entry(2, 1, b"MBOOT      ", 0x20, 0, 0);
    entry(2, 2, b"\xe5BOOT      ", 0x10, 0, 0);
    mboot = folder(2, 3, b"MBOOT      ");
    entry(mboot, 0, b"MENU    BIN", 0x10, 0, 0);
    entry(mboot, 1, b"MENU    BIN", 0x08, 0, 0);
    file(mboot, 2, b"MENU    BIN", menu, false);
    assert!(run().is_none() && loaded(menu));

    // Missing: no folder, only a root MENU.BIN, an empty folder, a file named MBOOT, another file.
    format(PARTITION);
    assert!(same(run(), NO_MENU));
    file(2, 0, b"MENU    BIN", menu, false);
    assert!(same(run(), NO_MENU));
    format(PARTITION);
    folder(2, 0, b"MBOOT      ");
    assert!(same(run(), NO_MENU));
    format(PARTITION);
    file(2, 0, b"MBOOT      ", menu, false);
    assert!(same(run(), NO_MENU));
    format(PARTITION);
    mboot = folder(2, 0, b"MBOOT      ");
    file(mboot, 0, b"STAGE2  BIN", menu, false);
    assert!(same(run(), NO_MENU));

    // The first unused entry ends the directory, whatever is left after it.
    format(PARTITION);
    mboot = allocate(0);
    file(mboot, 0, b"MENU    BIN", menu, false);
    entry(2, 1, b"MBOOT      ", 0x10, mboot, 0);
    assert!(same(run(), NO_MENU));

    // A full directory with nothing after it ends at the end of its chain.
    format(PARTITION);
    for i in 0..ENTRIES {
        entry(2, i, b"OTHER   BIN", 0x20, 0, 0);
    }
    assert!(same(run(), NO_MENU));

    // A bad image.
    let mut bad = *menu;
    bad[100] ^= 1;
    format(PARTITION);
    file(folder(2, 0, b"MBOOT      "), 0, b"MENU    BIN", &bad, false);
    assert!(same(run(), "BAD MENU.BIN"));
    bad = *menu;
    bad[bad.len() - 8] ^= 1;
    format(PARTITION);
    file(folder(2, 0, b"MBOOT      "), 0, b"MENU    BIN", &bad, false);
    assert!(same(run(), "BAD MENU.BIN"));
    format(PARTITION);
    file(
        folder(2, 0, b"MBOOT      "),
        0,
        b"MENU    BIN",
        &menu[..15],
        false,
    );
    assert!(same(run(), "BAD MENU.BIN"));
    format(PARTITION);
    mboot = folder(2, 0, b"MBOOT      ");
    entry(mboot, 0, b"MENU    BIN", 0x20, 2, STAGE2_MAX + 1);
    assert!(same(run(), "BAD MENU.BIN"));
    // A size past its chain.
    format(PARTITION);
    mboot = folder(2, 0, b"MBOOT      ");
    file(mboot, 0, b"MENU    BIN", menu, false);
    put32(&mut cluster(mboot)[28..], (menu.len() + CLUSTER * 2) as u32);
    assert!(same(run(), "BAD MENU.BIN"));

    // Not a card this can read.
    good_card(PARTITION, menu);
    put16(&mut disk()[510..], 0);
    assert!(same(run(), "NO SD CARD"));
    good_card(PARTITION, menu);
    put16(&mut disk()[BOOT.read() + 17..], 512); // FAT12/16's root entry count
    assert!(same(run(), "SD NOT FAT32"));
    good_card(PARTITION, menu);
    assert!(init());
    bus().read_fails = true;
    assert!(same(load(), "NO SD CARD"));
    bus().read_fails = false;

    println!(
        "PASS: card probe, /MBOOT/MENU.BIN lookup and load, and refusals on generated FAT32 images"
    );
}
