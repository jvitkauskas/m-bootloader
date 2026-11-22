// SPDX-License-Identifier: MIT
//! SD mod, stage 1: load /MBOOT/MENU.BIN from a FAT32 card into RAM and call it.
//!
//! The card shares the panel's SPI1, with DO on the panel's D/C and CS = NOT the panel's CS (an inverter), so the card
//! is selected while that's high. FM-1: CLK PC9, DI PC10, DO PC8, CS = NOT PC7. SMK-37: CLK PB6, DI PB7, DO PB5,
//! CS = NOT PC8. A session first sends the panel a NOP, so the bytes clocked while the card is deselected are ignored,
//! then turns D/C into SPI1's DI and SPI1 to separate DI/DO.
//!
//! /MBOOT/MENU.BIN is a flat image linked for `STAGE2_ADDRESS`, entry at its first byte, at most `STAGE2_MAX` bytes,
//! ending in an 8-byte trailer: "STG2", CRC16 (little endian) of everything before the trailer, two zero bytes. It is
//! called with the LSB clock in MHz and may return.
use crate::formats::{crc16, le16, le32};
use crate::hw::{Reg, Text, bit, sync_cpu};
use crate::platform::{gpio_input, gpio_level, gpio_output, hal_delay_us};
use crate::{Global, text};

pub const STAGE2_ADDRESS: usize = 0x01c2_8000;
pub const STAGE2_MAX: u32 = 0x8000;
const SPI1_CON: Reg = Reg::at::<0x11d00>();
const SPI1_BAUD: Reg = SPI1_CON.add(4);
const SPI1_BUF: Reg = SPI1_CON.add(8);
const CS: (u32, u32) = if cfg!(smk) { (2, 8) } else { (2, 7) };
const DO: (u32, u32) = if cfg!(smk) { (1, 5) } else { (2, 8) };
const NO_MENU: Text = text!("NO /MBOOT/MENU.BIN");
const READ_ERROR: Text = text!("SD READ ERROR");
const BAD_MENU: Text = text!("BAD MENU.BIN");
type Block = [u8; 512];

#[repr(align(4))]
struct Card {
    block: Block,
    /// High capacity: addressed by sector.
    hc: bool,
    fat_lba: u32,
    data_lba: u32,
    /// Sectors in a cluster.
    spc: u32,
}

static CARD: Global<Card> = Global::new(Card {
    block: [0; 512],
    hc: false,
    fat_lba: 0,
    data_lba: 0,
    spc: 0,
});

fn x(b: u8) -> u8 {
    SPI1_CON.set(bit(14));
    SPI1_BUF.write(b as u32);
    sync_cpu();
    let mut n = 0;
    while n < 400_000 && SPI1_CON.read() & bit(15) == 0 {
        n += 1;
    }
    SPI1_BUF.read() as u8
}

fn select(on: bool) {
    gpio_level(CS.0, CS.1, on);
}

fn cmd(c: u8, a: u32) -> u8 {
    select(false);
    x(0xff);
    select(true);
    x(0xff);
    x(0x40 | c);
    for b in a.to_be_bytes() {
        x(b);
    }
    x(match c {
        0 => 0x95,
        8 => 0x87,
        _ => 1,
    });
    let mut r = 0xff;
    for _ in 0..16 {
        r = x(0xff);
        if r & 0x80 == 0 {
            break;
        }
    }
    r
}

fn rd(hc: bool, lba: u32, p: &mut Block) -> bool {
    let mut ok = false;
    if cmd(17, if hc { lba } else { lba << 9 }) == 0 {
        let (mut r, mut i) = (x(0xff), 0);
        while r == 0xff && i < 99_999 {
            r = x(0xff);
            i += 1;
        }
        if r == 0xfe {
            for b in p {
                *b = x(0xff);
            }
            x(0xff);
            x(0xff);
            ok = true;
        }
    }
    select(false);
    ok
}

fn cluster_ok(c: u32) -> bool {
    (2..0x0fff_fff8).contains(&c)
}

impl Card {
    fn init(&mut self) -> bool {
        select(false);
        for _ in 0..10 {
            x(0xff);
        }
        let mut r = 0;
        for _ in 0..20 {
            r = cmd(0, 0);
            if r == 1 {
                break;
            }
        }
        if r != 1 {
            return false;
        }
        let v2 = cmd(8, 0x1aa) == 1;
        for _ in 0..4 {
            x(0xff);
        }
        for _ in 0..1000 {
            cmd(55, 0);
            r = cmd(41, if v2 { bit(30) } else { 0 });
            if r == 0 {
                break;
            }
            hal_delay_us(1000);
        }
        self.hc = false;
        if r == 0 && v2 && cmd(58, 0) == 0 {
            self.hc = x(0xff) & 0x40 != 0;
            x(0xff);
            x(0xff);
            x(0xff);
        }
        r == 0
    }

    fn read_cluster_sector(&mut self, c: u32, s: u32) -> bool {
        rd(
            self.hc,
            self.data_lba + (c - 2) * self.spc + s,
            &mut self.block,
        )
    }

    fn next(&mut self, c: u32) -> u32 {
        if !rd(self.hc, self.fat_lba + c / 128, &mut self.block) {
            return 0;
        }
        let at = (c % 128 * 4) as usize;
        u32::from_le_bytes(self.block[at..at + 4].try_into().unwrap()) & 0x0fff_ffff
    }

