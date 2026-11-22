// SPDX-License-Identifier: MIT
//! A USB-MIDI device on USB0: only what MIDI mode needs, polled, with no interrupts. The register sequences were
//! written from the facts in JieLi's AC79 SDK (Apache-2.0) and felucca-bsp's chip/fm1_usb.h (MPL-2.0); no code was
//! taken from either (NOTICE.md). The controller is a MUSB, whose registers are reached one at a time through USB_CON1.
use crate::Global;
use crate::hw::{Reg, bit, dma_address, sync_cpu};
use crate::midi_update::mu_rx;
use crate::platform::{hal_delay_us, settle};

const CON0: Reg = Reg::at::<0x11800>();
const CON1: Reg = Reg::at::<0x11804>();
const EP0_COUNT: Reg = Reg::at::<0x11808>();
const EP1_COUNT: Reg = EP0_COUNT.add(4);
const EP0_ADDRESS: Reg = Reg::at::<0x11818>();
const EP1_TX_ADDRESS: Reg = Reg::at::<0x1181c>();
/// Endpoints 1 to 3: eight bytes apart.
const EP1_RX_ADDRESS: Reg = Reg::at::<0x11820>();
const EP4_RX_ADDRESS: Reg = Reg::at::<0x1183c>();
const IO_CON0: Reg = Reg::at::<0x51000>();
const CLOCK_CON1: Reg = Reg::at::<0x10010>();
// The MUSB's registers.
const FADDR: u32 = 0;
const POWER: u32 = 1;
const INT_TX1: u32 = 2;
const INT_RX1: u32 = 4;
const INT_USB: u32 = 6;
const INT_TX1_ENABLE: u32 = 7;
const INT_TX2_ENABLE: u32 = 8;
const INT_RX1_ENABLE: u32 = 9;
const INT_RX2_ENABLE: u32 = 10;
const INT_USB_ENABLE: u32 = 11;
const INDEX: u32 = 14;
const TX_MAX: u32 = 16;
const CSR0: u32 = 17;
const TX_CSR1: u32 = 17;
const TX_CSR2: u32 = 18;
const RX_MAX: u32 = 19;
const RX_CSR1: u32 = 20;
const RX_CSR2: u32 = 21;
const RX_COUNT1: u32 = 22;
const RX_COUNT2: u32 = 23;
const WAIT_LIMIT: u32 = 20000;
const PACKET: usize = 64;

static DEVICE_DESCRIPTOR: [u8; 18] = [18, 1, 0x10, 0x01, 0, 0, 0, PACKET as u8, 0x09, 0x12, 0x02, 0x00, 0x00, 0x02, 0, 1, 0, 1];
#[rustfmt::skip]
static CONFIG_DESCRIPTOR: [u8; 101] = [
    9, 2, 101, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,                     // audio control
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,                     // MIDI streaming
    7, 0x24, 1, 0x00, 0x01, 65, 0,
    6, 0x24, 2, 1, 1, 0,                           // jacks
    6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 0x01, 2, PACKET as u8, 0, 0, 0, 0,       // endpoint 1, out
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, PACKET as u8, 0, 0, 0, 0,       // endpoint 1, in
    5, 0x25, 1, 1, 3,
];
static LANGUAGE_STRING: [u8; 4] = [4, 3, 0x09, 0x04];
const PRODUCT: &[u8] = if cfg!(smk) { b"SMK-37 Update" } else { b"FM-1 Update" };
/// The product's name as a string descriptor: UTF-16.
static PRODUCT_STRING: [u8; 2 + 2 * PRODUCT.len()] = {
    let mut d = [0; 2 + 2 * PRODUCT.len()];
    d[0] = d.len() as u8;
    d[1] = 3;
    let mut i = 0;
    while i < PRODUCT.len() {
        d[2 + 2 * i] = PRODUCT[i];
        i += 1;
    }
    d
};
static ZERO: [u8; 2] = [0; 2];

/// What the controller's DMA reads and writes: aligned for it.
#[repr(align(4))]
struct Buffer<const N: usize>([u8; N]);

impl<const N: usize> Buffer<N> {
    /// A byte the controller wrote: read where it is, each time.
    fn received(&self, i: usize) -> u8 {
        unsafe { (&raw const self.0[i]).read_volatile() }
    }

