/* SPDX-License-Identifier: MIT */
/* FM-1 SD mod, stage 1: load /MBOOT/MENU.BIN from a FAT32 card into RAM and call it.
 *
 * The card shares the panel's SPI1: CLK PC9, DI PC10, DO PC8 (the panel's D/C), CS = NOT PC7 (an inverter on the
 * panel's CS), so the card is selected while PC7 is high. A session first sends the panel a NOP, so the bytes
 * clocked while the card is deselected are ignored, then turns PC8 into SPI1's DI and SPI1 to separate DI/DO.
 *
 * /MBOOT/MENU.BIN is a flat image linked for SD_STAGE2_ADDRESS, entry at its first byte, at most
 * SD_STAGE2_MAX bytes, ending in an 8-byte trailer: "STG2", CRC16 (core_crc16, little endian) of everything
 * before the trailer, two zero bytes. It is called with the LSB clock in MHz and may return. */
#include "sdboot.h"
#include "core.h"
#include "platform.h"
#include <string.h>
#define SPI1 0x11d00u
#define PC(o) GPIO(2, o)
static uint8_t sb[512] __attribute__((aligned(4)));
static bool hc;
static uint32_t fat_lba, data_lba;
static unsigned spc;

static uint8_t x(uint8_t b) {
    REG(SPI1) |= BIT(14);
    REG(SPI1+8) = b;
    sync_cpu();
    for (unsigned n = 0; n < 400000 && !(REG(SPI1)&BIT(15)); ++n)
        ;
    return (uint8_t)REG(SPI1+8);
}

static void card(bool on) {
    gpio_level(2, 7, on);
}

static uint8_t cmd(unsigned c, uint32_t a) {
    card(false);
    x(0xff);
    card(true);
    x(0xff);
    x((uint8_t)(0x40 | c));
    for (int s = 24; s >= 0; s -= 8)
        x((uint8_t)(a >> s));
    x(c == 0 ? 0x95:c == 8 ? 0x87:1);
    uint8_t r = 0xff;
    for (unsigned i = 0; i < 16 && ((r = x(0xff))&0x80); ++i)
        ;
    return r;
}

static bool rd(uint32_t lba, uint8_t *p) {
    bool ok = false;
    if (!cmd(17, hc ? lba:lba << 9)) {
        uint8_t r;
        unsigned i = 0;
        while ((r = x(0xff)) == 0xff && ++i < 100000)
            ;
        if (r == 0xfe) {
            for (i = 0; i < 512; ++i)
                p[i] = x(0xff);
            x(0xff);
            x(0xff);
            ok = true;
        }
    }
    card(false);
    return ok;
}

static bool init(void) {
    card(false);
    for (unsigned i = 0; i < 10; ++i)
        x(0xff);
    uint8_t r = 0;
    for (unsigned i = 0; i < 20 && (r = cmd(0, 0)) != 1; ++i)
        ;
    if (r != 1)
        return false;
    bool v2 = cmd(8, 0x1aa) == 1;
    for (unsigned i = 0; i < 4; ++i)
        x(0xff);
    for (unsigned i = 0; i < 1000; ++i) {
        cmd(55, 0);
        if (!(r = cmd(41, v2 ? BIT(30):0)))
            break;
        hal_delay_us(1000);
    }
    hc = false;
    if (!r && v2 && !cmd(58, 0)) {
        hc = (x(0xff)&0x40) != 0;
        x(0xff);
        x(0xff);
        x(0xff);
    }
    return !r;
}

static uint32_t next(uint32_t c) {
    if (!rd(fat_lba+c/128, sb))
        return 0;
    return core_le32(sb+(c%128)*4)&0x0fffffff;
}

static bool cluster_ok(uint32_t c) {
    return c >= 2 && c < 0x0ffffff8;
}

#define NO_MENU "NO /MBOOT/MENU.BIN"

/* Finds an 8.3 name in a directory; type is the entry's folder and volume-label bits. */
static const char *find(uint32_t c, const char *name, unsigned type, uint32_t *start, uint32_t *size) {
    for (; cluster_ok(c); c = next(c))
        for (unsigned s = 0; s < spc; ++s) {
            if (!rd(data_lba+(c-2)*spc+s, sb))
                return "SD READ ERROR";
            for (unsigned e = 0; e < 512; e += 32) {
                if (!sb[e])
                    return NO_MENU;
                if (!memcmp(sb+e, name, 11) && (sb[e+11]&0x18) == type) {
                    *start = core_le16(sb+e+26) | (uint32_t)core_le16(sb+e+20) << 16;
                    *size = core_le32(sb+e+28);
                    return 0;
                }
            }
        }
    return NO_MENU;
}

static const char *load(void) {
    uint32_t base = 0;
    if (!rd(0, sb) || core_le16(sb+510) != 0xaa55)
        return "NO SD CARD";
    if (core_le16(sb+11) != 512) {
        base = core_le32(sb+454);
        if (!rd(base, sb))
            return "SD READ ERROR";
    }
    if (core_le16(sb+11) != 512 || core_le16(sb+17) || !sb[13])
        return "SD NOT FAT32";
    spc = sb[13];
    fat_lba = base+core_le16(sb+14);
    data_lba = fat_lba+sb[16]*core_le32(sb+36);
    uint32_t c = core_le32(sb+44), start = 0, size = 0;
    const char *error = find(c, "MBOOT      ", 0x10, &start, &size);
    if (!error)
        error = find(start, "MENU    BIN", 0, &start, &size);
    if (error)
        return error;
    if (size < 16 || size > SD_STAGE2_MAX)
        return "BAD MENU.BIN";
    c = start;
    uint8_t *p = (uint8_t *)(uintptr_t)SD_STAGE2_ADDRESS;
    for (uint32_t done = 0; done < size; c = next(c)) {
        if (!cluster_ok(c))
            return "BAD MENU.BIN";
        for (unsigned s = 0; s < spc && done < size; ++s, done += 512)
            if (!rd(data_lba+(c-2)*spc+s, p+done))
                return "SD READ ERROR";
    }
    p += size-8;
    if (memcmp(p, "STG2", 4) || core_le16(p+4) != core_crc16(0, (uint8_t *)(uintptr_t)SD_STAGE2_ADDRESS, size-8))
        return "BAD MENU.BIN";
    return 0;
}

static void session_open(unsigned lsb_mhz) {
    /* Panel NOP (D/C low, CS low), then PC8 as the card's DO, SPI1 separate DI/DO at ~400 kHz. */
    gpio_level(2, 8, false);
    card(false);
    REG(SPI1) = 0x4021;
    x(0);
    gpio_input(2, 8, true, false);
    REG(SPI1) = 0x4029;
    REG(SPI1+4) = lsb_mhz*5/2-1;
}

static void session_close(void) {
    card(false);
    x(0xff);
    REG(SPI1) = 0x4021;
    REG(SPI1+4) = 4;
    gpio_output(2, 8, true);
}

bool sd_probe(unsigned lsb_mhz) {
    session_open(lsb_mhz);
    bool found = init();
    session_close();
    return found;
}

const char *sd_stage2(unsigned lsb_mhz) {
    session_open(lsb_mhz);
    const char *error = "NO SD CARD";
    if (init()) {
        REG(SPI1+4) = lsb_mhz/4-1;
        error = load();
    }
    session_close();
    if (error)
        return error;
    sync_cpu();
    ((void (*)(unsigned))(uintptr_t)SD_STAGE2_ADDRESS)(lsb_mhz);
    return 0;
}
