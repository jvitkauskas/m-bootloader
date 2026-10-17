/* SPDX-License-Identifier: MIT */
#include "display.h"
#include "platform.h"
#include <string.h>
static unsigned cs_port, cs_pin, dc_port, dc_pin;
static bool active;
static uint8_t pixels[384] __attribute__((aligned(4)));
/* docs/SPEC.md Appendix A, author-provided MIT glyphs; see NOTICE.md. */
static const char characters[] = " -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t glyphs[][5] = {
    {
        0, 0, 0, 0, 0
    }, {
        8, 8, 8, 8, 8
    }, {
        0, 0x60, 0x60, 0, 0
    }, {
        0x60, 0x10, 8, 4, 3
    }, {
        0, 0x36, 0x36, 0, 0
    },
    {
        0, 8, 0x14, 0x22, 0x41
    }, {
        0x41, 0x22, 0x14, 8, 0
    }, {
        8, 8, 0x3e, 8, 8
    }, {
        2, 1, 0x51, 9, 6
    },
    {
        0x3e, 0x51, 0x49, 0x45, 0x3e
    }, {
        0, 0x42, 0x7f, 0x40, 0
    }, {
        0x42, 0x61, 0x51, 0x49, 0x46
    },
    {
        0x21, 0x41, 0x45, 0x4b, 0x31
    }, {
        0x18, 0x14, 0x12, 0x7f, 0x10
    }, {
        0x27, 0x45, 0x45, 0x45, 0x39
    },
    {
        0x3c, 0x4a, 0x49, 0x49, 0x30
    }, {
        1, 0x71, 9, 5, 3
    }, {
        0x36, 0x49, 0x49, 0x49, 0x36
    }, {
        6, 0x49, 0x49, 0x29, 0x1e
    },
    {
        0x7e, 9, 9, 9, 0x7e
    }, {
        0x7f, 0x49, 0x49, 0x49, 0x36
    }, {
        0x3e, 0x41, 0x41, 0x41, 0x22
    },
    {
        0x7f, 0x41, 0x41, 0x41, 0x3e
    }, {
        0x7f, 0x49, 0x49, 0x49, 0x41
    }, {
        0x7f, 9, 9, 9, 1
    },
    {
        0x3e, 0x41, 0x49, 0x49, 0x7a
    }, {
        0x7f, 8, 8, 8, 0x7f
    }, {
        0, 0x41, 0x7f, 0x41, 0
    },
    {
        0x20, 0x40, 0x41, 0x3f, 1
    }, {
        0x7f, 8, 0x14, 0x22, 0x41
    }, {
        0x7f, 0x40, 0x40, 0x40, 0x40
    },
    {
        0x7f, 2, 0x0c, 2, 0x7f
    }, {
        0x7f, 4, 8, 0x10, 0x7f
    }, {
        0x3e, 0x41, 0x41, 0x41, 0x3e
    },
    {
        0x7f, 9, 9, 9, 6
    }, {
        0x3e, 0x41, 0x51, 0x21, 0x5e
    }, {
        0x7f, 9, 0x19, 0x29, 0x46
    },
    {
        0x46, 0x49, 0x49, 0x49, 0x31
    }, {
        1, 1, 0x7f, 1, 1
    }, {
        0x3f, 0x40, 0x40, 0x40, 0x3f
    },
    {
        0x1f, 0x20, 0x40, 0x20, 0x1f
    }, {
        0x3f, 0x40, 0x38, 0x40, 0x3f
    }, {
        0x63, 0x14, 8, 0x14, 0x63
    },
    {
        3, 4, 0x78, 4, 3
    }, {
        0x61, 0x51, 0x49, 0x45, 0x43
    }
};
static bool wait_panel(void) {
    sync_cpu();
    for (unsigned n = 1000000; n; --n)
        if (REG(0x11d00)&BIT(15)) {
            REG(0x11d00) |= BIT(14);
            return true;
        }
    active = false;
    return false;
}

static void byte(uint8_t b) {
    if (active) {
        REG(0x11d08) = b;
        wait_panel();
    }
}

static void burst(unsigned command, const uint8_t *data, unsigned n) {
    if (!active)
        return;
    gpio_level(cs_port, cs_pin, false);
    gpio_level(dc_port, dc_pin, false);
    byte(command);
    gpio_level(dc_port, dc_pin, true);
    while (n--)
        byte(*data++);
    gpio_level(cs_port, cs_pin, true);
}

static bool delay(unsigned ms) {
    bool prior = hal_fault(), ok = hal_delay_us(ms*1000);
    if (!prior && !ok)
        hal_fault_clear();
    if (!ok)
        active = false;
    return ok;
}

