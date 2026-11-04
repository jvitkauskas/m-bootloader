/* SPDX-License-Identifier: MIT */
/* Everything up to holding the recovery key for download mode: docs/SPEC.md §4.4.
 * Linked on its own, so it cannot depend on stage 1, the flash, the screen or the firmware. */
#include "stage0.h"
#include "board.h"
#include "platform.h"
#include <string.h>
#ifdef BOARD_SMK
#define SMK true
#else
#define SMK false
#endif
#ifdef BOOT_SIM
#define stage1_entry boot_main
#else
extern uint8_t __stage0_bss_start[], __stage0_bss_end[];
void stage1_entry(const uint32_t *device);
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
    stage1_entry(device);
}
