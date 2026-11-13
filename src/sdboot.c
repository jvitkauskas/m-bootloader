/* SPDX-License-Identifier: MIT */
/* SD mod, stage 1: load /MBOOT/MENU.BIN from a FAT32 card into RAM and call it.
 *
 * The card shares the panel's SPI1, with DO on the panel's D/C and CS = NOT the panel's CS (an inverter), so the card
 * is selected while that's high. FM-1: CLK PC9, DI PC10, DO PC8, CS = NOT PC7. SMK-37: CLK PB6, DI PB7, DO PB5,
 * CS = NOT PC8. A session first sends the panel a NOP, so the bytes clocked while the card is deselected are ignored,
 * then turns D/C into SPI1's DI and SPI1 to separate DI/DO.
 *
 * /MBOOT/MENU.BIN is a flat image linked for SD_STAGE2_ADDRESS, entry at its first byte, at most
 * SD_STAGE2_MAX bytes, ending in an 8-byte trailer: "STG2", CRC16 (core_crc16, little endian) of everything
 * before the trailer, two zero bytes. It is called with the LSB clock in MHz and may return. */
#include "sdboot.h"
#include "core.h"
#include "platform.h"
#include <string.h>
#define SPI1 0x11d00u
#ifdef BOARD_SMK
#define CS_PORT 2
#define CS_PIN 8
#define DO_PORT 1
#define DO_PIN 5
#else
#define CS_PORT 2
#define CS_PIN 7
#define DO_PORT 2
#define DO_PIN 8
#endif
/* The caller's, while loading. */
static uint8_t *sb;
/* crc: the card checks CRCs both ways (USB DISK); without, data's CRCs are ignored, as stage 2's loading always has. */
static bool hc, crc;
static unsigned disk_mhz;
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
    gpio_level(CS_PORT, CS_PIN, on);
}

static uint8_t cmd(unsigned c, uint32_t a) {
    uint8_t frame[5] = {(uint8_t)(0x40 | c), (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a}, crc7 = 0;
    card(false);
    x(0xff);
    card(true);
    x(0xff);
    for (unsigned i = 0; i < 5; ++i) {
        x(frame[i]);
        for (unsigned b = 0; b < 8; ++b) {
            crc7 = (uint8_t)(crc7 << 1);
            if ((frame[i] << b ^ crc7)&0x80)
                crc7 ^= 0x09;
        }
    }
    x((uint8_t)(crc7 << 1 | 1));
    uint8_t r = 0xff;
    for (unsigned i = 0; i < 16 && ((r = x(0xff))&0x80); ++i)
        ;
    return r;
}

/* A data block after a command's answer. */
static bool block(uint8_t *p, unsigned n) {
    uint8_t r;
    unsigned i = 0;
    while ((r = x(0xff)) == 0xff && ++i < 100000)
        ;
    if (r != 0xfe)
        return false;
    for (i = 0; i < n; ++i)
        p[i] = x(0xff);
    unsigned check = x(0xff) << 8;
    check |= x(0xff);
    return !crc || check == core_crc16(0, p, n);
}

static bool rd(uint32_t lba, uint8_t *p) {
    bool ok = !cmd(17, hc ? lba:lba << 9) && block(p, 512);
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
    /* Panel NOP (D/C low, CS low), then D/C as the card's DO, SPI1 separate DI/DO at ~400 kHz. */
    gpio_level(DO_PORT, DO_PIN, false);
    card(false);
    REG(SPI1) = 0x4021;
    x(0);
    gpio_input(DO_PORT, DO_PIN, true, false);
    REG(SPI1) = 0x4029;
    REG(SPI1+4) = lsb_mhz*5/2-1;
}

static void session_close(void) {
    card(false);
    x(0xff);
    REG(SPI1) = 0x4021;
    REG(SPI1+4) = 4;
    gpio_output(DO_PORT, DO_PIN, true);
}

bool sd_probe(unsigned lsb_mhz) {
    session_open(lsb_mhz);
    bool found = init();
    session_close();
    return found;
}

/* USB DISK's card: started at about 400 kHz, then run at 4 MHz as for stage 2, which has worked on every wiring tried.
 * CRCs catch a bad transfer, and a failure restarts the card, as some get stuck after an error (m-apps' AGENTS.md). */
static bool disk_start(void) {
    crc = false;
    REG(SPI1+4) = disk_mhz*5/2-1;
    bool ok = init() && !cmd(59, 1) && (hc || !cmd(16, 512));
    card(false);
    REG(SPI1+4) = disk_mhz/4-1;
    crc = ok;
    return ok;
}

uint32_t sd_disk_open(unsigned lsb_mhz) {
    uint8_t csd[16];
    disk_mhz = lsb_mhz;
    session_open(lsb_mhz);
    bool ok = disk_start() && !cmd(9, 0) && block(csd, 16);
    card(false);
    if (!ok)
        return 0;
    if (csd[0] >> 6 == 1)
        return (((uint32_t)(csd[7]&0x3f) << 16 | (uint32_t)csd[8] << 8 | csd[9])+1)*1024;
    uint32_t size = ((uint32_t)(csd[6]&3) << 10 | (uint32_t)csd[7] << 2 | csd[8] >> 6)+1;
    unsigned multiply = ((csd[9]&3u) << 1 | csd[10] >> 7)+2, length = csd[5]&15u;
    return size << multiply << length >> 9;
}

static bool ready(void) {
    for (unsigned i = 0; i < 2000000; ++i)
        if (x(0xff) == 0xff)
            return true;
    return false;
}

/* The card can only report a failed write after it, so this asks. */
static bool wr(uint32_t lba, const uint8_t *p) {
    bool ok = false;
    if (!cmd(24, hc ? lba:lba << 9)) {
        unsigned check = core_crc16(0, p, 512);
        x(0xff);
        x(0xfe);
        for (unsigned i = 0; i < 512; ++i)
            x(p[i]);
        x((uint8_t)(check >> 8));
        x((uint8_t)check);
        ok = (x(0xff)&0x1f) == 5 && ready() && !cmd(13, 0) && !x(0xff);
    }
    card(false);
    return ok;
}

bool sd_disk(uint32_t lba, uint8_t *p, bool write) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (write ? wr(lba, p):rd(lba, p))
            return true;
        disk_start();
    }
    return false;
}

void sd_disk_close(void) {
    /* A card keeps its state through a soft reset, and other programs may expect it without CRCs. */
    cmd(59, 0);
    crc = false;
    session_close();
}

const char *sd_stage2(unsigned lsb_mhz, uint8_t scratch[512]) {
    sb = scratch;
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
