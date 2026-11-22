// SPDX-License-Identifier: MIT
//! The flash loader that M-Flasher runs on the chip, as a plug-in of the ROM's download mode: docs/SPEC.md §10.
use crate::formats::{
    CNor, Flash, Nor, be16, be32, crc16, le16, le32, put_be16, put_be32, put_le32, range,
    usb_begin, usb_stream,
};
use core::ffi::c_void;

/// What it answers to FC40, after "MFLR".
const VERSION: &[u8; 7] = b"MFLR1.4";

/// What the loader works through: the ROM's USB callbacks, and the chip. C lays these two out, for the tests.
#[repr(C)]
pub struct Services {
    pub nor: CNor,
    pub send: unsafe extern "C" fn(*const c_void, u32) -> i32,
    pub receive: unsafe extern "C" fn(*mut c_void, u32) -> i32,
    pub read_start: Option<unsafe extern "C" fn(u32, *mut c_void, u32) -> bool>,
    pub read_finish: Option<extern "C" fn() -> bool>,
    pub jedec: extern "C" fn() -> u32,
    pub key: extern "C" fn() -> u16,
    pub uid: extern "C" fn(&mut [u8; 8]),
    pub screen_init: Option<extern "C" fn(u32) -> bool>,
    pub screen_row: Option<unsafe extern "C" fn(u32, u16, u16, *const u8, u32)>,
    pub screen_progress: Option<extern "C" fn(u32, u16)>,
    pub entry: u32,
    pub cpu_mhz: u8,
}

#[repr(C)]
pub struct State {
    io: Services,
    crc_table: [u32; 256],
    buffer: [[u8; 4096]; 2],
    error: u8,
    screen: u8,
}

fn send_bytes(io: &Services, data: &[u8]) {
    for part in data.chunks(512) {
        unsafe { (io.send)(part.as_ptr().cast(), part.len() as u32) };
    }
}

fn receive_bytes(io: &Services, data: &mut [u8]) {
    for part in data.chunks_mut(512) {
        unsafe { (io.receive)(part.as_mut_ptr().cast(), part.len() as u32) };
    }
}

impl State {
    /// Takes `n` bytes from the host, and drops them.
    fn drain(&mut self, mut n: u32) {
        while n != 0 {
            let k = n.min(512);
            receive_bytes(&self.io, &mut self.buffer[1][..k as usize]);
            n -= k;
        }
    }

    /// The first bytes of an answer: as many as the host asked for.
    fn answer(&self, ans: &[u8; 16], count: u32) {
        send_bytes(&self.io, &ans[..count.min(16) as usize]);
    }

    fn crc32(&self, mut c: u32, data: &[u8]) -> u32 {
        for &b in data {
            c = c >> 8 ^ self.crc_table[(c as u8 ^ b) as usize];
        }
        c
    }

    /// Sends the flash from `a` (mode 0), or puts its CRC16 (1) or CRC32 (2) in the answer. A second DMA runs while
    /// the completed first buffer is sent or summed.
    fn stream(&mut self, mut a: u32, mut n: u32, mode: u32, answer: &mut [u8; 16]) -> bool {
        let mut index = 0;
        let mut crc = if mode == 2 { u32::MAX } else { 0 };
        let dma = self.io.read_start.zip(self.io.read_finish);
        let mut k = n.min(4096);
        if k != 0
            && let Some((start, _)) = dma
            && !unsafe { start(a, self.buffer[index].as_mut_ptr().cast(), k) }
        {
            return false;
        }
        while k != 0 {
            let ok = match dma {
                Some((_, finish)) => finish(),
                None => self
                    .io
                    .nor
                    .flash
                    .read(a, &mut self.buffer[index][..k as usize]),
            };
            if !ok {
                return false;
            }
            a += k;
            n -= k;
            let next = n.min(4096);
            if next != 0
                && let Some((start, _)) = dma
                && !unsafe { start(a, self.buffer[index ^ 1].as_mut_ptr().cast(), next) }
            {
                return false;
            }
            let p = &self.buffer[index][..k as usize];
            match mode {
                0 => send_bytes(&self.io, p),
                1 => crc = crc16(crc as u16, p) as u32,
                _ => crc = self.crc32(crc, p),
            }
            index ^= 1;
            k = next;
        }
        if mode == 1 {
            put_be16(answer, 2, crc as u16);
        }
        if mode == 2 {
            put_be32(answer, 2, !crc);
        }
        true
    }

