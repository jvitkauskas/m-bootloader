/* SPDX-License-Identifier: MIT */
/* src/mirror.c offline: the MIDI transport and the panel are stubs. Covers a quiet mirror with nobody asking, the
 * first hello, asking again, the three-second expiry, a host that stops reading, and a new viewer getting the
 * progress bars back in the order they were drawn. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "mirror.h"
#include "midi_update.h"

/* The transport: counts what goes out, and can refuse (a host that stopped reading). */
static unsigned sent, attempts, refuse_after = ~0u;
static unsigned first_y[4096], first_colour[4096];
bool mu_send(const uint8_t *data, uint32_t size) {
    assert(size >= 14 && data[0] == 0xf0 && data[1] == 0x7d && data[2] == 'F' && data[3] == 'S' && data[4] == 1);
    assert(data[size-1] == 0xf7 && (size-10) % 4 == 0 && data[9] >= 1 && data[9] <= 127);
    ++attempts;
    if (sent >= refuse_after)
        return false;
    if (sent < 4096) {
        first_y[sent] = data[5] | data[6] << 7;
        first_colour[sent] = data[10] | data[11] << 7 | data[12] << 14;
    }
    ++sent;
    return true;
}

/* The panel, as src/display.c calls the mirror (MIRROR). */
static unsigned panel_rects, panel_rows;
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    ++panel_rects;
    mirror_rect(x, y, w, h, color);
}
void display_row(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n) {
    ++panel_rows;
    mirror_row_begin(row, fg, bg, text, n);
    mirror_row_end();
}
/* A stand-in font: a space is blank, anything else lights every third pixel. */
unsigned display_glyph(char c) { return c != ' '; }
bool display_lit(unsigned g, unsigned x, unsigned y) { return g && (x+y) % 3 == 0; }

static void wait_ms(unsigned ms) {
    while (ms--)
        mirror_tick();
}

/* What a hello and a wait do, as MIDI mode's loop (src/midi_update.c mu_run) does them. */
static void hello(uint8_t all) {
    mirror_hello(all);
    wait_ms(1);
    mirror_poll();
}

int main(void) {
    /* Nobody asking: drawing sends nothing. */
    display_row(2, 0xffff, 0, "MIDI MODE", 9);
    display_rect(10, 96, 220, 12, 0xffff);
    assert(sent == 0);

    /* A full clear while nobody is watching forgets what was kept. */
    display_rect(0, 0, 240, 240, 0);
    display_row(3, 0xffff, 0, "WAITING", 7);
    assert(sent == 0);

    /* The first hello: the whole screen, black first, then the one row kept (16 lines), the panel drawn again. */
    unsigned rows_before = panel_rows;
    hello(0);
    assert(sent >= 240+16);
    assert(first_y[0] == 0 && first_colour[0] == 0);
    assert(panel_rows == rows_before+1);           /* only row 3: row 2 was cleared away */
    unsigned after_first = sent;

    /* Watching: a progress bar's rectangles go out as they're drawn. */
    display_progress(0, 0x07e0);
    display_progress(40, 0x07e0);
    assert(sent > after_first);

    /* Asking again (01): the screen again, with the bar in the order it was drawn: its last fill is green. */
    unsigned before = sent, rects_before = panel_rects;
    hello(1);
    assert(sent > before+240);
    assert(panel_rects > rects_before);            /* the bar's rectangles were drawn again */
    unsigned last_green = 0;
    for (unsigned i = before; i < sent && i < 4096; ++i)
        if (first_colour[i] == 0x07e0)
            last_green = i;
    assert(last_green > before+240);               /* after the black screen, not lost under it */

    /* A hello every second keeps it on; three seconds without one stop it. */
    wait_ms(900); hello(0); wait_ms(2900);
    before = sent;
    display_row(5, 0xffff, 0, "STILL ON", 8);
    assert(sent > before);
    wait_ms(200);
    before = sent;
    display_row(5, 0xffff, 0, "NOW OFF", 7);
    assert(sent == before);

    /* A host that asks, then stops reading: the first send that doesn't go stops the replay at once. */
    refuse_after = sent+3;
    before = sent;
    unsigned tried = attempts;
    hello(1);
    assert(sent == before+3 && attempts == tried+4);   /* three went, the fourth didn't, and nothing more was tried */
    display_row(6, 0xffff, 0, "QUIET", 5);
    assert(sent == before+3 && attempts == tried+4);

    /* It reads again and asks: the whole screen again. */
    refuse_after = ~0u;
    hello(0);
    assert(sent > before+3+240);

    /* Many bars: the kept list stays bounded, and a row drawn over a bar hides it. */
    for (unsigned p = 0; p <= 100; ++p)
        display_progress(p, 0x07e0);
    display_row(9, 0xffff, 0, "DONE", 4);
    display_row(10, 0xffff, 0, "", 0);
    before = sent; rects_before = panel_rects;
    hello(1);
    assert(panel_rects-rects_before <= 16);
    printf("mirror test: %u messages, all checks passed\n", sent);
    return 0;
}

/* display_progress, as src/display.c draws it, through the stub display_rect above. */
void display_progress(unsigned percent, uint16_t color) {
    if (percent > 100)
        percent = 100;
    display_rect(10, 148, 220, 2, 0xffff);
    display_rect(10, 172, 220, 2, 0xffff);
    display_rect(10, 150, 2, 22, 0xffff);
    display_rect(228, 150, 2, 22, 0xffff);
    display_rect(12, 150, 216, 22, 0);
    display_rect(12, 152, 216*percent/100, 18, color);
}