    /// Finds an 8.3 name in a directory; `kind` is the entry's folder and volume-label bits. The entry's first cluster
    /// and size go to `found`.
    fn find(
        &mut self,
        mut c: u32,
        name: &[u8; 11],
        kind: u8,
        found: &mut (u32, u32),
    ) -> Option<Text> {
        while cluster_ok(c) {
            for s in 0..self.spc {
                if !self.read_cluster_sector(c, s) {
                    return Some(READ_ERROR);
                }
                for entry in self.block.as_chunks::<32>().0 {
                    if entry[0] == 0 {
                        return Some(NO_MENU);
                    }
                    if entry[..11] == *name && entry[11] & 0x18 == kind {
                        *found = (
                            le16(entry, 26) as u32 | (le16(entry, 20) as u32) << 16,
                            le32(entry, 28),
                        );
                        return None;
                    }
                }
            }
            c = self.next(c);
        }
        Some(NO_MENU)
    }

    fn load(&mut self) -> Option<Text> {
        let mut base = 0;
        if !rd(self.hc, 0, &mut self.block) || le16(&self.block, 510) != 0xaa55 {
            return Some(text!("NO SD CARD"));
        }
        if le16(&self.block, 11) != 512 {
            base = le32(&self.block, 454);
            if !rd(self.hc, base, &mut self.block) {
                return Some(READ_ERROR);
            }
        }
        let b = &self.block;
        if le16(b, 11) != 512 || le16(b, 17) != 0 || b[13] == 0 {
            return Some(text!("SD NOT FAT32"));
        }
        self.spc = b[13] as u32;
        self.fat_lba = base + le16(b, 14) as u32;
        self.data_lba = self.fat_lba + (b[16] as u32) * le32(b, 36);
        let mut found = (0, 0);
        let error = self
            .find(le32(b, 44), b"MBOOT      ", 0x10, &mut found)
            .or_else(|| self.find(found.0, b"MENU    BIN", 0, &mut found));
        if error.is_some() {
            return error;
        }
        let (mut c, size) = found;
        if !(16..=STAGE2_MAX).contains(&size) {
            return Some(BAD_MENU);
        }
        // Safety: this RAM is the image's to load into (§11.5), in whole sectors.
        let image = unsafe {
            core::slice::from_raw_parts_mut(
                STAGE2_ADDRESS as *mut u8,
                size.next_multiple_of(512) as usize,
            )
        };
        let mut blocks = image.as_chunks_mut::<512>().0.iter_mut();
        'file: loop {
            if !cluster_ok(c) {
                return Some(BAD_MENU);
            }
            for s in 0..self.spc {
                let Some(block) = blocks.next() else {
                    break 'file;
                };
                if !rd(self.hc, self.data_lba + (c - 2) * self.spc + s, block) {
                    return Some(READ_ERROR);
                }
            }
            if blocks.len() == 0 {
                break;
            }
            c = self.next(c);
        }
        let (body, trailer) = image[..size as usize].split_at(size as usize - 8);
        let trailer: &[u8; 8] = trailer.try_into().unwrap();
        if trailer[..4] != *b"STG2" || le16(trailer, 4) != crc16(0, body) {
            return Some(BAD_MENU);
        }
        None
    }
}

fn session_open(lsb_mhz: u32) {
    // Panel NOP (D/C low, CS low), then D/C as the card's DO, SPI1 separate DI/DO at ~400 kHz.
    gpio_level(DO.0, DO.1, false);
    select(false);
    SPI1_CON.write(0x4021);
    x(0);
    gpio_input(DO.0, DO.1, true, false);
    SPI1_CON.write(0x4029);
    SPI1_BAUD.write(lsb_mhz * 5 / 2 - 1);
}

fn session_close() {
    select(false);
    x(0xff);
    SPI1_CON.write(0x4021);
    SPI1_BAUD.write(4);
    gpio_output(DO.0, DO.1, true);
}

/// Whether a card answers (there is no card-detect line).
#[unsafe(no_mangle)]
pub extern "C" fn sd_probe(lsb_mhz: u32) -> bool {
    // Safety: the card's state is used only inside these two entries, which call nothing that comes back.
    let card = unsafe { CARD.get() };
    session_open(lsb_mhz);
    let found = card.init();
    session_close();
    found
}

/// Loads and calls /MBOOT/MENU.BIN; None when stage 2 returned, else what failed (for the menu).
#[unsafe(no_mangle)]
pub extern "C" fn sd_stage2(lsb_mhz: u32) -> Option<Text> {
    let card = unsafe { CARD.get() };
    session_open(lsb_mhz);
    let mut error = Some(text!("NO SD CARD"));
    if card.init() {
        SPI1_BAUD.write(lsb_mhz / 4 - 1);
        error = card.load();
    }
    session_close();
    if error.is_some() {
        return error;
    }
    sync_cpu();
    #[cfg(not(platform_sim))]
    unsafe {
        core::mem::transmute::<usize, extern "C" fn(u32)>(STAGE2_ADDRESS)(lsb_mhz)
    }
    None
}

/// For the card's test (rust/tests/sdboot.rs), which drives the two halves by themselves.
#[cfg(sdboot_test)]
pub mod test {
    use super::*;

    pub fn sd_test_init() -> bool {
        unsafe { CARD.get() }.init()
    }

    pub fn sd_test_load() -> Option<Text> {
        unsafe { CARD.get() }.load()
    }
}
