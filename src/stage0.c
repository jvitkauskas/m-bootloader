/* SPDX-License-Identifier: MIT */
/* Everything up to holding the recovery key for download mode: docs/SPEC.md §4.4.
 * Linked on its own, so it cannot depend on stage 1, the flash, the screen or the firmware. */
#include "stage0.h"
#include "board.h"
#include "platform.h"
#include "upkr.h"
#include <string.h>
#ifdef BOARD_SMK
#define SMK true
#else
#define SMK false
#endif
#ifdef BOOT_SIM
#define stage1_entry boot_main
#else
extern uint8_t __stage0_bss_start[], __stage0_bss_end[], __stage0_limit[];
void stage1_entry(const uint32_t *device);
/* Free RAM above the bootloader's, until a staged updater is loaded there. */
#define SCRATCH ((uint8_t *)(uintptr_t)0x01c0a800)
#define SCRATCH_PACK 0x3f2cu
#endif

void rom_download(void) {
    memcpy(RETAINED, "usb_update_mode", 16);
    sync_cpu();
    hal_reset();
}

__attribute__((noinline)) bool apply_config(void) {
    p33_write(9, (p33_read(9)&0xf0) | 11);
    p33_write(6, (p33_read(6)&0xf8) | 3);
    if (!SMK)
        p33_write(5, (p33_read(5)&0xf8) | 4); /* 3.2 V, as FM-1 016's configuration */
    for (unsigned n = 0; n < 2; ++n)
        sync_cpu();
    for (unsigned n = 0; n < 16; ++n)
        p33_write(0xd0+n, 0);
    for (unsigned n = 0; n < 3; ++n)
        p33_write(0xad+n, 0);
    for (unsigned n = 0; n < 2; ++n)
        sync_cpu();
    if (hal_fault())
        return false;
    return hal_clock_a();
}

#ifndef BOOT_SIM
/* The pack lies where stage 1 runs, so it moves out of the way first. */
static bool unpack_stage1(void) {
    uint8_t *to = (uint8_t *)(uintptr_t)stage1_entry;
    uint32_t size = STAGE1_HEADER+(to[0] | (uint32_t)to[1] << 8);
    if (size > SCRATCH_PACK)
        return false;
    memcpy(SCRATCH, to, size);
    return stage1_unpack(to, (uint32_t)(__stage0_limit-to), SCRATCH, size, SCRATCH+SCRATCH_PACK);
}

#endif
void stage0_main(const uint32_t *device) {
#ifndef BOOT_SIM
    memset(__stage0_bss_start, 0, (size_t)(__stage0_bss_end-__stage0_bss_start));
#endif
    hal_early();
    REG(0x13004) = 0xffff;
    REG(0x40304) = 0xffff;
    if (!apply_config())
        rom_download();
    board_init(SMK);
    if (board_hold(KEY_RECOVERY, 1000))
        rom_download();
#ifndef BOOT_SIM
    if (!unpack_stage1())
        rom_download();
#endif
    stage1_entry(device);
}
