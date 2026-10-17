/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "platform.h"
static uint32_t saved[2][8], phy;
static bool smk, saved_valid, phy_active;
static unsigned led_phase;
static const unsigned offsets[] = {
    0, 8, 12, 16, 20, 24, 28, 32
};
static void shift(uint32_t word) {
    for (unsigned b = 0; b < (smk ? 4u:2u); ++b) {
        unsigned shift_by = smk ? b*8:(1-b)*8;
        uint8_t byte = (uint8_t)(word >> shift_by);
        for (unsigned bit = 8; bit; --bit) {
            gpio_level(0, 4, !!(byte&(1u << (bit-1))));
            gpio_level(0, 3, true);
            sync_cpu();
            gpio_level(0, 3, false);
        }
    }
    gpio_level(0, 1, true);
    sync_cpu();
    gpio_level(0, 1, false);
}

static void dark(void) {
    if (smk) {
        if (phy_active)
            REG(0x16a0c) |= BIT(17) | BIT(18);
    }
    else {
        gpio_level(0, 9, false);
        gpio_level(0, 10, false);
        gpio_level(7, 6, false);
        gpio_level(7, 9, false);
    }
}

void board_init(bool s) {
    smk = s;
    led_phase = 0;
    phy_active = false;
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned r = 0; r < 8; ++r)
            saved[p][r] = GPIO(p ? 7:0, offsets[r]);
    phy = REG(0x16a0c);
    saved_valid = true;
    gpio_output(0, 1, false);
    gpio_output(0, 3, false);
    gpio_output(0, 4, false);
    if (smk) {
        gpio_output(0, 0, false);
        gpio_input(7, 8, true, false);
    }
    else {
        /* PA2 also enables the FM-1 key matrix; saved[] preserves its state. */
        gpio_output(0, 2, false);
        gpio_input(0, 6, true, false);
        gpio_input(0, 8, true, false);
        gpio_output(0, 9, false);
        gpio_output(0, 10, false);
        gpio_output(7, 6, false);
        gpio_output(7, 9, false);
    }
    dark();
    shift(UINT32_MAX);
}

unsigned board_keys(void) {
    static const uint32_t smk_words[] = {
        0xfffeffff, 0xefffffff, 0xfeffffff, 0xf7ffffff
    };
    static const uint32_t fm_words[] = {
        0xf7fe, 0xeffd, 0xfbff, 0xff7f
    };
    unsigned keys = 0;
    dark();
    for (unsigned i = 0; i < 4; ++i) {
        shift(smk ? smk_words[i]:fm_words[i]);
        hal_delay_us(10);
        if (!gpio_read(smk ? 7:0, smk ? 8:(i < 2 ? 8:6)))
            keys |= 1u << i;
    }
    shift(UINT32_MAX);
    return keys;
}

void board_wait(unsigned ms, bool leds) {
    while (ms--) {
        dark();
        if (leds) {
            if (smk) {
                if (!phy_active) {
                    REG(0x16a0c) |= 0x1000f | BIT(17) | BIT(18);
                    phy_active = true;
                }
                bool group = (led_phase++&1) != 0;
                shift(UINT32_MAX & ~(group ? (BIT(0) | BIT(12)):BIT(0)));
                REG(0x16a0c) &= ~BIT(group ? 18:17);
            } else {
                unsigned p = led_phase++%3;
                shift(p == 0 ? 0xf7fe:(p == 1 ? 0xeffd:0xfbff));
                gpio_level(p == 2 ? 0:7, p == 2 ? 10:9, true);
            }
        }
        hal_delay_us(1000);
    }
    dark();
    shift(UINT32_MAX);
}

bool board_hold(unsigned key, unsigned ms) {
    if (!(board_keys()&key))
        return false;
    unsigned missing = 0;
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) {
        board_wait(20, false);
        if (board_keys()&key)
            missing = 0;
        else if (++missing >= 2)
            return false;
    }
    return true;
}

void board_restore(void) {
    if (!saved_valid)
        return;
    dark();
    shift(UINT32_MAX);
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned r = 0; r < 8; ++r)
            GPIO(p ? 7:0, offsets[r]) = saved[p][r];
    if (phy_active)
        REG(0x16a0c) = phy;
    phy_active = false;
    saved_valid = false;
}
