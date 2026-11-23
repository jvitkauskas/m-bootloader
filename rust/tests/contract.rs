// SPDX-License-Identifier: MIT
//! What the firmware and updaters are handed at start (docs/SPEC.md §6): byte for byte, and what is asked of the
//! retained area once only.
use crate::contract;
use crate::formats::{
    App, Config, Request, crc16, le32, put_le16, put_le32, request_valid, uart_request,
};

fn test_app() {
    let mut out = [0xa5; 24];
    let mut mac = [0, 1, 2, 3, 4, 5, 0x34, 0x12];
    let expected = [
        0x20, 0xfe, 0xc7, 1, 0, 0x40, 0, 0, 0, 0, 0, 2, 0x0f, 0x98, 0xcd, 0xab, 0, 1, 2, 3, 4, 5,
        0x34, 0x12,
    ];
    contract::app(&mut out, 0x4000, 0x980f, 0xabcd, &mac);
    assert!(out == expected);
    mac = [255; 8];
    contract::app(&mut out, 0x20000, 0xffff, 0, &mac);
    assert!(le32(&out, 4) == 0x20000 && le32(&out, 8) == 0x0200_0000);
    assert!(out[16..] == mac);
}

fn test_config() {
    let mut c = Config::DEFAULTS;
    let (mut clock, mut memory) = ([0xa5; 52], [0xa5; 60]);
    let (mut expected_clock, mut expected_memory) = ([0; 52], [0; 60]);
    expected_clock[1] = 0x80;
    // LSB encoding 2 at bits17:14; compatibility OSC tag0.
    put_le32(&mut expected_clock, 4, 24_000_000);
    put_le32(&mut expected_clock, 8, 360_000_000);
    expected_clock[48] = 17;
    expected_clock[50] = 8;
    expected_memory[12] = 1;
    contract::config(&mut clock, &mut memory, &c);
    assert!(clock == expected_clock);
    assert!(memory == expected_memory);
    // Applied default voltage codes11/3 must not leak into absent parsed metadata.
    assert!(le32(&memory, 52) == 0);
    c.high_current = 1;
    c.one_pin = 1;
    c.reset_pin = 127;
    c.reset_level = 1;
    c.reset_hold = 99;
    contract::config(&mut clock, &mut memory, &c);
    assert!(le32(&clock, 0) == 0xb000);
    assert!(clock[12..48].iter().all(|&b| b == 0));
    // §5.3: empty pin strings.
    assert!(clock[48] == 127 && clock[49] == 1 && clock[50] == 99 && clock[51] == 0);
    assert!(le32(&memory, 52) == 0);
    assert!(memory.iter().enumerate().all(|(i, &b)| i == 12 || b == 0));
    assert!(le32(&clock, 28) == 0);
    assert!(le32(&clock, 0) & 0x0f00 == 0);
}

/// The request in what is retained, which is from its eighth byte.
fn request_in(retained: &mut [u8; 224]) -> &mut Request {
    (&mut retained[8..88]).try_into().unwrap()
}

fn make_request(request: &mut Request) {
    *request = [0; 80];
    put_le16(request, 2, 0x5a0d);
    put_le16(request, 4, 0x5a01);
    put_le16(request, 6, 0x5441);
    request[8..17].copy_from_slice(b"ota-test\0");
    for i in 0..32 {
        request[40 + i] = (i * 7 + 1) as u8;
    }
    put_le32(request, 72, 0xb9000);
    let crc = crc16(0, &request[2..]);
    put_le16(request, 0, crc);
    assert!(request_valid(request));
}

fn test_updater() {
    let mut app = App::EMPTY;
    let (mut out, mut retained) = ([0; 24], [0; 224]);
    app.base = 0x4000;
    app.entry = 0x0200_0120;
    make_request(request_in(&mut retained));
    let saved = *request_in(&mut retained);
    contract::updater(&mut out, &app, 0x980f, 0x01c0_6000, 0x01c7_fdb0);
    let expected = [
        0x20, 1, 0, 2, 0, 0x40, 0, 0, 0, 0x60, 0xc0, 1, 0x0f, 0x98, 0, 0, 0, 0, 0, 0, 0xb0, 0xfd,
        0xc7, 1,
    ];
    assert!(out == expected);
    assert!(*request_in(&mut retained) == saved);
    app.base = 0;
    app.entry = 0;
    contract::updater(&mut out, &app, 0x980f, 0x01c0_6000, 0x01c0_6100);
    assert!(le32(&out, 0) == 0 && le32(&out, 4) == 0);
    assert!(le32(&out, 20) == 0x01c0_6100);
    assert!(out[14..20].iter().all(|&b| b == 0));
}

fn test_retained() {
    let mut retained = [0x81; 224];
    make_request(request_in(&mut retained));
    retained[..8].copy_from_slice(b"BOOTANY\0");
    let mut original = retained;
    assert!(contract::consume_bypass(
        retained.first_chunk_mut().unwrap()
    ));
    assert!(!contract::consume_bypass(
        retained.first_chunk_mut().unwrap()
    ));
    assert!(retained[..8].iter().all(|&b| b == 0));
    assert!(retained[8..] == original[8..]);
    assert!(request_valid(request_in(&mut retained)));
    assert!(contract::update_choice(request_in(&mut retained), true) == 1);
    assert!(contract::update_choice(request_in(&mut retained), false) == 1);
    retained[80] ^= 1;
    assert!(contract::update_choice(request_in(&mut retained), true) == 0);
    assert!(contract::update_choice(request_in(&mut retained), false) == 2);
    retained[..8].copy_from_slice(b"BOOTANY!");
    original = retained;
    assert!(!contract::consume_bypass(retained.first_chunk_mut().unwrap()) && retained == original);
    // §6.2 applies only after no request and a successful application mount.
    retained = [0; 224];
    retained[..11].copy_from_slice(b"UARTUPDATE\0");
    assert!(uart_request(retained.first_chunk().unwrap()));
    let choice = contract::update_choice(request_in(&mut retained), true);
    assert!(choice == 0 && uart_request(retained.first_chunk().unwrap()));
    assert!(contract::update_choice(request_in(&mut retained), false) == 2);
    retained[..4].fill(0);
    assert!(!uart_request(retained.first_chunk().unwrap()));
    assert!(retained[4..11] == *b"UPDATE\0");
    retained = [0; 224];
    retained[..19].copy_from_slice(b"UART_UPDATE_CUSTOM\0");
    assert!(uart_request(retained.first_chunk().unwrap()));
    retained[18] = b'x';
    assert!(!uart_request(retained.first_chunk().unwrap()));
    retained[18] = 0;
    retained[..4].fill(0);
    assert!(!uart_request(retained.first_chunk().unwrap()));
    make_request(request_in(&mut retained));
    assert!(contract::update_choice(request_in(&mut retained), true) == 1);
    assert!(contract::update_choice(request_in(&mut retained), false) == 1);
}

#[test]
fn contract() {
    test_app();
    test_config();
    test_updater();
    test_retained();
    println!(
        "contract: app/updater/config byte vectors, original private pointer, one-shot and §6.2 priority passed"
    );
}
