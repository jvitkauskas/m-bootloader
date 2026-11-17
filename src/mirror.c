/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Patrick F. Kellogg
 * FM-1 Big Screen in MIDI mode, for players who can't read the 1.54-inch screen: while an app asks with
 * F0 7D 'F' 'S' 00 F7 about once a second (01: and the whole screen again), every rectangle and text row drawn on the
 * panel is also sent as F0 7D 'F' 'S' 01 row(2 bytes) col(2 bytes) <runs> F7, each run a count (1-127) and the
 * RGB565 colour in three 7-bit bytes. Three seconds after the last hello it stops; with nobody asking it sends
 * nothing. The protocol FM-1 Mirror and the Big Screen builds of other FM-1 firmwares use. */
#include "mirror.h"
#include "midi_update.h"
#define ROWS 15u
#define COLS 20u
#define RUNS 16u                                   /* runs per message: 10 + 4*16 + 1 = 75 bytes */
#define QUIET_MS 3000u

static char text[ROWS][COLS];
static uint16_t fgs[ROWS], bgs[ROWS];
static uint8_t drawn[ROWS];
static uint8_t mask[16][COLS*12/8];                /* the row being drawn: 1 where a cell pixel is the text colour */
static unsigned cur_row;
static uint8_t on, asked, full, replaying;
static uint32_t quiet;
static uint8_t msg[10+4*RUNS+1];

/* One line of pixels, y, from x to x+w: colour(i) for each, sent as run-length messages. */
static void line(unsigned y, unsigned x, unsigned w, uint16_t (*colour)(unsigned)) {
    unsigned i = 0;
    while (i < w) {
        unsigned k = 10, runs = 0;
        msg[0] = 0xf0; msg[1] = 0x7d; msg[2] = 'F'; msg[3] = 'S'; msg[4] = 1;
        msg[5] = y&0x7f; msg[6] = (uint8_t)(y >> 7); msg[7] = (x+i)&0x7f; msg[8] = (uint8_t)((x+i) >> 7); msg[9] = 0;
        k = 9;
        while (i < w && runs < RUNS) {
            uint16_t c = colour(i);
            unsigned n = 1;
            while (i+n < w && n < 127 && colour(i+n) == c)
                ++n;
            msg[k++] = (uint8_t)n; msg[k++] = c&0x7f; msg[k++] = (c >> 7)&0x7f; msg[k++] = (uint8_t)(c >> 14);
            i += n; ++runs;
        }
        msg[k++] = 0xf7;
        mu_send(msg, k);
    }
}

static uint16_t solid;
static uint16_t solid_at(unsigned i) { (void)i; return solid; }
static unsigned mask_y;
static uint16_t text_at(unsigned i) {
    return mask[mask_y][i >> 3]&(1u << (i&7)) ? fgs[cur_row]:bgs[cur_row];
}

void mirror_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (x == 0 && y == 0 && w == 240 && h == 240 && !replaying)
        for (unsigned r = 0; r < ROWS; ++r)
            drawn[r] = 0;                          /* the panel was cleared, watched or not */
    if (!on)
        return;
    solid = color;
    for (unsigned r = 0; r < h; ++r)
        line(y+r, x, w, solid_at);
}

void mirror_row_begin(unsigned row, uint16_t fg, uint16_t bg, const char *t, unsigned n) {
    cur_row = row;
    for (unsigned c = 0; c < COLS; ++c)
        text[row][c] = c < n ? t[c]:' ';
    fgs[row] = fg; bgs[row] = bg; drawn[row] = 1;
}

void mirror_cell(unsigned c, const uint8_t *px) {
    if (!on)
        return;
    uint16_t fg = fgs[cur_row];
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 12; ++x) {
            unsigned at = 2*(y*12+x), i = c*12+x;
            if ((uint16_t)(px[at] << 8 | px[at+1]) == fg)
                mask[y][i >> 3] |= (uint8_t)(1u << (i&7));
            else
                mask[y][i >> 3] &= (uint8_t)~(1u << (i&7));
        }
}

void mirror_row_end(void) {
    if (!on)
        return;
    for (mask_y = 0; mask_y < 16; ++mask_y)
        line(cur_row*16+mask_y, 0, 240, text_at);
}

void mirror_hello(uint8_t all) {
    asked = 1;
    if (all == 1)
        full = 1;
}

void display_row(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n);

/* Every wait in MIDI mode, about a millisecond each, installs included: a hello keeps the mirror on, and three
 * seconds without one stop it. */
void mirror_tick(void) {
    if (asked) {
        if (!on)
            full = 1;
        on = 1; asked = 0; quiet = 0;
    } else if (on && ++quiet > QUIET_MS)
        on = 0;
}

/* MIDI mode's loop: a new viewer (or one asking again) gets the whole screen, from the text kept here */
void mirror_poll(void) {
    if (on && full) {
        full = 0; replaying = 1;
        mirror_rect(0, 0, 240, 240, 0);
        for (unsigned r = 0; r < ROWS; ++r)
            if (drawn[r]) {
                char copy[COLS];
                for (unsigned c = 0; c < COLS; ++c)
                    copy[c] = text[r][c];
                display_row(r, fgs[r], bgs[r], copy, COLS);   /* the same text again: the panel doesn't change */
            }
        replaying = 0;
    }
}
