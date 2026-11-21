// SPDX-License-Identifier: MIT
//! Clock, SPI flash, pins and delays: still src/platform.c, declared here for the Rust that calls it.
unsafe extern "C" {
    pub safe fn gpio_output(port: u32, pin: u32, high: bool);
    pub safe fn gpio_level(port: u32, pin: u32, high: bool);
    pub safe fn gpio_input(port: u32, pin: u32, up: bool, down: bool);
    pub safe fn gpio_read(port: u32, pin: u32) -> bool;
    pub safe fn hal_delay_us(usec: u32) -> bool;
}
