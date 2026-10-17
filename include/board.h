/* SPDX-License-Identifier: MIT */
#ifndef MB_BOARD_H
#define MB_BOARD_H
#include <stdbool.h>
#include <stdint.h>
#define KEY_PREV 1u
#define KEY_NEXT 2u
#define KEY_SELECT 4u
#define KEY_RECOVERY 8u
void board_init(bool smk);
unsigned board_keys(void);
bool board_hold(unsigned key, unsigned ms);
void board_wait(unsigned ms, bool leds);
void board_restore(void);
#endif
