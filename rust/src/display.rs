// SPDX-License-Identifier: MIT
//! The screen: docs/SPEC.md §8.4. 240 by 240 pixels on SPI1, written in 15 rows of 20 characters.
use crate::Global;
use crate::hw::{Reg, Text, bit, dma_address, sync_cpu};
use crate::platform::{gpio_level, gpio_output, hal_delay_us, hal_fault, hal_fault_clear};

const SPI1_CON: Reg = Reg::at::<0x11d00>();
const SPI1_BAUD: Reg = SPI1_CON.add(4);
const SPI1_BUF: Reg = SPI1_CON.add(8);
const SPI1_ADR: Reg = SPI1_CON.add(12);
const SPI1_CNT: Reg = SPI1_CON.add(16);
const IO_MUX1: Reg = Reg::at::<0x51020>();
pub const YELLOW: u16 = 0xffe0;
pub const WHITE: u16 = 0xffff;
pub const GREEN: u16 = 0x07e0;
pub const RED: u16 = 0xf800;
pub const GREY: u16 = 0x8410;
/// A character's cell: 12 by 16 pixels of two bytes.
const CELL: usize = 384;
/// docs/SPEC.md Appendix A, author-provided MIT glyphs; see NOTICE.md.
const CHARACTERS: &[u8; 45] = b" -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static GLYPHS: [[u8; 5]; 45] = [
    [0, 0, 0, 0, 0],
    [8, 8, 8, 8, 8],
    [0, 0x60, 0x60, 0, 0],
    [0x60, 0x10, 8, 4, 3],
    [0, 0x36, 0x36, 0, 0],
    [0, 8, 0x14, 0x22, 0x41],
    [0x41, 0x22, 0x14, 8, 0],
    [8, 8, 0x3e, 8, 8],
    [2, 1, 0x51, 9, 6],
    [0x3e, 0x51, 0x49, 0x45, 0x3e],
    [0, 0x42, 0x7f, 0x40, 0],
    [0x42, 0x61, 0x51, 0x49, 0x46],
    [0x21, 0x41, 0x45, 0x4b, 0x31],
    [0x18, 0x14, 0x12, 0x7f, 0x10],
    [0x27, 0x45, 0x45, 0x45, 0x39],
    [0x3c, 0x4a, 0x49, 0x49, 0x30],
    [1, 0x71, 9, 5, 3],
    [0x36, 0x49, 0x49, 0x49, 0x36],
    [6, 0x49, 0x49, 0x29, 0x1e],
    [0x7e, 9, 9, 9, 0x7e],
    [0x7f, 0x49, 0x49, 0x49, 0x36],
    [0x3e, 0x41, 0x41, 0x41, 0x22],
    [0x7f, 0x41, 0x41, 0x41, 0x3e],
    [0x7f, 0x49, 0x49, 0x49, 0x41],
    [0x7f, 9, 9, 9, 1],
    [0x3e, 0x41, 0x49, 0x49, 0x7a],
    [0x7f, 8, 8, 8, 0x7f],
    [0, 0x41, 0x7f, 0x41, 0],
    [0x20, 0x40, 0x41, 0x3f, 1],
    [0x7f, 8, 0x14, 0x22, 0x41],
    [0x7f, 0x40, 0x40, 0x40, 0x40],
    [0x7f, 2, 0x0c, 2, 0x7f],
    [0x7f, 4, 8, 0x10, 0x7f],
    [0x3e, 0x41, 0x41, 0x41, 0x3e],
    [0x7f, 9, 9, 9, 6],
    [0x3e, 0x41, 0x51, 0x21, 0x5e],
    [0x7f, 9, 0x19, 0x29, 0x46],
    [0x46, 0x49, 0x49, 0x49, 0x31],
    [1, 1, 0x7f, 1, 1],
    [0x3f, 0x40, 0x40, 0x40, 0x3f],
    [0x1f, 0x20, 0x40, 0x20, 0x1f],
    [0x3f, 0x40, 0x38, 0x40, 0x3f],
    [0x63, 0x14, 8, 0x14, 0x63],
    [3, 4, 0x78, 4, 3],
    [0x61, 0x51, 0x49, 0x45, 0x43],
];

/// What the DMA sends: aligned for it.
#[repr(align(4))]
struct Pixels([u8; CELL]);

struct Display {
    cs: (u32, u32),
    dc: (u32, u32),
    active: bool,
    pixels: Pixels,
}

static DISPLAY: Global<Display> = Global::new(Display {
    cs: (0, 0),
    dc: (0, 0),
    active: false,
    pixels: Pixels([0; CELL]),
});

/// Safety, for every use: these entries call nothing that comes back into this module.
fn display() -> &'static mut Display {
    unsafe { DISPLAY.get() }
}

