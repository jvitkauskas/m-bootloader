// SPDX-License-Identifier: MIT
//! The installer as a package carries it (docs/SPEC.md §6.3): packed like stage 1 (tools/pack_stage1.py), behind this
//! unpacker. The bootloader, stock or ours, loads the image to 0x01C0A800 and calls its first byte, which comes here
//! with r0 (the context) and r1 (the stream's end) as it got them. The pack ends past where the installer will, so
//! unpacking it in place, at 0x01C0A800, never writes over what is still to be read; this code lies past both. Then
//! the installer is called as the bootloader called this. It never returns. If the pack is damaged, this returns to
//! the bootloader, having written only RAM.
use crate::Global;
use crate::upkr::{PROBS, unpack_stage1};
use core::slice::from_raw_parts;

const INSTALLER: usize = 0x01c0_a800;

unsafe extern "C" {
    static sfx_pack: u8;
    static sfx_pack_end: u8;
}

#[unsafe(no_mangle)]
pub extern "C" fn sfx_main(context: u32, end: u32) {
    static SCRATCH: Global<[u8; PROBS]> = Global::new([0; PROBS]);
    let out = INSTALLER as *mut u8;
    // Safety: the image's own RAM, laid out as tools/sfx_layout.c checked; and this is entered once.
    unsafe {
        let (pack, pack_end) = (&raw const sfx_pack, &raw const sfx_pack_end);
        let (room, pack) = (from_raw_parts(out.cast(), pack_end.offset_from_unsigned(out)), from_raw_parts(pack.cast(), pack_end.offset_from_unsigned(pack)));
        if unpack_stage1(room, pack, SCRATCH.get()) {
            core::mem::transmute::<usize, extern "C" fn(u32, u32)>(INSTALLER)(context, end);
        }
    }
}
