/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "display.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t registers[0x60000/4], stream_word, words[2048];
static unsigned bits, word_count, reads, pressed, dmas, serial_count, delays;
static bool smk, failed, delay_fail, spi_fail;
static uint8_t serial[20000];
static bool pending_byte;
static uint32_t gpio(unsigned p, unsigned o) {
    return 0x50000+p*0x40+o;
}

volatile uint32_t *platform_register(uintptr_t a) {
    assert(!(a&3) && a < 0x60000);
    if (pending_byte && a != 0x11d08) {
        assert(serial_count < sizeof(serial));
        serial[serial_count++] = registers[0x11d08/4];
        pending_byte = false;
    }
    if (a == 0x11d08)
        pending_byte = true;
    if (a == 0x11d00) {
        unsigned count = registers[0x11d10/4];
        if (count) {
            assert(count <= 384 && !(count&1));
            ++dmas;
            registers[0x11d10/4] = 0;
        }
        if (!spi_fail)
            registers[a/4] |= BIT(15);
        else
            registers[a/4] &= ~BIT(15);
    }
    return registers+a/4;
}

void gpio_level(unsigned p, unsigned n, bool high) {
    uint32_t *out = registers+gpio(p, 0)/4;
    bool old = !!(*out&BIT(n));
    if (high)
        *out |= BIT(n);
    else
        *out &= ~BIT(n);
    if (p == 0 && n == 3 && high && !old) {
        stream_word = (stream_word << 1) | !!(*out&BIT(4));
        ++bits;
    }
    if (p == 0 && n == 1 && high && !old) {
        assert(bits == (smk ? 32u:16u));
        assert(word_count < 2048);
        words[word_count++] = stream_word;
        bits = 0;
        stream_word = 0;
    }
}

void gpio_output(unsigned p, unsigned n, bool v) {
    gpio_level(p, n, v);
    registers[gpio(p, 8)/4] &= ~BIT(n);
}

void gpio_input(unsigned p, unsigned n, bool u, bool d) {
    registers[gpio(p, 8)/4] |= BIT(n);
    registers[gpio(p, 12)/4] |= BIT(n);
    if (u)
        registers[gpio(p, 16)/4] |= BIT(n);
    else
        registers[gpio(p, 16)/4] &= ~BIT(n);
    if (d)
        registers[gpio(p, 20)/4] |= BIT(n);
    else
        registers[gpio(p, 20)/4] &= ~BIT(n);
}

bool gpio_read(unsigned p, unsigned n) {
    assert(p == (smk ? 7u:0u));
    assert(n == (smk ? 8u:(reads%4 < 2 ? 8u:6u)));
    return !(pressed&(1u << (reads++%4)));
}

bool hal_delay_us(uint32_t us) {
    assert(us <= 150000);
    ++delays;
    if (delay_fail) {
        failed = true;
        return false;
    }
    return true;
}

bool hal_fault(void) {
    return failed;
}

void hal_fault_clear(void) {
    failed = false;
}

static void reset(bool s) {
    memset(registers, 0, sizeof(registers));
    memset(words, 0, sizeof(words));
    stream_word = bits = word_count = reads = pressed = dmas = serial_count = delays = 0;
    smk = s;
    failed = delay_fail = spi_fail = pending_byte = false;
}

static void keys(bool s) {
    reset(s);
    uint32_t original[2][8], phy = 0x76543210;
    unsigned offsets[] = {
        0, 8, 12, 16, 20, 24, 28, 32
    };
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned r = 0; r < 8; ++r)
        original[p][r] = registers[gpio(p ? 7:0, offsets[r])/4] = (p ? 0x1234u:0x56aau) ^ (r*17);
    /* Initial levels must permit detecting the first deliberate latch edge. */
    original[0][0] &= ~(BIT(1) | BIT(3));
    registers[gpio(0, 0)/4] = original[0][0];
    registers[0x16a0c/4] = phy;
    board_init(s);
    assert(word_count == 1);
    assert(registers[0x16a0c/4] == phy);
    pressed = KEY_PREV | KEY_SELECT | KEY_RECOVERY;
    assert(board_keys() == pressed);
    assert(reads == 4);
    const uint32_t smk_stream[] = {
        0xfffffeff, 0xffffffef, 0xfffffffe, 0xfffffff7
    };
    const uint32_t fm_stream[] = {
        0xf7fe, 0xeffd, 0xfbff, 0xff7f
    };
    for (unsigned i = 0; i < 4; ++i)
        assert(words[1+i] == (s ? smk_stream[i]:fm_stream[i]));
    assert(words[5] == (s ? UINT32_MAX:65535u));
    board_wait(6, false);
    assert(registers[0x16a0c/4] == phy);
    word_count = 6;
    /* Discard the unlit wait's final all-off shift. */
    board_wait(6, true);
    if (s)
        for (unsigned i = 6; i < 12; ++i) {
            assert(words[i] == ((i&1) ? 0xfeefffff:0xfeffffff));
            /* Shifted low byte still retains all three analog-mux bits. */
            assert((words[i] >> 24 & 0x38) == 0x38);
        }
    board_restore();
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned r = 0; r < 8; ++r)
            assert(registers[gpio(p ? 7:0, offsets[r])/4] == original[p][r]);
    assert(registers[0x16a0c/4] == phy);
}

static void panel(bool s) {
    reset(s);
    assert(!display_init(3));
    assert(!serial_count && !dmas);
    assert(display_init(s ? 2:1));
    assert(dmas && delays);
    assert((registers[0x51020/4]&BIT(4)) == (s ? 0:BIT(4)));
    unsigned before = dmas;
    display_rect(0, 0, 0, 10, 0xffff);
    display_rect(240, 0, 1, 1, 0xffff);
    display_rect(239, 239, 2, 1, 0xffff);
    display_row(15, 0xffff, 0, "HI", 2);
    assert(dmas == before);
    display_row(0, 0xffff, 0, "hello", 5);
    assert(dmas == before+20);
    display_progress(0, 0x7e0);
    display_progress(100, 0x7e0);
    display_progress(255, 0x7e0);
    before = dmas;
    display_release();
    display_rect(0, 0, 10, 10, 0);
    assert(dmas == before);
    delay_fail = true;
    assert(!display_init(s ? 2:1));
    assert(!failed);
    delay_fail = false;
    spi_fail = true;
    assert(!display_init(s ? 2:1));
    assert(!failed);
}

int main(void) {
    keys(false);
    keys(true);
    panel(false);
    panel(true);
    puts("board/panel: shift order, key columns, GPIO/PHY restoration, bounded display and zero-area paths passed");
}
