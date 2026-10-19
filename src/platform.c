/* SPDX-License-Identifier: MIT */
#include "platform.h"
#include <string.h>
#define SPI 0x11c00u
#define P33 0x13e00u
static bool failed, dma_active;
static unsigned read_width = 1, timer_mhz = 24;
static uint32_t capacity;
static bool wait_mask(uint32_t address, uint32_t mask, bool value) {
    for (uint32_t n = 3000000; n; --n)
        if (!!(REG(address)&mask) == value)
            return true;
    failed = true;
    return false;
}

bool hal_fault(void) {
    return failed;
}

void hal_fault_clear(void) {
    failed = false;
}

void gpio_level(unsigned p, unsigned n, bool v) {
    if (v)
        GPIO(p, 0) |= BIT(n);
    else
        GPIO(p, 0) &= ~BIT(n);
}

void gpio_output(unsigned p, unsigned n, bool v) {
    gpio_level(p, n, v);
    GPIO(p, 8) &= ~BIT(n);
}

void gpio_input(unsigned p, unsigned n, bool u, bool d) {
    GPIO(p, 8) |= BIT(n);
    GPIO(p, 0xc) |= BIT(n);
    if (u)
        GPIO(p, 0x10) |= BIT(n);
    else
        GPIO(p, 0x10) &= ~BIT(n);
    if (d)
        GPIO(p, 0x14) |= BIT(n);
    else
        GPIO(p, 0x14) &= ~BIT(n);
}

bool gpio_read(unsigned p, unsigned n) {
    return !!(GPIO(p, 4)&BIT(n));
}

static void settle(unsigned n) {
#ifdef BOOT_SIM
    void boot_sim_settle(unsigned iterations);
    boot_sim_settle(n);
#endif
    while (n--)
        __asm__ volatile("nop" ::: "memory");
}

bool hal_delay_us(uint32_t us) {
    while (us) {
        uint32_t part = us > 100000 ? 100000:us;
        us -= part;
        REG(0x10400) = BIT(14);
        REG(0x10404) = 0;
        REG(0x10408) = timer_mhz*part;
        REG(0x10400) = BIT(0);
        bool ok = wait_mask(0x10400, BIT(15), true);
        REG(0x10400) = BIT(14);
        if (!ok)
            return false;
    }
    return true;
}

static uint8_t p33_byte(uint8_t b) {
    REG(P33+8) |= 12;
    REG(P33+12) = b;
    REG(P33+8) |= 16;
    sync_cpu();
    if (!wait_mask(P33+8, 2, false))
        return 0xff;
    return (uint8_t)REG(P33+12);
}

static uint8_t p33_access(unsigned a, unsigned op, uint8_t b) {
    REG(P33+8) |= 1;
    p33_byte((uint8_t)(op | (a >> 8)));
    p33_byte((uint8_t)a);
    b = p33_byte(b);
    REG(P33+8) &= ~1u;
    return b;
}

uint8_t p33_read(unsigned a) {
    return p33_access(a, 0x80, 0x5e);
}

void p33_write(unsigned a, uint8_t b) {
    (void)p33_access(a, 0, b);
}

void p33_or(unsigned a, uint8_t b) {
    (void)p33_access(a, 0x20, b);
}

void p33_and(unsigned a, uint8_t b) {
    (void)p33_access(a, 0x40, b);
}

void hal_early(void) {
    failed = false;
    dma_active = false;
    p33_and(0x80, 0xef);
}