static void window(unsigned x, unsigned y, unsigned w, unsigned h) {
    uint8_t a[4] = {
        (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x+w-1) >> 8), (uint8_t)(x+w-1)
    };
    burst(0x2a, a, 4);
    a[0] = y >> 8;
    a[1] = y;
    a[2] = (y+h-1) >> 8;
    a[3] = y+h-1;
    burst(0x2b, a, 4);
    gpio_level(cs_port, cs_pin, false);
    gpio_level(dc_port, dc_pin, false);
    byte(0x2c);
    gpio_level(dc_port, dc_pin, true);
}

static void dma(unsigned n) {
    if (!active || !n)
        return;
    REG(0x11d00) |= BIT(14);
    REG(0x11d0c) = DMA_ADDRESS(pixels);
    sync_cpu();
    REG(0x11d10) = n;
    wait_panel();
}

void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (!active || !w || !h || x >= 240 || y >= 240 || w > 240-x || h > 240-y)
        return;
    for (unsigned i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = color >> 8;
        pixels[i+1] = color;
    }
    window(x, y, w, h);
    unsigned n = w*h*2;
    while (n && active) {
        unsigned k = n > sizeof(pixels) ? sizeof(pixels):n;
        dma(k);
        n -= k;
    }
    gpio_level(cs_port, cs_pin, true);
}

bool display_init(unsigned board) {
    if (board != 1 && board != 2)
        return false;
    active = true;
    if (board == 1) {
        REG(0x51020) |= BIT(4);
        cs_port = 2;
        cs_pin = 7;
        dc_port = 2;
        dc_pin = 8;
        gpio_output(2, 9, false);
        gpio_output(2, 10, false);
        gpio_output(0, 2, true);
    } else {
        REG(0x51020) &= ~BIT(4);
        cs_port = 2;
        cs_pin = 8;
        dc_port = 1;
        dc_pin = 5;
        gpio_output(1, 6, false);
        gpio_output(1, 7, false);
    }
    gpio_output(cs_port, cs_pin, true);
    gpio_output(dc_port, dc_pin, false);
    REG(0x11d00) = 0x4021;
    REG(0x11d04) = 4;
    if (board == 1) {
        burst(1, 0, 0);
        delay(150);
        burst(0x11, 0, 0);
        delay(120);
        const uint8_t format = 0x55, orientation = 0;
        burst(0x3a, &format, 1);
        burst(0x36, &orientation, 1);
        burst(0x21, 0, 0);
        burst(0x13, 0, 0);
    } else {
        delay(100);
        burst(0x11, 0, 0);
        delay(120);
        const uint8_t orientation = 0, format = 5, porch[] = {
            12, 12, 0, 0x33, 0x33
        };
        burst(0x36, &orientation, 1);
        burst(0x3a, &format, 1);
        burst(0xb2, porch, 5);
        const uint8_t commands[] = {
            0xb7, 0xbb, 0xc2, 0xc3, 0xc4, 0xc6
        };
        const uint8_t values[] = {
            0x35, 0x32, 1, 0x15, 0x20, 0xf
        };
        for (unsigned i = 0; i < 6; ++i)
            burst(commands[i], values+i, 1);
        const uint8_t power[] = {
            0xa4, 0xa1
        }, gamma[] = {
            0xd0, 8, 14, 9, 9, 5, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34
        };
        burst(0xd0, power, 2);
        burst(0xe0, gamma, 14);
        burst(0xe1, gamma, 14);
        burst(0x21, 0, 0);
    }
    display_rect(0, 0, 240, 240, 0);
    burst(0x29, 0, 0);
    if (board == 1)
        gpio_level(0, 2, false);
    return active;
}

void display_row(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n) {
    if (!active || row >= 15)
        return;
    if (n > 20)
        n = 20;
    for (unsigned c = 0; c < 20 && active; ++c) {
        unsigned char ch = c < n ? (unsigned char)text[c]:' ';
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        unsigned g = 0;
        while (characters[g] && (unsigned char)characters[g] != ch)
            ++g;
        if (!characters[g])
            g = 0;
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 12; ++x) {
                uint16_t color = x < 10 && y < 14 && (glyphs[g][x/2]&(1u << (y/2))) ? fg:bg;
                unsigned at = 2*(y*12+x);
                pixels[at] = color >> 8;
                pixels[at+1] = color;
            }
        window(c*12, row*16, 12, 16);
        dma(sizeof(pixels));
        gpio_level(cs_port, cs_pin, true);
    }
}

void display_text(unsigned row, uint16_t fg, const char *text) {
    display_row(row, fg, 0, text, (unsigned)strlen(text));
}

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

void display_release(void) {
    active = false;
    REG(0x11d00) = 0;
}