    fn address(&mut self) -> u32 {
        dma_address(self.0.as_mut_ptr())
    }
}

struct Usb {
    // The controller's DMA writes past a full packet when receiving, so these have room to spare.
    ep0: Buffer<{ PACKET + 4 }>,
    ep1_in: Buffer<PACKET>,
    ep1_out: Buffer<{ PACKET + 4 }>,
    /// What is left of the answer being sent on endpoint 0. (None, not an empty slice, which isn't all zeros: one
    /// byte that isn't zero would put all of this in the image.)
    ep0_data: Option<&'static [u8]>,
    pending_address: u8,
    configuration: u8,
    address_pending: bool,
    ep0_sending: bool,
    zero_end: bool,
    running: bool,
}

static USB: Global<Usb> = Global::new(Usb {
    ep0: Buffer([0; PACKET + 4]),
    ep1_in: Buffer([0; PACKET]),
    ep1_out: Buffer([0; PACKET + 4]),
    ep0_data: None,
    pending_address: 0,
    configuration: 0,
    address_pending: false,
    ep0_sending: false,
    zero_end: false,
    running: false,
});

/// Safety, for every use: these entries are not entered from what they call (the timer, and MIDI mode's receiver).
fn usb() -> &'static mut Usb {
    unsafe { USB.get() }
}

/// Waits until what the controller sent to memory can be read.
fn memory_sync() {
    #[cfg(target_arch = "pi32v2")]
    unsafe {
        core::arch::asm!("ssync", options(nostack, preserves_flags))
    }
}

/// Accesses a few cycles apart make the controller babble on the bus, and the host then turns the port off: seen on
/// the FM-1, and warned of in felucca-bsp's chip/usb.c. So they're kept apart.
#[inline(never)]
fn sie_start(command: u32) {
    settle(40);
    CON1.write(command);
    sync_cpu();
}

fn sie_write(r: u32, v: u32) {
    if CON0.read() & 4 == 0 {
        return;
    }
    sie_start(r << 8 & 0xff_ff00 | v & 0xff);
    let mut n = WAIT_LIMIT;
    while n != 0 && CON1.read() & 0x8000 == 0 {
        n -= 1;
    }
}

fn sie_read(r: u32) -> u32 {
    if CON0.read() & 4 == 0 {
        return 0;
    }
    sie_start(r << 8 & 0xff_bf00 | 0x4000);
    let mut n = WAIT_LIMIT;
    while n != 0 {
        if CON1.read() & 0x8000 != 0 {
            return CON1.read() & 0xff;
        }
        n -= 1;
    }
    0
}

fn descriptor(value: u32) -> Option<&'static [u8]> {
    match (value >> 8, value & 255) {
        (1, _) => Some(&DEVICE_DESCRIPTOR),
        (2, _) => Some(&CONFIG_DESCRIPTOR),
        (3, 0) => Some(&LANGUAGE_STRING),
        (3, 1) => Some(&PRODUCT_STRING),
        _ => None,
    }
}

impl Usb {
    fn ep0_chunk(&mut self) {
        let data = self.ep0_data.unwrap_or(&[]);
        let n = data.len().min(PACKET);
        let (now, rest) = data.split_at(n);
        self.ep0.0[..n].copy_from_slice(now);
        sync_cpu();
        EP0_ADDRESS.write(self.ep0.address());
        EP0_COUNT.write(n as u32);
        self.ep0_data = Some(rest);
        let last = rest.is_empty() && !(n == PACKET && self.zero_end);
        if rest.is_empty() && n == PACKET {
            self.zero_end = false;
        }
        sie_write(INDEX, 0);
        sie_write(CSR0, if last { 0x0a } else { 0x02 });
        self.ep0_sending = !last;
    }

    fn ep0_send(&mut self, data: &'static [u8], asked: usize) {
        let size = data.len().min(asked);
        self.ep0_data = Some(&data[..size]);
        self.zero_end = size < asked && size % PACKET == 0;
        sie_write(INDEX, 0);
        sie_write(CSR0, 0x40);
        self.ep0_chunk();
    }

