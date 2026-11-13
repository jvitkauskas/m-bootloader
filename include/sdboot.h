/* SPDX-License-Identifier: MIT */
#ifndef MB_SDBOOT_H
#define MB_SDBOOT_H
#define SD_STAGE2_ADDRESS 0x01c28000u
#define SD_STAGE2_MAX 0x8000u
#include <stdbool.h>
#include <stdint.h>
/* Whether a card answers (there is no card-detect line). */
bool sd_probe(unsigned lsb_mhz);
/* Loads and calls /MBOOT/MENU.BIN, using scratch for the card's sectors; returns 0 when stage 2 returned, else what
 * failed (for the menu). */
const char *sd_stage2(unsigned lsb_mhz, uint8_t scratch[512]);
/* USB DISK's access to the card, by 512-byte sector. Opening returns its size in sectors, or 0 if it can't be used,
 * and the screen can't be drawn until it's closed, which it has to be either way. */
uint32_t sd_disk_open(unsigned lsb_mhz);
bool sd_disk(uint32_t lba, uint8_t *p, bool write);
void sd_disk_close(void);
#endif
