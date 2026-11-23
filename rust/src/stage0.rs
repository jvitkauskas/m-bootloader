// SPDX-License-Identifier: MIT
//! Stage 0: everything up to holding the recovery key for download mode (docs/SPEC.md §4.4). It is linked on its
//! own, so it cannot depend on stage 1, the flash, the screen or the firmware.

#[cfg(stage1)]
unsafe extern "C" {
    /// Restarts the chip in its ROM's USB download mode (§4.2).
    pub safe fn rom_download() -> !;
}

#[cfg(any(stage0, boot_sim))]
pub use own::*;

#[cfg(any(stage0, boot_sim))]
mod own {
    use crate::board::{KEY_RECOVERY, board_hold, board_init};
    use crate::hw::{Reg, Retained, sync_cpu};
    use crate::platform::{hal_clock_a, hal_early, hal_fault, hal_reset, p33_read, p33_write};

    const SMK: bool = cfg!(smk);
    /// Restarts the chip in its ROM's USB download mode (§4.2).
    #[unsafe(no_mangle)]
    pub extern "C" fn rom_download() -> ! {
        // Safety: whoever calls this holds no reference into the retained area, and the ROM reads it after the reset.
        let mut retained = unsafe { Retained::claim() };
        *retained.bytes::<0, _>() = *b"usb_update_mode\0";
        sync_cpu();
        hal_reset()
    }

    /// Power, and the clock (§3.1, §3.2).
    #[unsafe(no_mangle)]
    #[inline(never)]
    pub extern "C" fn apply_config() -> bool {
        p33_write(9, p33_read(9) & 0xf0 | 11);
        p33_write(6, p33_read(6) & 0xf8 | 3);
        sync_cpu();
        sync_cpu();
        for n in 0..16 {
            p33_write(0xd0 + n, 0);
        }
        for n in 0..3 {
            p33_write(0xad + n, 0);
        }
        sync_cpu();
        sync_cpu();
        if hal_fault() {
            return false;
        }
        hal_clock_a()
    }

    /// Stage 1, where it runs: the simulated chip's tests have it there already, and unpacked.
    #[cfg(boot_sim)]
    use crate::boot::boot_main as stage1_entry;

    #[cfg(not(boot_sim))]
    unsafe extern "C" {
        /// The first byte after stage 0, where stage 1 runs: until it is unpacked, its pack.
        fn stage1_entry(device: *const u32);
        static mut __stage0_bss_start: u8;
        static __stage0_bss_end: u8;
        static __stage0_limit: u8;
    }

    /// The pack lies where stage 1 runs, so it moves out of the way first.
    #[cfg(not(boot_sim))]
    fn unpack_stage1() -> bool {
        use crate::upkr::{HEADER, PROBS, unpack_stage1};
        use core::slice::from_raw_parts;
        /// Free RAM above the bootloader's, until a staged updater is loaded there.
        const SCRATCH: usize = 0x01c0_a800;
        const SCRATCH_PACK: usize = 0x3f2c;
        // (made here, not as a constant: a pointer to memory that Rust did not make has to be made when it runs)
        let scratch: *mut u8 = core::ptr::with_exposed_provenance_mut(SCRATCH);
        let to = stage1_entry as *mut u8;
        // Safety: the pack is in the image the ROM loaded, the scratch RAM is free, and stage 1 has its room up to
        // stage 0's data.
        unsafe {
            let size = HEADER + (*to as usize | (*to.add(1) as usize) << 8);
            if size > SCRATCH_PACK {
                return false;
            }
            core::ptr::copy_nonoverlapping(to, scratch, size);
            let room = crate::hw::distance(to, &raw const __stage0_limit);
            unpack_stage1(
                from_raw_parts(to.cast(), room),
                from_raw_parts(scratch.cast(), size),
                &mut *scratch.add(SCRATCH_PACK).cast::<[u8; PROBS]>(),
            )
        }
    }

    /// From the entry (src/startup.S), with the ROM's boot-device descriptor.
    #[unsafe(no_mangle)]
    pub extern "C" fn stage0_main(device: *const u32) {
        // Safety: stage 0's own data, which nothing has used yet.
        #[cfg(not(boot_sim))]
        unsafe {
            core::ptr::write_bytes(
                &raw mut __stage0_bss_start,
                0,
                crate::hw::distance(&raw const __stage0_bss_start, &raw const __stage0_bss_end),
            )
        };
        hal_early();
        const DMA_PRIORITY: Reg = Reg::at::<0x13004>();
        const ENC_KEY: Reg = Reg::at::<0x40304>();
        DMA_PRIORITY.write(0xffff);
        ENC_KEY.write(0xffff);
        if !apply_config() {
            rom_download();
        }
        board_init(SMK);
        if board_hold(KEY_RECOVERY, 1000) {
            rom_download();
        }
        #[cfg(not(boot_sim))]
        if !unpack_stage1() {
            rom_download();
        }
        // Safety: stage 1, just unpacked and checked.
        unsafe { stage1_entry(device) };
    }
}