    fn ep1_configure(&mut self) {
        EP1_TX_ADDRESS.write(self.ep1_in.address());
        EP1_RX_ADDRESS.write(self.ep1_out.address());
        sie_write(INDEX, 1);
        sie_write(TX_MAX, 0xff);
        sie_write(TX_CSR1, 0x48);
        sie_write(TX_CSR2, 0);
        sie_write(RX_MAX, 0xff);
        sie_write(RX_CSR1, 0x90);
        sie_write(RX_CSR2, 0);
        sie_write(INT_RX1_ENABLE, 0x02);
        CON0.clear(1 << 20);
    }

    /// Answers the request in `ep0`, or says what to put in CSR0 for it: 0x48 for done, 0x60 for a stall.
    fn ep0_request(&mut self) -> Option<u32> {
        const DONE: Option<u32> = Some(0x48);
        const STALL: Option<u32> = Some(0x60);
        let ep0 = |i| self.ep0.received(i) as u32;
        let (value, asked) = (ep0(2) | ep0(3) << 8, (ep0(6) | ep0(7) << 8) as usize);
        match ep0(0) << 8 | ep0(1) {
            // set address
            0x0005 => {
                self.pending_address = value as u8 & 0x7f;
                self.address_pending = true;
            }
            // get descriptor
            0x8006 => {
                let Some(d) = descriptor(value) else { return STALL };
                if asked != 0 {
                    self.ep0_send(d, asked);
                    return None;
                }
            }
            // set configuration
            0x0009 => {
                if value & 255 > 1 {
                    return STALL;
                }
                self.configuration = value as u8;
                if self.configuration != 0 {
                    self.ep1_configure();
                }
            }
            0x8008 => {
                self.ep0_send(if self.configuration != 0 { &[1] } else { &ZERO[..1] }, asked);
                return None;
            }
            0x8000 | 0x8100 | 0x8200 => {
                self.ep0_send(&ZERO, asked);
                return None;
            }
            // clear or set endpoint or interface features
            0x0102 | 0x0302 | 0x000b | 0x010b => {}
            _ => return STALL,
        }
        DONE
    }

    fn ep0_service(&mut self) {
        if self.address_pending {
            sie_write(FADDR, self.pending_address as u32);
            self.address_pending = false;
        }
        sie_write(INDEX, 0);
        let csr = sie_read(CSR0);
        // we sent a stall
        if csr & 0x04 != 0 {
            sie_write(CSR0, 0);
            self.ep0_sending = false;
            return;
        }
        // the host abandoned the transfer
        if csr & 0x10 != 0 {
            sie_write(CSR0, 0x80);
            self.ep0_sending = false;
        }
        if self.ep0_sending {
            if csr & 0x02 == 0 {
                self.ep0_chunk();
            }
            return;
        }
        if csr & 0x01 == 0 {
            return;
        }
        memory_sync();
        if let Some(answer) = self.ep0_request() {
            sie_write(INDEX, 0);
            sie_write(CSR0, answer);
        }
    }

    fn ep1_receive(&mut self) {
        sie_write(INDEX, 1);
        let csr = sie_read(RX_CSR1) | sie_read(RX_CSR2) << 8;
        if csr & 1 == 0 {
            return;
        }
        let n = ((sie_read(RX_COUNT1) | sie_read(RX_COUNT2) << 8) as usize).min(PACKET);
        memory_sync();
        let mut i = 0;
        while i + 3 < n {
            // Code index 4 to 7 are the SysEx ones, and say how many of the 3 bytes count.
            let code = self.ep1_out.received(i) & 15;
            if (4..=7).contains(&code) {
                let count = match code {
                    5 => 1,
                    6 => 2,
                    _ => 3,
                };
                for b in 0..count {
                    mu_rx(self.ep1_out.received(i + 1 + b));
                }
            }
            i += 4;
        }
        let csr = csr & !0x164 | 0x10;
        sie_write(RX_CSR1, csr & 0xff);
        sie_write(RX_CSR2, csr >> 8);
    }