bool hal_clock_a(void) {
    /* §3.2: ROM's startup clock is slow. Register loops use 10x the tested
     * memory-counter counts; do not lengthen these or use Timer 0 here. */
    REG(0x1000c) &= ~BIT(1);
    settle(20);
    REG(0x1000c) &= ~BIT(8);
    settle(20);
    REG(0x10014) = (REG(0x10014)&~255u) | 6;
    REG(0x119a0) = 0x45400200;
    REG(0x119a4) = 0x3f503026;
    REG(0x119a0) |= 1;
    settle(100);
    REG(0x119a0) |= 2;
    uint32_t v = REG(0x119a8)&~3u;
    REG(0x119a8) = v;
    const uint32_t m0 = (31u << 2) | BIT(7) | (3u << 8) | BIT(10) | (3u << 22) | (3u << 24) | (3u << 26);
    REG(0x119a8) = (v&~m0) | (10u << 2) | (2u << 8) | (1u << 22) | (1u << 24) | (2u << 26);
    const uint32_t m1 = 0xfffu | (7u << 12) | (15u << 15) | (3u << 20) | (3u << 22) | (7u << 24);
    REG(0x119ac) = (REG(0x119ac)&~m1) | 268u | (3u << 12) | (1u << 20) | (1u << 22) | (7u << 24);
    REG(0x119a8) |= 1;
    settle(100);
    REG(0x119a8) |= 2;
    settle(900);
    REG(0x1000c) |= BIT(6) | BIT(7);
    REG(0x10008) = (REG(0x10008)&~0x30700u) | 0x10200;
    REG(0x1000c) |= BIT(8);
    sync_cpu();
    timer_mhz = 60;
    return !failed;
}

unsigned hal_clock_b(void) {
    unsigned mhz = 0;
    if ((REG(0x10014)&255) == 0x80 && !(REG(0x10008)&0x30700)) {
        REG(0x10008) |= BIT(8);
        sync_cpu();
        REG(0x10014) = (REG(0x10014)&~255u) | 0x40;
        sync_cpu();
        mhz = 96;
    } else if ((REG(0x10014)&255) == 0x40 && (REG(0x10008)&0x30700) == 0x100)
        mhz = 96;
    timer_mhz = 48;
    return mhz;
}

/* For code started after the clock switch (a staged updater): LSB MHz. */
void hal_timer_clock(unsigned mhz) {
    timer_mhz = mhz;
}

uint32_t hal_efuse(unsigned bank, unsigned row) {
    unsigned count = (REG(0x10200)&255) ? 4:3;
    uint32_t v = 0;
    p33_write(0xb0, (uint8_t)(bank << 6));
    for (unsigned j = 0; j < count; ++j) {
        p33_write(0xb1, (uint8_t)((row << 6) | (j << 2) | 2));
        if (!hal_delay_us(50))
            break;
        v |= (uint32_t)p33_read(0xb2) << (8*j);
        p33_write(0xb1, 0);
    }
    return v;
}

static void cs(bool high) {
    gpio_level(3, 0, high);
    settle(40);
}

static uint8_t spi_byte(uint8_t v, bool rx) {
    if (failed)
        return 0xff;
    if (rx)
        REG(SPI) |= BIT(12);
    else
        REG(SPI) &= ~BIT(12);
    REG(SPI+8) = v;
    sync_cpu();
    bool ok = wait_mask(SPI, BIT(15), true);
    uint8_t r = (uint8_t)REG(SPI+8);
    REG(SPI) |= BIT(14);
    return ok ? r:0xff;
}

static void spi_single(void) {
    REG(SPI) = (REG(SPI)&~((3u << 10) | BIT(12))) | BIT(3);
}

static void command(unsigned op) {
    spi_single();
    cs(false);
    spi_byte((uint8_t)op, false);
}

static void address24(uint32_t a) {
    spi_byte(a >> 16, false);
    spi_byte(a >> 8, false);
    spi_byte(a, false);
}

static uint8_t status(unsigned op) {
    command(op);
    uint8_t v = spi_byte(255, true);
    cs(true);
    return v;
}

static bool ready(void) {
    for (unsigned i = 0; i < 30000; ++i) {
        if (failed)
            return false;
        if (!(status(5)&1))
            return !failed;
        if (!hal_delay_us(100))
            return false;
    }
    failed = true;
    return false;
}

static void wren(void) {
    command(6);
    cs(true);
}

static bool write_status(uint8_t s1, uint8_t s2, bool combined) {
    wren();
    command(1);
    spi_byte(s1, false);
    if (combined)
        spi_byte(s2, false);
    cs(true);
    if (!ready())
        return false;
    if (!combined) {
        wren();
        command(0x31);
        spi_byte(s2, false);
        cs(true);
    }
    return ready();
}