    /// 1 if the command was the loader's, with its answer sent; 0 to leave it to the ROM (FB06, FD07 and FB08 are).
    fn dispatch(&mut self, cbw: &[u8; 31], ans: &mut [u8; 16]) -> i32 {
        let c: &[u8; 16] = cbw.last_chunk().unwrap();
        let op = be16(c, 0);
        if !matches!(
            op,
            0xfb00
                | 0xfb01
                | 0xfb02
                | 0xfb04
                | 0xfd05
                | 0xfc09
                | 0xfc0a
                | 0xfc0b
                | 0xfc0e
                | 0xfc12
                | 0xfc14
                | 0xfc40
                | 0xfc41
                | 0xfc42
                | 0xfc43
                | 0xfc44
                | 0xfc45
                | 0xfc46
        ) {
            return 0;
        }
        let (count, a, mut n) = (le32(cbw, 8), be32(c, 2), be16(c, 6) as u32);
        let inward = cbw[12] & 128 != 0;
        let capacity = self.io.nor.flash.capacity;
        *ans = [0; 16];
        ans[0] = c[0];
        ans[1] = c[1];
        let out = op == 0xfb04 || op == 0xfc44;
        let refused = if op == 0xfb02 || op == 0xfc12 {
            5
        } else if cbw[14] != 16 || (count != 0 && inward == out) {
            7
        } else {
            0
        };
        if refused != 0 {
            if !inward {
                self.drain(count);
            }
            self.error = refused;
            if inward {
                self.answer(ans, count);
            }
            return 1;
        }
        if out {
            let declared = if op == 0xfb04 { n } else { be16(c, 7) as u32 };
            // Keep complete OUT framing even when text cannot be drawn.
            let refused = if count != declared {
                7
            } else if op == 0xfb04 && n > 4096 {
                1
            } else if op == 0xfc44 && (count > 20 || c[2] > 14) {
                6
            } else {
                0
            };
            if refused != 0 {
                self.drain(count);
                self.error = refused;
                return 1;
            }
            let data = &mut self.buffer[0][..count as usize];
            receive_bytes(&self.io, data);
            let mut data = &*data;
            if op == 0xfc44 {
                if self.screen != 0
                    && let Some(row) = self.io.screen_row
                {
                    unsafe { row(c[2] as u32, be16(c, 3), be16(c, 5), data.as_ptr(), count) };
                }
                return 1;
            }
            if crc16(0, data) != le16(c, 9) {
                self.error = 2;
                return 1;
            }
            if !range(a, n, capacity) {
                self.error = 3;
                return 1;
            }
            // Verify whole payload first, then split pages independently of HAL.
            let mut at = a;
            while !data.is_empty() {
                let (page, rest) = data.split_at((256 - (at & 255) as usize).min(data.len()));
                if !self.io.nor.program(at, page) {
                    self.error = 4;
                    break;
                }
                at += page.len() as u32;
                data = rest;
            }
            return 1;
        }
        if op == 0xfd05 && count != n {
            self.error = 7;
            self.answer(ans, count);
            return 1;
        }
        if op != 0xfd05 && count > 16 {
            self.error = 7;
            self.answer(ans, 16);
            return 1;
        }
        match op {
            0xfb00 | 0xfb01 => {
                let size = if op == 0xfb00 { 65536 } else { 4096 };
                if a & (size - 1) != 0 || !range(a, size, capacity) {
                    self.error = 3;
                } else if !self.io.nor.erase(a, size) {
                    self.error = 4;
                }
            }
            0xfd05 => {
                if range(a, n, capacity) {
                    if !self.stream(a, n, 0, ans) {
                        self.error = 4;
                    }
                    return 1;
                }
                self.error = 3;
            }
            0xfc09 => {
                let (mut k, mut i) = ((self.io.key)().to_le_bytes(), 0);
                usb_stream(usb_begin(u32::MAX), &mut i, &mut k);
                ans[7] = k[0];
                ans[6] = k[1];
            }
            0xfc0a => {
                ans[2] = 3;
                put_le32(ans, 4, (self.io.jedec)());
            }
            0xfc0b => {
                let id = (self.io.jedec)().to_be_bytes();
                ans[2..5].copy_from_slice(&id[1..]);
            }
            0xfc0e | 0xfc41 => {
                if op == 0xfc41 {
                    n = be32(c, 6);
                }
                if !range(a, n, capacity) {
                    self.error = 3;
                } else if !self.stream(a, n, if op == 0xfc41 { 2 } else { 1 }, ans) {
                    self.error = 4;
                }
            }
            0xfc14 => put_be32(ans, 2, 4096),
            0xfc40 => {
                ans[2..9].copy_from_slice(VERSION);
                put_be32(ans, 10, self.io.entry);
                ans[14] = 15;
                ans[15] = self.io.cpu_mhz;
            }
            0xfc42 => (self.io.uid)((&mut ans[2..10]).try_into().unwrap()),
            0xfc43 => {
                self.screen = 0;
                if (c[2] == 1 || c[2] == 2)
                    && let Some(init) = self.io.screen_init
                {
                    self.screen = init(c[2] as u32) as u8;
                }
                ans[2] = self.screen;
            }
            0xfc45 => {
                if self.screen != 0
                    && let Some(progress) = self.io.screen_progress
                {
                    progress(c[2].min(100) as u32, be16(c, 3));
                }
            }
            0xfc46 => {
                ans[2] = self.error;
                self.error = 0;
            }
            _ => {}
        }
        self.answer(ans, count);
        1
    }
}