    fn poll(&mut self) {
        if !self.running {
            return;
        }
        // a frame's start is pending: acknowledge it
        if CON0.read() & bit(13) != 0 {
            CON0.set(bit(12));
        }
        let (bus, tx, rx) = (sie_read(INT_USB), sie_read(INT_TX1), sie_read(INT_RX1));
        // bus reset
        if bus & 0x04 != 0 {
            sie_write(FADDR, 0);
            self.configuration = 0;
            self.ep0_sending = false;
            self.address_pending = false;
            EP0_ADDRESS.write(self.ep0.address());
            sie_write(INT_USB_ENABLE, 0x07);
            sie_write(INT_TX1_ENABLE, 0x01);
            sie_write(INT_RX1_ENABLE, 0);
        }
        if tx & 0x01 != 0 {
            self.ep0_service();
        }
        if rx & 0x02 != 0 && self.configuration != 0 {
            self.ep1_receive();
        }
    }

    fn start(&mut self) {
        CON0.write(0);
        CON1.write(0);
        IO_CON0.write(0x0c);
        hal_delay_us(25000);
        // USB clock: PLL 48 MHz
        CLOCK_CON1.clear(3);
        let ep0 = self.ep0.address();
        EP0_ADDRESS.write(ep0);
        for n in 0..3 {
            EP1_RX_ADDRESS.add(8 * n).write(ep0);
        }
        EP4_RX_ADDRESS.write(ep0);
        IO_CON0.set(bit(10) | bit(9));
        IO_CON0.clear(bit(11));
        IO_CON0.set(bit(12));
        CON0.clear(0x4035);
        CON1.write(0);
        CON0.set(1);
        IO_CON0.clear(0xf0);
        // D+ pull-up: on the bus
        IO_CON0.set(0x40);
        CON0.set(0x3c);
        sie_write(POWER, 0x60);
        sie_write(INT_USB_ENABLE, 0x07);
        sie_write(INT_TX1_ENABLE, 0x01);
        sie_write(INT_TX2_ENABLE, 0);
        sie_write(INT_RX1_ENABLE, 0);
        sie_write(INT_RX2_ENABLE, 0);
        CON0.clear(0x1f << 19);
        self.running = true;
        self.configuration = 0;
    }

    fn ep1_wait(&mut self) -> bool {
        // About 200 ms of polling: a host that isn't reading loses the message, and asks again.
        for _ in 0..400 {
            sie_write(INDEX, 1);
            let csr = sie_read(TX_CSR1);
            if csr & 0x01 == 0 {
                if csr & 0x80 != 0 {
                    sie_write(TX_CSR1, csr & !0x80);
                }
                return true;
            }
            self.poll();
            hal_delay_us(500);
        }
        false
    }

    fn send(&mut self, mut data: &[u8]) {
        while !data.is_empty() {
            if self.configuration == 0 || !self.ep1_wait() {
                return;
            }
            let mut n = 0;
            for event in self.ep1_in.0.as_chunks_mut::<4>().0 {
                let count = data.len().min(3);
                if count == 0 {
                    break;
                }
                let (now, rest) = data.split_at(count);
                *event = [0; 4];
                event[0] = if count == 3 && rest.is_empty() { 7 } else { 4 + (count as u8 % 3) };
                event[1..1 + count].copy_from_slice(now);
                data = rest;
                n += 4;
            }
            sync_cpu();
            EP1_TX_ADDRESS.write(self.ep1_in.address());
            EP1_COUNT.write(n);
            sie_write(INDEX, 1);
            sie_write(TX_CSR1, sie_read(TX_CSR1) | 0x01);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn usb_midi_start() {
    usb().start()
}

#[unsafe(no_mangle)]
pub extern "C" fn usb_midi_poll() {
    usb().poll()
}

#[unsafe(no_mangle)]
pub extern "C" fn usb_midi_configured() -> bool {
    usb().configuration != 0
}

/// Sends one whole F0..F7 message, polling until it has gone or the host has gone.
///
/// # Safety
/// `data` holds `size` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usb_midi_send(data: *const u8, size: u32) {
    usb().send(unsafe { core::slice::from_raw_parts(data, size as usize) })
}

#[unsafe(no_mangle)]
pub extern "C" fn usb_midi_stop() {
    usb().running = false;
    CON0.write(0);
    IO_CON0.write(0x0c);
}
