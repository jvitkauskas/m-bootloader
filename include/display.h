/* SPDX-License-Identifier: MIT */
#ifndef MB_DISPLAY_H
#define MB_DISPLAY_H
#include <stdint.h>
#include <stdbool.h>
bool display_init(unsigned board);
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
/* w x h pixels from data, RGB565 big-endian, row by row. For RAM apps: the bootloader doesn't use it. */
void display_pixels(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *data);
void display_row(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n);
void display_text(unsigned row, uint16_t fg, const char *text);
void display_progress(unsigned percent, uint16_t color);
void display_release(void);
#endif