/// # Safety
/// `s` is room for a `State`, whatever it holds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn loader_init(s: *mut State, io: &Services) {
    // Safety: all zeros, then the services, make a State: its other fields are numbers.
    unsafe {
        s.write_bytes(0, 1);
        (&raw mut (*s).io).copy_from_nonoverlapping(io, 1);
        for (i, entry) in (&mut (*s).crc_table).iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                c = c >> 1 ^ if c & 1 != 0 { 0xedb8_8320 } else { 0 };
            }
            *entry = c;
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn loader_dispatch(s: &mut State, command: &[u8; 31], answer: &mut [u8; 16]) -> i32 {
    s.dispatch(command, answer)
}

/// The loader as the ROM starts it: one for each address it is linked for (§10.1). The tests have both in one program.
#[allow(unused_macros)]
macro_rules! target {
    ($module:ident, $entry:ident, $address:expr $(, $state:ident)?) => {
        pub mod $module {
            use super::{Services, State, loader_init};
            use crate::Global;
            use crate::formats::{CFlash, CNor};
            use crate::hw::sync_cpu;
            use crate::platform::*;
            use core::mem::MaybeUninit;

            static LOADER: Global<MaybeUninit<State>> = Global::new(MaybeUninit::zeroed());

            extern "C" fn handler(command: &[u8; 31], answer: &mut [u8; 16]) -> i32 {
                // Safety: the entry below made it a State before the ROM was given this.
                unsafe { LOADER.get().assume_init_mut() }.dispatch(command, answer)
            }

            extern "C" fn key() -> u16 {
                (hal_efuse(1, 3) >> 8) as u16
            }

            /// `params`: the ROM's send and receive, and where it takes the plug-in's handler from.
            ///
            /// # Safety
            /// As the ROM calls it.
            #[unsafe(no_mangle)]
            #[cfg_attr(target_arch = "pi32v2", unsafe(link_section = ".entry"))]
            pub unsafe extern "C" fn $entry(params: &[u32; 3]) -> i32 {
                #[cfg(target_arch = "pi32v2")]
                unsafe {
                    unsafe extern "C" {
                        static mut __bss_start: u8;
                        static __bss_end: u8;
                    }
                    core::ptr::write_bytes(&raw mut __bss_start, 0, crate::hw::distance(&raw const __bss_start, &raw const __bss_end));
                }
                // Keep ROM's watchdog policy; B never takes A's power/exception path.
                hal_fault_clear();
                let mhz = hal_clock_b();
                flash_init(1, 1);
                let nor = CNor {
                    flash: CFlash { ctx: core::ptr::null_mut(), capacity: flash_capacity(), read: flash_read },
                    program: flash_program,
                    erase: flash_erase,
                };
                // Safety: the ROM's two callbacks, at the addresses it gave.
                let io = unsafe {
                    Services {
                        nor,
                        send: core::mem::transmute::<usize, unsafe extern "C" fn(*const core::ffi::c_void, u32) -> i32>(params[0] as usize),
                        receive: core::mem::transmute::<usize, unsafe extern "C" fn(*mut core::ffi::c_void, u32) -> i32>(params[1] as usize),
                        read_start: Some(flash_dma_start),
                        read_finish: Some(flash_dma_finish),
                        jedec: flash_jedec,
                        key,
                        uid: flash_uid,
                        screen_init: Some(crate::display::display_init),
                        screen_row: Some(crate::display::display_row),
                        screen_progress: Some(crate::display::display_progress),
                        entry: $address,
                        cpu_mhz: mhz as u8,
                    }
                };
                unsafe { loader_init(LOADER.get().as_mut_ptr(), &io) };
                sync_cpu();
                // Safety: the ROM's slot for a plug-in's handler.
                unsafe { (params[2] as usize as *mut u32).write_volatile(handler as *const () as usize as u32) };
                sync_cpu();
                0
            }

            $(
                /// For the tests: this slot's private state.
                #[unsafe(no_mangle)]
                pub extern "C" fn $state() -> *mut State {
                    unsafe { LOADER.get().as_mut_ptr() }
                }
            )?
        }
    };
}

#[cfg(loader_a)]
target!(target, loader_entry, 0x01c2_0000);
#[cfg(loader_b)]
target!(target, loader_entry, 0x01c2_8000);
#[cfg(boot_sim)]
target!(
    slot_a,
    loader_test_install_a,
    0x01c2_0000,
    loader_test_state_a
);
#[cfg(boot_sim)]
target!(
    slot_b,
    loader_test_install_b,
    0x01c2_8000,
    loader_test_state_b
);
