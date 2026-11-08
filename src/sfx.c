/* SPDX-License-Identifier: MIT */
/* The installer as a package carries it (docs/SPEC.md §6.3): packed like stage 1 (tools/pack_stage1.py), behind this
 * unpacker. The bootloader, stock or ours, loads the image to 0x01C0A800 and calls its first byte, which comes here
 * with r0 (the context) and r1 (the stream's end) as it got them. The pack ends past where the installer will, so
 * unpacking it in place, at 0x01C0A800, never writes over what is still to be read; this code lies past both. Then
 * the installer is called as the bootloader called this. It never returns. If the pack is damaged, this returns to
 * the bootloader, having written only RAM. */
#include "upkr.h"
#define INSTALLER 0x01c0a800u
extern uint8_t sfx_pack[], sfx_pack_end[];
static uint8_t probs[STAGE1_PROBS];

void sfx_main(uint32_t context, uint32_t end) {
    uint8_t *out = (uint8_t *)(uintptr_t)INSTALLER;
    if (stage1_unpack(out, (uint32_t)(sfx_pack_end-out), sfx_pack, (uint32_t)(sfx_pack_end-sfx_pack), probs))
        ((void (*)(uint32_t, uint32_t))(uintptr_t)INSTALLER)(context, end);
}
