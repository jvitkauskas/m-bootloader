// SPDX-License-Identifier: MIT
//! What the firmware and updaters are handed at start. Pointers on the wire are little-endian 32-bit CPU addresses.
use crate::formats::{App, Config, Request, put_le16, put_le32, request_valid};

pub fn config(clock: &mut [u8; 52], memory: &mut [u8; 60], c: &Config) {
    *clock = [0; 52];
    *memory = [0; 60];
    put_le32(
        clock,
        0,
        (c.high_current as u32) << 12 | (c.one_pin as u32) << 13 | (c.lsb_divider as u32) << 14,
    );
    put_le32(clock, 4, c.oscillator_hz);
    put_le32(clock, 8, c.system_hz);
    clock[48] = c.reset_pin;
    clock[49] = c.reset_level;
    clock[50] = c.reset_hold;
    put_le32(memory, 12, (c.hsb_divider & 15) as u32);
}

pub fn app(out: &mut [u8; 24], physical: u32, key: u16, trim: u16, mac: &[u8; 8]) {
    put_le32(out, 0, 0x01c7_fe20);
    put_le32(out, 4, physical);
    put_le32(out, 8, 0x0200_0000);
    put_le16(out, 12, key);
    put_le16(out, 14, trim);
    *out.last_chunk_mut().unwrap() = *mac;
}

pub fn updater(out: &mut [u8; 24], app: &App, key: u16, clock: u32, private_info: u32) {
    *out = [0; 24];
    put_le32(out, 0, app.entry);
    put_le32(out, 4, app.base);
    put_le32(out, 8, clock);
    put_le16(out, 12, key);
    put_le32(out, 20, private_info);
}

pub fn consume_bypass(retained: &mut [u8; 8]) -> bool {
    if retained != b"BOOTANY\0" {
        return false;
    }
    *retained = [0; 8];
    true
}

/// 1: original RAM request; 2: scan flash; 0: normal app path (UART may apply).
pub fn update_choice(request: &Request, mounted: bool) -> u32 {
    if request_valid(request) {
        1
    } else if mounted {
        0
    } else {
        2
    }
}

/// The C interface (include/contract.h).
pub mod c {
    use super::*;

    #[unsafe(no_mangle)]
    pub extern "C" fn contract_config(clock: &mut [u8; 52], memory: &mut [u8; 60], c: &Config) {
        config(clock, memory, c)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn contract_app(
        out: &mut [u8; 24],
        physical: u32,
        key: u16,
        trim: u16,
        mac: &[u8; 8],
    ) {
        app(out, physical, key, trim, mac)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn contract_updater(
        out: &mut [u8; 24],
        app: &App,
        key: u16,
        clock: u32,
        private_info: u32,
    ) {
        updater(out, app, key, clock, private_info)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn contract_consume_bypass(retained: &mut [u8; 8]) -> bool {
        consume_bypass(retained)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn contract_update_choice(request: &Request, mounted: bool) -> u32 {
        update_choice(request, mounted)
    }
}
