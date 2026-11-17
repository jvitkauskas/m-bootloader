/* SPDX-License-Identifier: MIT */
/* FM-1 Big Screen in MIDI mode: while an app asks (F0 7D 'F' 'S' 00/01 F7, about once a second), what's drawn on the
 * panel also goes out over USB MIDI, so FM-1 Mirror (github.com/pfkellogg/fm1-mirror) can show it big. src/mirror.c. */
#ifndef MB_MIRROR_H
#define MB_MIRROR_H
#include <stdint.h>
#if defined(STAGE1) || defined(RAM_STAGE)
#define MIRROR 1
void mirror_hello(uint8_t all);                              /* from the frame parser */
void mirror_tick(void);                                      /* every wait (mu_idle, about a millisecond) */
void mirror_poll(void);                                      /* MIDI mode's loop: the whole screen for a new viewer */
void mirror_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
void mirror_row_begin(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n);
void mirror_cell(unsigned c, const uint8_t *pixels);         /* one 12x16 cell of that row, RGB565 big-endian */
void mirror_row_end(void);
#endif
#endif
