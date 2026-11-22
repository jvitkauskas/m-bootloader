// SPDX-License-Identifier: MIT
//! For a test build's bootloader: a program for M-Flasher's run-ram that leaves a one-shot request in retained RAM
//! and resets (docs/SPEC.md §11.9).
use crate::hw::{Reg, Retained, bit, irq_off, sync_cpu};

const POWER: Reg = Reg::at::<0x10000>();
/// The request: the SD card item is 4, and MIDI mode is 5.
const REQUEST: u8 = if cfg!(test_request_sd) { 4 } else { 5 };

#[unsafe(no_mangle)]
#[unsafe(link_section = ".entry")]
pub extern "C" fn test_request_entry() -> ! {
    irq_off();
    // Safety: nothing else runs until the reset.
    *unsafe { Retained::claim() }.bytes::<0, _>() =
        [b'M', b'B', b'T', b'E', b'S', b'T', REQUEST, 0];
    sync_cpu();
    POWER.set(bit(4));
    sync_cpu();
    loop {}
}
