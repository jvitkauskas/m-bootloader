/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Patrick F. Kellogg
 * FM-1 Big Screen in MIDI mode, for players who can't read the 1.54-inch screen: while an app asks with
 * F0 7D 'F' 'S' 00 F7 about once a second (01: and the whole screen again), every rectangle and text row drawn on the
 * panel is also sent as F0 7D 'F' 'S' 01 row(2 bytes) col(2 bytes) <runs> F7, each run a count (1-127) and the
 * RGB565 colour in three 7-bit bytes. Three seconds after the last hello it stops; with nobody asking it sends
 * nothing, and a host that stops reading stops it at once (the next hello starts it again, with the whole screen).
 * The protocol FM-1 Mirror and the Big Screen builds of other FM-1 firmwares use. */
#include "mirror.h"
#include "display.h"
#include "midi_update.h"
#include "usb_midi.h"
#define ROWS 15u
#define COLS 20u
#define RUNS 16u                                   /* runs per message: 10 + 4*16 + 1 = 75 bytes */
#define QUIET_MS 3000u
#define RECTS 16u                                  /* rectangles kept for a new viewer: the bars on screen now */

static char text[ROWS][COLS];
static uint16_t fgs[ROWS], bgs[ROWS];
static uint8_t drawn[ROWS];
static uint8_t glyph[COLS];                        /* the row being sent: each cell's glyph (display_glyph) */
static unsigned cur_row;
static uint8_t on, asked, full, replaying;
static uint32_t heard;                             /* usb_midi_polls() at the last hello taken */
static uint8_t msg[10+4*RUNS+1];

/* What's on the panel since the last full clear, in the order it was drawn: the text rows, and the rectangles
 * (progress bars and their frames) that aren't hidden under something drawn later. A new viewer gets them in the
 * same order, so the panel, drawn again, doesn't change. */
struct rect { uint8_t x, y, w, h; uint16_t color; };
static struct rect rects[RECTS];
static uint32_t rect_seq[RECTS], row_seq[ROWS], seq;
static unsigned nrects;

/* One line of pixels, y, from x to x+w: colour(i) for each, sent as run-length messages. A host that stops reading
 * (a send that doesn't go) turns the mirror off: no more waits until it asks again. */
static void line(unsigned y, unsigned x, unsigned w, uint16_t (*colour)(unsigned)) {
    unsigned i = 0;
    while (on && i < w) {
        if (usb_midi_polls()-heard > 2*QUIET_MS) { /* 3 s since the last hello, time inside sends included */
            on = full = 0;
            break;
        }
        unsigned k = 9, runs = 0;
        msg[0] = 0xf0; msg[1] = 0x7d; msg[2] = 'F'; msg[3] = 'S'; msg[4] = 1;
        msg[5] = y&0x7f; msg[6] = (uint8_t)(y >> 7); msg[7] = (x+i)&0x7f; msg[8] = (uint8_t)((x+i) >> 7);
        while (i < w && runs < RUNS) {
            uint16_t c = colour(i);
            unsigned n = 1;
            while (i+n < w && n < 127 && colour(i+n) == c)
                ++n;
            msg[k++] = (uint8_t)n; msg[k++] = c&0x7f; msg[k++] = (c >> 7)&0x7f; msg[k++] = (uint8_t)(c >> 14);
            i += n; ++runs;
        }
        msg[k++] = 0xf7;
        if (!mu_send(msg, k))
            on = full = 0;
    }
}

static uint16_t solid;
static uint16_t solid_at(unsigned i) { (void)i; return solid; }
static unsigned text_y;
static uint16_t text_at(unsigned i) {
    return display_lit(glyph[i/12], i%12, text_y) ? fgs[cur_row]:bgs[cur_row];
}

