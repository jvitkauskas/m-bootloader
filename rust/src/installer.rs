// SPDX-License-Identifier: MIT
//! Staged updater that installs the bootloader built with it: M-UPGRADE stages it from a .fwsc package and the
//! installed bootloader, stock or ours, runs it (§6.3). It rewrites only the bootloader and its boot-area record,
//! checks them, retires the update request, in RAM and in flash, and resets into the new bootloader.
use crate::display::{self, GREEN, RED, WHITE, YELLOW};
use crate::formats::{self, BOOT_AREA, Header};
use crate::hw::{Retained, Text, irq_off, sync_cpu};
use crate::platform::{
    self, flash_capacity, flash_erase, flash_init, flash_release, hal_delay_us, hal_early,
    hal_fault_clear, hal_reset, hal_timer_clock,
};
use crate::{Aligned, Global, text};

const BOARD: u32 = if cfg!(smk) { 2 } else { 1 };
const TITLE: Text = if cfg!(smk) {
    text!("SMK-37 M-BOOTLOADER")
} else {
    text!("FM-1 M-BOOTLOADER")
};
const SECTOR: usize = 4096;
type Sector = [u8; SECTOR];

unsafe extern "C" {
    // The bootloader this installs: src/installer_start.S.
    static installer_container: u8;
    static installer_container_end: u8;
    static mut __bss_start: u8;
    static __bss_end: u8;
}

static AREA: Global<Aligned<BOOT_AREA>> = Global::new(Aligned([0; BOOT_AREA]));
static CHECK: Global<Aligned<SECTOR>> = Global::new(Aligned([0; SECTOR]));

fn write_sector(address: u32, sector: &Sector, check: &mut Sector) -> bool {
    for _ in 0..3 {
        hal_fault_clear();
        if flash_erase(core::ptr::null_mut(), address, SECTOR as u32)
            && platform::program(address, sector)
            && platform::read(address, check)
            && check == sector
        {
            return true;
        }
    }
    false
}

/// What went wrong, or None once the boot area holds this bootloader.
fn install(screen: bool) -> Option<Text> {
    // Safety: used only here, which is entered once; and the container is as the linker placed it.
    let (area, check, container) = unsafe {
        let (start, end) = (
            &raw const installer_container,
            &raw const installer_container_end,
        );
        (
            &mut AREA.get().0,
            &mut CHECK.get().0,
            core::slice::from_raw_parts(start, crate::hw::distance(start, end)),
        )
    };
    if !flash_init(1, 1) || flash_capacity() != 0x10_0000 {
        return Some(text!("NO FLASH"));
    }
    if !platform::read(0, area) {
        return Some(text!("READ FAILED"));
    }
    if !formats::boot_install(area, container, flash_capacity()) {
        return Some(text!("BOOT AREA UNKNOWN"));
    }
    let mut changed = false;
    for (n, sector) in area.as_chunks().0.iter().enumerate() {
        let address = (n * SECTOR) as u32;
        if screen {
            display::progress(address * 100 / BOOT_AREA as u32, GREEN);
        }
        if !platform::read(address, check) {
            return Some(text!("READ FAILED"));
        }
        if check == sector {
            continue;
        }
        changed = true;
        if !write_sector(address, sector, check) {
            return Some(text!("WRITE FAILED"));
        }
    }
    if screen {
        display::progress(100, GREEN);
    }
    if changed {
        None
    } else {
        Some(text!("ALREADY INSTALLED"))
    }
}

/// The flash copy of the request is acted on whenever the app does not mount, by this bootloader as by the stock one:
/// retire it too, or a later broken app would have every start run this again (§6.2).
pub fn retire_flash_request(stream_end: u32) {
    let mut header = Header::EMPTY;
    hal_fault_clear();
    if formats::header_read(&platform::Chip, &mut header) {
        formats::request_retire(&platform::Chip, header.alignment, stream_end);
    }
}

/// From the entry (src/installer_start.S), as the bootloader called it.
#[unsafe(no_mangle)]
pub extern "C" fn installer_main(_context: *mut u8, stream_end: u32) -> ! {
    irq_off();
    // Safety: this image's own data, which nothing has used yet.
    unsafe {
        core::ptr::write_bytes(
            &raw mut __bss_start,
            0,
            crate::hw::distance(&raw const __bss_start, &raw const __bss_end),
        )
    };
    // Watchdog off, as §6.3 says it already is: never reset mid-write.
    hal_early();
    // The bootloader that started us has set the 60 MHz LSB clock (§3.2).
    hal_timer_clock(60);
    let screen = display::init(BOARD);
    if screen {
        for row in 0..15 {
            display::text(row, WHITE, text!(""));
        }
        display::text(1, GREEN, TITLE);
        display::text(3, YELLOW, text!("INSTALLING"));
        // Safety: the version, in the container's payload, which is NUL-terminated (src/startup.S).
        display::text(4, WHITE, unsafe {
            Text::at((&raw const installer_container) as usize + 0x60)
        });
        display::text(6, WHITE, text!("DO NOT SWITCH OFF"));
    }
    let failure = install(screen);
    // Never run again for this request, by this or the next bootloader.
    if flash_capacity() != 0 {
        retire_flash_request(stream_end);
    }
    // Safety: nothing else runs until the reset.
    let mut retained = unsafe { Retained::claim() };
    *retained.bytes::<8, 2>() = [0; 2];
    sync_cpu();
    if screen {
        display::text(6, WHITE, text!(""));
        display::text(
            3,
            if failure.is_some() { RED } else { GREEN },
            failure.unwrap_or(text!("INSTALLED")),
        );
        display::text(8, WHITE, text!("RESTARTING"));
    }
    hal_delay_us(if failure.is_some() {
        5_000_000
    } else {
        2_000_000
    });
    flash_release();
    hal_reset()
}