impl Display {
    fn wait_panel(&mut self) -> bool {
        sync_cpu();
        let mut n = 1_000_000;
        while n != 0 {
            if SPI1_CON.read() & bit(15) != 0 {
                SPI1_CON.set(bit(14));
                return true;
            }
            n -= 1;
        }
        self.active = false;
        false
    }

    fn byte(&mut self, b: u8) {
        if self.active {
            SPI1_BUF.write(b as u32);
            self.wait_panel();
        }
    }

    fn select(&self, on: bool) {
        gpio_level(self.cs.0, self.cs.1, !on);
    }

    fn command(&mut self, command: u8) {
        self.select(true);
        gpio_level(self.dc.0, self.dc.1, false);
        self.byte(command);
        gpio_level(self.dc.0, self.dc.1, true);
    }

    fn burst(&mut self, command: u8, data: &[u8]) {
        if !self.active {
            return;
        }
        self.command(command);
        for &b in data {
            self.byte(b);
        }
        self.select(false);
    }

    fn delay(&mut self, ms: u32) -> bool {
        let prior = hal_fault();
        let ok = hal_delay_us(ms * 1000);
        if !prior && !ok {
            hal_fault_clear();
        }
        if !ok {
            self.active = false;
        }
        ok
    }

    /// Opens the area that the pixels sent next fill. The panel stays selected.
    fn window(&mut self, x: u32, y: u32, w: u32, h: u32) {
        let span = |from: u32, size: u32| {
            let to = from + size - 1;
            [(from >> 8) as u8, from as u8, (to >> 8) as u8, to as u8]
        };
        self.burst(0x2a, &span(x, w));
        self.burst(0x2b, &span(y, h));
        self.command(0x2c);
    }

    fn dma(&mut self, n: u32) {
        if !self.active || n == 0 {
            return;
        }
        SPI1_CON.set(bit(14));
        SPI1_ADR.write(dma_address(self.pixels.0.as_mut_ptr()));
        sync_cpu();
        SPI1_CNT.write(n);
        self.wait_panel();
    }

    fn visible(&self, x: u32, y: u32, w: u32, h: u32) -> bool {
        self.active && w != 0 && h != 0 && x < 240 && y < 240 && w <= 240 - x && h <= 240 - y
    }

    fn rect(&mut self, x: u32, y: u32, w: u32, h: u32, color: u16) {
        if !self.visible(x, y, w, h) {
            return;
        }
        for pixel in self.pixels.0.as_chunks_mut().0 {
            *pixel = color.to_be_bytes();
        }
        self.window(x, y, w, h);
        let mut n = w * h * 2;
        while n != 0 && self.active {
            let k = n.min(CELL as u32);
            self.dma(k);
            n -= k;
        }
        self.select(false);
    }

    /// `data` holds two bytes for each pixel.
    #[cfg(not(target_arch = "pi32v2"))]
    fn pixels(&mut self, x: u32, y: u32, w: u32, h: u32, data: &[u8]) {
        if !self.visible(x, y, w, h) {
            return;
        }
        self.window(x, y, w, h);
        for part in data.chunks(CELL) {
            if !self.active {
                break;
            }
            self.pixels.0[..part.len()].copy_from_slice(part);
            self.dma(part.len() as u32);
        }
        self.select(false);
    }

    fn init(&mut self, board: u32) -> bool {
        let fm1 = match board {
            1 => true,
            2 => false,
            _ => return false,
        };
        self.active = true;
        if fm1 {
            IO_MUX1.set(bit(4));
            (self.cs, self.dc) = ((2, 7), (2, 8));
            gpio_output(2, 9, false);
            gpio_output(2, 10, false);
            gpio_output(0, 2, true);
        } else {
            IO_MUX1.clear(bit(4));
            (self.cs, self.dc) = ((2, 8), (1, 5));
            gpio_output(1, 6, false);
            gpio_output(1, 7, false);
        }
        gpio_output(self.cs.0, self.cs.1, true);
        gpio_output(self.dc.0, self.dc.1, false);
        SPI1_CON.write(0x4021);
        SPI1_BAUD.write(4);
        if fm1 {
            self.burst(1, &[]);
            self.delay(150);
            self.burst(0x11, &[]);
            self.delay(120);
            self.burst(0x3a, &[0x55]);
            self.burst(0x36, &[0]);
            self.burst(0x21, &[]);
            self.burst(0x13, &[]);
        } else {
            self.delay(100);
            self.burst(0x11, &[]);
            self.delay(120);
            self.burst(0x36, &[0]);
            self.burst(0x3a, &[5]);
            self.burst(0xb2, &[12, 12, 0, 0x33, 0x33]);
            for (command, value) in [
                (0xb7, 0x35),
                (0xbb, 0x32),
                (0xc2, 1),
                (0xc3, 0x15),
                (0xc4, 0x20),
                (0xc6, 0xf),
            ] {
                self.burst(command, &[value]);
            }
            const GAMMA: [u8; 14] = [
                0xd0, 8, 14, 9, 9, 5, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34,
            ];
            self.burst(0xd0, &[0xa4, 0xa1]);
            self.burst(0xe0, &GAMMA);
            self.burst(0xe1, &GAMMA);
            self.burst(0x21, &[]);
        }
        self.rect(0, 0, 240, 240, 0);
        self.burst(0x29, &[]);
        if fm1 {
            gpio_level(0, 2, false);
        }
        self.active
    }