/* Forgets the kept rectangles that lie wholly inside x..x+w, y..y+h: something drawn over them hides them. */
static void cover(unsigned x, unsigned y, unsigned w, unsigned h) {
    unsigned k = 0;
    for (unsigned i = 0; i < nrects; ++i) {
        const struct rect *r = &rects[i];
        if (r->x >= x && r->y >= y && r->x+r->w <= x+w && r->y+r->h <= y+h)
            continue;
        rects[k] = rects[i]; rect_seq[k++] = rect_seq[i];
    }
    nrects = k;
}

void mirror_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (!replaying) {                              /* the panel changed, watched or not */
        if (x == 0 && y == 0 && w == 240 && h == 240) {
            for (unsigned r = 0; r < ROWS; ++r)
                drawn[r] = 0;
            nrects = 0;
        } else if (w && h) {
            cover(x, y, w, h);
            if (nrects == RECTS) {                 /* full: the oldest goes */
                for (unsigned i = 1; i < RECTS; ++i) {
                    rects[i-1] = rects[i]; rect_seq[i-1] = rect_seq[i];
                }
                --nrects;
            }
            rects[nrects] = (struct rect){ (uint8_t)x, (uint8_t)y, (uint8_t)w, (uint8_t)h, color };
            rect_seq[nrects++] = ++seq;
        }
    }
    solid = color;
    for (unsigned r = 0; on && r < h; ++r)
        line(y+r, x, w, solid_at);
}

void mirror_row_begin(unsigned row, uint16_t fg, uint16_t bg, const char *t, unsigned n) {
    cur_row = row;
    for (unsigned c = 0; c < COLS; ++c)
        text[row][c] = c < n ? t[c]:' ';
    fgs[row] = fg; bgs[row] = bg; drawn[row] = 1;
    if (!replaying) {
        row_seq[row] = ++seq;
        cover(0, row*16, 240, 16);
    }
}

/* The row just drawn, line by line, from its text and the panel's own font: the same pixels. */
void mirror_row_end(void) {
    if (!on)
        return;
    for (unsigned c = 0; c < COLS; ++c)
        glyph[c] = (uint8_t)display_glyph(text[cur_row][c]);
    for (text_y = 0; on && text_y < 16; ++text_y)
        line(cur_row*16+text_y, 0, 240, text_at);
}

void mirror_hello(uint8_t all) {
    asked = 1;
    if (all == 1)
        full = 1;
}

/* Every wait in MIDI mode, installs included: a hello keeps the mirror on, and three seconds without one (by the
 * USB poll clock, which counts time inside sends too) stop it. */
void mirror_tick(void) {
    if (asked) {
        if (!on)
            full = 1;
        on = 1; asked = 0; heard = usb_midi_polls();
    } else if (on && usb_midi_polls()-heard > 2*QUIET_MS)
        on = 0;
}

/* MIDI mode's loop: a new viewer (or one asking again) gets the whole screen: black, then the kept rows and
 * rectangles drawn again in their order (the same pixels, so the panel doesn't change). */
void mirror_poll(void) {
    if (!on || !full)
        return;
    full = 0; replaying = 1;
    solid = 0;
    for (unsigned y = 0; on && y < 240; ++y)
        line(y, 0, 240, solid_at);
    for (uint32_t last = 0; on; ) {
        uint32_t next = UINT32_MAX;
        unsigned row = ROWS, rect = RECTS;
        for (unsigned r = 0; r < ROWS; ++r)
            if (drawn[r] && row_seq[r] > last && row_seq[r] < next) {
                next = row_seq[r]; row = r;
            }
        for (unsigned i = 0; i < nrects; ++i)
            if (rect_seq[i] > last && rect_seq[i] < next) {
                next = rect_seq[i]; rect = i; row = ROWS;
            }
        if (next == UINT32_MAX)
            break;
        last = next;
        if (row < ROWS) {
            char copy[COLS];
            for (unsigned c = 0; c < COLS; ++c)
                copy[c] = text[row][c];
            display_row(row, fgs[row], bgs[row], copy, COLS);
        } else {
            const struct rect *r = &rects[rect];
            display_rect(r->x, r->y, r->w, r->h, r->color);
        }
    }
    replaying = 0;
}
