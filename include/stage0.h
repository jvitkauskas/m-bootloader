/* SPDX-License-Identifier: MIT */
#ifndef MB_STAGE0_H
#define MB_STAGE0_H
#include <stdbool.h>
#include <stdint.h>
#define RETAINED ((uint8_t *)(uintptr_t)0x01c7fd80)
void stage0_main(const uint32_t *device);
bool apply_config(void);
void rom_download(void) __attribute__((noreturn));
void boot_main(const uint32_t *device);
#endif