    fn row(&mut self, row: u32, fg: u16, bg: u16, text: &[u8]) {
        if !self.active || row >= 15 {
            return;
        }
        for c in 0..20 {
            if !self.active {
                break;
            }
            let ch = text.get(c).map_or(b' ', |b| b.to_ascii_uppercase());
            let glyph = &GLYPHS[CHARACTERS
                .iter()
                .position(|&known| known == ch)
                .unwrap_or(0)];
            let mut pixels = self.pixels.0.as_chunks_mut().0.iter_mut();
            for y in 0..16 {
                for x in 0..12 {
                    let lit = x < 10 && y < 14 && glyph[x / 2] & 1 << (y / 2) != 0;
                    if let Some(pixel) = pixels.next() {
                        *pixel = (if lit { fg } else { bg }).to_be_bytes();
                    }
                }
            }
            self.window(c as u32 * 12, row * 16, 12, 16);
            self.dma(CELL as u32);
            self.select(false);
        }
    }

    fn progress(&mut self, percent: u32, color: u16) {
        self.rect(10, 148, 220, 2, 0xffff);
        self.rect(10, 172, 220, 2, 0xffff);
        self.rect(10, 150, 2, 22, 0xffff);
        self.rect(228, 150, 2, 22, 0xffff);
        self.rect(12, 150, 216, 22, 0);
        self.rect(12, 152, 216 * percent.min(100) / 100, 18, color);
    }
}

pub fn init(board: u32) -> bool {
    display().init(board)
}

pub fn rect(x: u32, y: u32, w: u32, h: u32, color: u16) {
    display().rect(x, y, w, h, color)
}

/// A row of text: 15 rows of 20 characters, on black.
pub fn text(row: u32, fg: u16, text: Text) {
    line(row, fg, text.as_bytes())
}

pub fn line(row: u32, fg: u16, text: &[u8]) {
    display().row(row, fg, 0, text)
}

/// The bar of an operation that takes a while, in a frame.
pub fn progress(percent: u32, color: u16) {
    display().progress(percent, color)
}

pub fn release() {
    display().active = false;
    SPI1_CON.write(0);
}

// The C interface (include/display.h).
#[unsafe(no_mangle)]
pub extern "C" fn display_init(board: u32) -> bool {
    init(board)
}

#[unsafe(no_mangle)]
pub extern "C" fn display_rect(x: u32, y: u32, w: u32, h: u32, color: u16) {
    display().rect(x, y, w, h, color)
}

/// w x h pixels from data, RGB565 big-endian, row by row. For RAM apps: the bootloader doesn't use it.
///
/// # Safety
/// `data` holds `w * h * 2` bytes.
#[cfg(not(target_arch = "pi32v2"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_pixels(x: u32, y: u32, w: u32, h: u32, data: *const u8) {
    let d = display();
    // Checked before the size is worked out, which a wild width would overflow.
    if d.visible(x, y, w, h) {
        d.pixels(x, y, w, h, unsafe {
            core::slice::from_raw_parts(data, (w * h * 2) as usize)
        })
    }
}

/// # Safety
/// `text` holds `n` bytes.
#[cfg(any(not(target_arch = "pi32v2"), loader_a, loader_b))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_row(row: u32, fg: u16, bg: u16, text: *const u8, n: u32) {
    display().row(row, fg, bg, unsafe {
        core::slice::from_raw_parts(text, n.min(20) as usize)
    })
}

/// # Safety
/// `text` is NUL-terminated.
#[cfg(not(target_arch = "pi32v2"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_text(row: u32, fg: u16, text: *const core::ffi::c_char) {
    display().row(
        row,
        fg,
        0,
        unsafe { core::ffi::CStr::from_ptr(text) }.to_bytes(),
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn display_progress(percent: u32, color: u16) {
    progress(percent, color)
}

#[unsafe(no_mangle)]
pub extern "C" fn display_release() {
    release()
}