static bool unprotect(void) {
    uint8_t a = status(5), b = status(0x35);
    if ((a&0x7c) || (b&0x40)) {
        if (!write_status(a&~0x7f, b&~0x40, true))
            return false;
        if ((status(5)&0x7c) || (status(0x35)&0x40))
            if (!write_status(a&~0x7f, b&~0x40, false))
                return false;
        if ((status(5)&0x7c) || (status(0x35)&0x40)) {
            failed = true;
            return false;
        }
    }
    return !failed;
}

bool flash_init(unsigned width, unsigned baud) {
    REG(0x40200) = 0;
    REG(0x13000) = 0;
    REG(0x51020) &= ~BIT(1);
    REG(0x5101c) &= ~BIT(5);
    gpio_output(3, 0, true);
    gpio_output(3, 4, false);
    gpio_output(3, 5, false);
    GPIO(3, 0x1c) |= BIT(4);
    gpio_input(3, 1, false, false);
    gpio_output(3, 2, true);
    gpio_output(3, 3, true);
    REG(SPI) = 0x28;
    REG(SPI+4) = baud;
    REG(SPI) |= BIT(14) | BIT(0);
    cs(true);
    /* Exit any continuous-read state inherited across reset/reinstallation. */
    command(0xff);
    spi_byte(0xff, false);
    cs(true);
    read_width = 1;
    dma_active = false;
    uint32_t id = flash_jedec();
    unsigned e = id&255;
    capacity = (e >= 16 && e <= 24) ? 1u << e:0;
    if (!capacity) {
        failed = true;
        return false;
    }
    if (width > 1) {
        uint8_t single[32] __attribute__((aligned(4)));
        uint8_t wide[32] __attribute__((aligned(4)));
        if (!flash_read(0, 0, single, sizeof(single)))
            return false;
        read_width = width == 4 ? (flash_quad() ? 4:2):2;
        while (read_width > 1) {
            if (!flash_read(0, 0, wide, sizeof(wide)))
                return false;
            if (!memcmp(single, wide, sizeof(single)))
                break;
            read_width /= 2;
        }
    }
    return !failed;
}

bool flash_quad(void) {
    uint8_t a = status(5), b = status(0x35);
    if (b&2)
        return !failed;
    if (!write_status(a&~3u, b | 2, true))
        return false;
    for (unsigned i = 0; !(status(0x35)&2) && i < 3; ++i)
        if (!write_status(a&~3u, b | 2, false))
            return false;
    if (!(status(0x35)&2))
        return false;
    read_width = 4;
    return !failed;
}

uint32_t flash_jedec(void) {
    command(0x9f);
    uint32_t id = 0;
    for (unsigned i = 0; i < 3; ++i)
        id = (id << 8) | spi_byte(255, true);
    cs(true);
    return id;
}

uint32_t flash_capacity(void) {
    return capacity;
}

unsigned flash_read_width(void) {
    return read_width;
}

void flash_uid(uint8_t out[8]) {
    command(0x4b);
    for (unsigned i = 0; i < 4; ++i)
        spi_byte(255, false);
    for (unsigned i = 0; i < 8; ++i)
        out[i] = spi_byte(255, true);
    cs(true);
}

static void dma_receive(void *out, uint32_t n) {
    REG(0x13000) = 2;
    REG(SPI) |= BIT(12) | BIT(14);
    REG(SPI+12) = DMA_ADDRESS(out);
    sync_cpu();
    REG(SPI+16) = n;
}

static bool dma_wait(void) {
    bool ok = wait_mask(SPI, BIT(15), true);
    REG(SPI) |= BIT(14);
    REG(0x13000) = 0;
    return ok && !failed;
}

