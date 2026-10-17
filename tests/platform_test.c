/* SPDX-License-Identifier: MIT */
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SPI 0x11c00u
#define P33_CTRL 0x13e08u
#define P33_DATA 0x13e0cu
#define TIMER 0x10400u
static uint32_t low[0x60000/4], cache[0xb200/4], cache_control;
static uintptr_t previous_address;
static bool spi_write, spi_reply, p33_write_pending, p33_reply, timer_stuck;
static unsigned spi_position, p33_count, p33_busy, timer_count;
static uint8_t p33_bytes[16];
static uint32_t periods[16], timer_seen;
/* Commit writes made through the previous MMIO lvalue before the next access.
 * Completion/response values are supplied by this independent peripheral model.
 */
static void advance(void)
{
    if (low[SPI/4] & BIT(14))
        low[SPI/4] &= ~(BIT(14) | BIT(15));
    if (spi_write) {
        static const uint8_t jedec[] = {
            0xeb, 0x60, 0x14
        };
        if (low[SPI/4] & BIT(12)) {
            assert(spi_position < sizeof(jedec));
            low[(SPI+8)/4] = jedec[spi_position++];
        } else
            low[(SPI+8)/4] = 0xff;
        low[SPI/4] |= BIT(15);
        spi_write = false;
        spi_reply = true;
    }
    uint32_t *ctrl = low + P33_CTRL/4;
    if (p33_write_pending && previous_address == P33_CTRL && (*ctrl & BIT(4))) {
        assert((*ctrl & 0x1d) == 0x1d);
        /* CS, byte fields and start held. */
        assert(p33_count < sizeof(p33_bytes));
        p33_bytes[p33_count++] = (uint8_t)low[P33_DATA/4];
        p33_write_pending = false;
        *ctrl |= BIT(1);
        p33_busy = 2;
    }
    if (low[TIMER/4] != timer_seen) {
        assert(low[TIMER/4] == 0x4000 || low[TIMER/4] == 1);
        if (low[TIMER/4] == 1) {
            assert(!low[(TIMER+4)/4] && low[(TIMER+8)/4]);
            assert(timer_count < sizeof(periods)/sizeof(periods[0]));
            periods[timer_count++] = low[(TIMER+8)/4];
            if (!timer_stuck)
                low[TIMER/4] |= BIT(15);
        }
        timer_seen = low[TIMER/4];
    }
}

volatile uint32_t *platform_register(uintptr_t address)
{
    advance();
    assert(!(address & 3));
    if (address == SPI+8) {
        if (spi_reply)
            spi_reply = false;
        else
            spi_write = true;
    }
    if (address == P33_DATA) {
        if (p33_reply)
            p33_reply = false;
        else
            p33_write_pending = true;
    }
    if (address == P33_CTRL && p33_busy && !--p33_busy) {
        low[P33_CTRL/4] &= ~BIT(1);
        low[P33_DATA/4] = 0x5a;
        p33_reply = true;
    }
    previous_address = address;
    if (address < 0x60000)
        return low + address/4;
    if (address == 0x1eee008)
        return &cache_control;
    assert(address >= 0x1f00000 && address < 0x1f0b200);
    return cache + (address-0x1f00000)/4;
}

static void reset_model(void)
{
    memset(low, 0, sizeof(low));
    memset(cache, 0xa5, sizeof(cache));
    previous_address = 0;
    spi_write = spi_reply = p33_write_pending = p33_reply = timer_stuck = false;
    spi_position = p33_count = p33_busy = timer_count = timer_seen = 0;
    cache_control = BIT(14);
    hal_fault_clear();
}

static void test_drive_and_mux(void)
{
    const uint32_t hd0 = 0x91234568, hd = 0x12340001, mux = 0xa5040020;
    for (unsigned entry_bit = 0; entry_bit < 2; ++entry_bit) {
        reset_model();
        low[(0x500c0+0x18)/4] = hd0;
        low[(0x500c0+0x1c)/4] = hd;
        low[0x5101c/4] = mux | (entry_bit ? BIT(16):0);
        assert(flash_init(1, 1));
        advance();
        assert(flash_capacity() == 0x100000 && spi_position == 3);
        assert(low[(0x500c0+0x18)/4] == hd0);
        /* §3.4: HD0 untouched. */
        assert(low[(0x500c0+0x1c)/4] == (hd | BIT(4)));
        assert((low[0x5101c/4]&BIT(16)) == (entry_bit ? BIT(16):0));
        assert(hal_xip(0x4000, 0x980f, 1, 1));
        assert(!(low[0x5101c/4]&BIT(16)));
        spi_position = 0;
        assert(flash_init(1, 1));
        advance();
        /* §3.6: raw SPI after XIP. */
        assert(!(low[0x5101c/4]&BIT(16)));
        assert((low[0x5101c/4]&~(BIT(5) | BIT(16))) == (mux&~(BIT(5) | BIT(16))));
        assert(!hal_fault());
    }
}

static void test_timer(void)
{
    reset_model();
    assert(hal_clock_a());
    assert(hal_delay_us(120000));
    advance();
    assert(timer_count == 2 && periods[0] == 6000000 && periods[1] == 1200000);
    timer_count = 0;
    assert(hal_delay_us(150000));
    advance();
    assert(timer_count == 2 && periods[0] == 6000000 && periods[1] == 3000000);
    timer_count = 0;
    assert(hal_delay_us(0));
    advance();
    assert(!timer_count);
    low[0x10014/4] = 0x80;
    low[0x10008/4] = 0;
    assert(hal_clock_b() == 96);
    assert(hal_delay_us(50));
    advance();
    assert(timer_count == 1 && periods[0] == 2400 && low[TIMER/4] == 0x4000);
    timer_stuck = true;
    assert(!hal_delay_us(1));
    advance();
    assert(hal_fault() && low[TIMER/4] == 0x4000);
}

static void expect_transaction(unsigned op, unsigned address, unsigned value)
{
    advance();
    assert(p33_count == 3 && !p33_busy);
    assert(p33_bytes[0] == (op | (address >> 8)));
    assert(p33_bytes[1] == (uint8_t)address && p33_bytes[2] == value);
    assert((low[P33_CTRL/4]&~31u) == 0xa5000000);
    assert(!(low[P33_CTRL/4]&BIT(0)) && (low[P33_CTRL/4]&BIT(4)));
    assert(!hal_fault());
    p33_count = 0;
}

static void test_p33(void)
{
    reset_model();
    low[P33_CTRL/4] = 0xa5000000;
    assert(p33_read(0x123) == 0x5a);
    expect_transaction(0x80, 0x123, 0x5e);
    p33_write(0x205, 0x6c);
    expect_transaction(0, 0x205, 0x6c);
    p33_or(0x80, 0x10);
    expect_transaction(0x20, 0x80, 0x10);
    p33_and(0x80, 0xef);
    expect_transaction(0x40, 0x80, 0xef);
}

int main(void)
{
    test_drive_and_mux();
    test_timer();
    test_p33();
    puts("platform: §3.4 HD register, §3.3 timer, §3.1 P33 and §3.6 raw/XIP mux regression checks passed");
}
