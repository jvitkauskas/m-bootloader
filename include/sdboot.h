/* SPDX-License-Identifier: MIT */
#ifndef MB_SDBOOT_H
#define MB_SDBOOT_H
#define SD_STAGE2_ADDRESS 0x01c28000u
#define SD_STAGE2_MAX 0x8000u
#include <stdbool.h>
/* Whether a card answers (there is no card-detect line). */
bool sd_probe(unsigned lsb_mhz);
/* Loads and calls /MBOOT/MENU.BIN; returns 0 when stage 2 returned, else what failed (for the menu). */
const char *sd_stage2(unsigned lsb_mhz);
#endif