bool flash_dma_start(uint32_t a, void *out, uint32_t n) {
    if (failed || dma_active || !n || n > 65535 || a > capacity || n > capacity-a)
        return false;
    command(read_width == 4 ? 0x6b:read_width == 2 ? 0x3b:3);
    address24(a);
    if (read_width > 1) {
        uint32_t dummy;
        REG(SPI) = (REG(SPI)&~(BIT(3) | (3u << 10))) | ((read_width == 4 ? 2u:1u) << 10) | BIT(12);
        /* §3.4: eight dummy clocks are a separate wide DMA receive. */
        dma_receive(&dummy, read_width);
        if (!dma_wait()) {
            cs(true);
            spi_single();
            return false;
        }
    }
    dma_receive(out, n);
    dma_active = true;
    return true;
}

bool flash_dma_finish(void) {
    if (!dma_active)
        return true;
    bool ok = dma_wait();
    cs(true);
    spi_single();
    dma_active = false;
    return ok;
}

bool flash_read(void *ctx, uint32_t a, void *out, uint32_t n) {
    (void)ctx;
    uint8_t *p = out;
    if (a > capacity || n > capacity-a)
        return false;
    while (n) {
        uint32_t k = n > 4096 ? 4096:n;
        if (!flash_dma_start(a, p, k) || !flash_dma_finish())
            return false;
        a += k;
        p += k;
        n -= k;
    }
    return !failed;
}

bool flash_program(void *ctx, uint32_t a, const void *data, uint32_t n) {
    (void)ctx;
    const uint8_t *p = data;
    if (a > capacity || n > capacity-a || !unprotect())
        return false;
    while (n) {
        unsigned k = 256-(a&255);
        if (k > n)
            k = n;
        wren();
        command(2);
        address24(a);
        for (unsigned i = 0; i < k; ++i)
            spi_byte(p[i], false);
        cs(true);
        if (!ready())
            return false;
        a += k;
        p += k;
        n -= k;
    }
    return !failed;
}

bool flash_erase(void *ctx, uint32_t a, uint32_t n) {
    (void)ctx;
    if ((n != 4096 && n != 65536) || (a&(n-1)) || a > capacity || n > capacity-a || !unprotect())
        return false;
    wren();
    command(n == 4096 ? 0x20:0xd8);
    address24(a);
    cs(true);
    return ready();
}

void flash_release(void) {
    flash_dma_finish();
    REG(SPI) = 0;
    REG(0x13000) = 0;
    REG(0x40300) = 0;
}

bool hal_xip(uint32_t base, uint16_t key, unsigned width, unsigned baud) {
    flash_release();
    REG(0x5101c) &= ~BIT(16);
    REG(0x40200) = 0xf00000;
    REG(0x40200) = 0;
    REG(0x40204) = baud;
    REG(0x40200) = ((width == 4 ? 0x00280381:width == 2 ? 0x00280281:0x00200089)&~0xf00000u) | 0x900000;
    REG(0x4020c) = base;
    REG(0x40304) = key;
    REG(0x1eee008) &= ~(BIT(8) | BIT(9));
    const uint32_t ranges[][2] = {
        {
            0x1f00000, 0x1f04000
        }, {
            0x1f08000, 0x1f0a000
        }, {
            0x1f0a000, 0x1f0a200
        }, {
            0x1f0b000, 0x1f0b200
        }
    };
    for (unsigned i = 0; i < 4; ++i)
        for (uint32_t a = ranges[i][0]; a < ranges[i][1]; a += 4)
            REG(a) = 0;
    REG(0x1eee008) |= BIT(8) | BIT(9);
    irq_off();
    if (!wait_mask(0x1eee008, BIT(14), true))
        return false;
    REG(0x40300) = 1;
    REG(0x1eee008) |= BIT(8) | BIT(9);
    sync_cpu();
    return true;
}

void hal_reset(void) {
#ifdef BOOT_SIM
    void boot_sim_reset(void) __attribute__((noreturn));
    boot_sim_reset();
#endif
    REG(0x10000) |= BIT(4);
    sync_cpu();
    for (; ; ) {
#ifndef PLATFORM_SIM
        __asm__ volatile("idle");
#endif
    }
}
