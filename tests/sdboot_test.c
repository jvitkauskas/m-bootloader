/* SPDX-License-Identifier: MIT */
/* src/sdboot.c against a simulated SPI card holding generated FAT32 images. */
#define _DEFAULT_SOURCE
#include "../src/sdboot.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#define SECTORS 4096u
#define RESERVED 4u
#define FAT_SECTORS 8u
#define SPC 2u
#define ENTRIES (SPC*512u/32u)
#define PARTITION 8u

static uint8_t disk[SECTORS*512], *boot, *fat;
static uint32_t data_start, next_free, high_lba;
static uint32_t con, baud, data;
static unsigned bus_state;
static bool selected, present, read_fails;
static uint8_t command[6], reply[600];
static unsigned command_len, reply_pos, reply_len;

static void answer(uint8_t b) {
    reply[reply_len++] = b;
}

static void card_command(void) {
    unsigned c = command[0]&0x3f;
    uint32_t arg = (uint32_t)command[1] << 24 | (uint32_t)command[2] << 16 | (uint32_t)command[3] << 8 | command[4];
    reply_pos = reply_len = 0;
    if (c == 0 || c == 55)
        answer(1);
    else if (c == 8) {
        answer(1);
        for (unsigned i = 0; i < 4; ++i)
            answer(i == 2 ? 1:i == 3 ? 0xaa:0);
    } else if (c == 41)
        answer(0);
    else if (c == 58) {
        answer(0);
        answer(0x40); /* high capacity: addressed by sector */
        for (unsigned i = 0; i < 3; ++i)
            answer(0);
    } else if (c == 17) {
        /* Keeps one far cluster of a big card in the image. */
        if (high_lba && arg >= high_lba)
            arg = arg-high_lba+SECTORS-SPC;
        if (read_fails || arg >= SECTORS) {
            answer(4);
            return;
        }
        answer(0);
        answer(0xfe);
        memcpy(reply+reply_len, disk+arg*512, 512);
        reply_len += 512;
        answer(0);
        answer(0);
    } else
        answer(4);
}

static uint8_t card_byte(uint8_t in) {
    if (!present || !selected)
        return 0xff;
    if (command_len || (in&0xc0) == 0x40) {
        command[command_len++] = in;
        if (command_len == 6) {
            command_len = 0;
            card_command();
        }
        return 0xff;
    }
    return reply_pos < reply_len ? reply[reply_pos++]:0xff;
}

/* x() always goes: control, data write, control until done, data read. */
volatile uint32_t *platform_register(uintptr_t address) {
    if (address == SPI1) {
        if (bus_state == 1) {
            data = card_byte((uint8_t)data);
            con |= BIT(15);
            bus_state = 2;
        }
        return &con;
    }
    if (address == SPI1+8) {
        bus_state = bus_state == 0 ? 1:0;
        return &data;
    }
    assert(address == SPI1+4);
    return &baud;
}

void gpio_level(unsigned port, unsigned pin, bool high) {
    assert(port == 2);
    if (pin == 7) {
        selected = high;
        reply_pos = reply_len = 0;
        command_len = 0;
    }
}

void gpio_output(unsigned port, unsigned pin, bool high) {
    (void)port;
    (void)pin;
    (void)high;
}

void gpio_input(unsigned port, unsigned pin, bool up, bool down) {
    (void)port;
    (void)pin;
    (void)up;
    (void)down;
}

bool hal_delay_us(uint32_t usec) {
    (void)usec;
    return true;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p+2, (uint16_t)(v >> 16));
}

static void set_fat(uint32_t c, uint32_t v) {
    put32(fat+c*4, v);
}

static uint8_t *cluster(uint32_t c) {
    return disk+(data_start+(c-2)*SPC)*512;
}

static uint32_t allocate(uint32_t after) {
    uint32_t c = next_free++;
    set_fat(c, 0x0fffffff);
    if (after)
        set_fat(after, c);
    return c;
}

/* An empty card: base 0 has no partition table. */
static void format(uint32_t base) {
    memset(disk, 0, sizeof(disk));
    if (base) {
        put32(disk+454, base);
        put16(disk+510, 0xaa55);
    }
    boot = disk+base*512;
    put16(boot+11, 512);
    boot[13] = SPC;
    put16(boot+14, RESERVED);
    boot[16] = 2;
    put32(boot+36, FAT_SECTORS);
    put32(boot+44, 2);
    put16(boot+510, 0xaa55);
    fat = boot+RESERVED*512;
    data_start = base+RESERVED+2*FAT_SECTORS;
    next_free = 2;
    allocate(0);
}

static uint8_t *entry(uint32_t dir, unsigned index, const char *name, uint8_t attr, uint32_t start, uint32_t size) {
    uint8_t *e = cluster(dir)+index*32;
    memcpy(e, name, 11);
    e[11] = attr;
    put16(e+20, (uint16_t)(start >> 16));
    put16(e+26, (uint16_t)start);
    put32(e+28, size);
    return e;
}

static uint32_t folder(uint32_t parent, unsigned index, const char *name) {
    uint32_t c = allocate(0);
    entry(parent, index, name, 0x10, c, 0);
    return c;
}

static void image(uint8_t *out, size_t size) {
    for (size_t i = 0; i < size-8; ++i)
        out[i] = (uint8_t)(i*7+3);
    memcpy(out+size-8, "STG2", 4);
    put16(out+size-4, core_crc16(0, out, size-8));
    out[size-2] = out[size-1] = 0;
}

/* A file, in clusters taking every other one when fragmented. */
static void file(uint32_t dir, unsigned index, const char *name, const uint8_t *bytes, size_t size, bool fragmented) {
    uint32_t first = 0, c = 0;
    for (size_t done = 0; done < size || !first; done += SPC*512) {
        c = allocate(c);
        if (!first)
            first = c;
        if (done < size)
            memcpy(cluster(c), bytes+done, size-done < SPC*512 ? size-done:SPC*512);
        if (fragmented)
            allocate(0);
    }
    entry(dir, index, name, 0x20, first, (uint32_t)size);
}

static const char *run(void) {
    memset((void *)(uintptr_t)SD_STAGE2_ADDRESS, 0, SD_STAGE2_MAX);
    present = true;
    assert(init());
    return load();
}

static bool same(const char *a, const char *b) {
    if (!a)
        return false;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

static bool loaded(const uint8_t *bytes, size_t size) {
    return !memcmp((void *)(uintptr_t)SD_STAGE2_ADDRESS, bytes, size);
}

static uint8_t menu[9000];

static void good_card(uint32_t base) {
    format(base);
    uint32_t mboot = folder(2, 0, "MBOOT      ");
    file(mboot, 0, "MENU    BIN", menu, sizeof(menu), false);
}

int main(void) {
    void *ram = mmap((void *)(uintptr_t)SD_STAGE2_ADDRESS, SD_STAGE2_MAX, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(ram == (void *)(uintptr_t)SD_STAGE2_ADDRESS);
    image(menu, sizeof(menu));

    /* No card: DO stays high, and nothing answers. */
    present = false;
    assert(!sd_probe(60));
    present = true;
    good_card(PARTITION);
    assert(sd_probe(60));

    /* In a partition and without a partition table. */
    assert(!run() && loaded(menu, sizeof(menu)));
    good_card(0);
    assert(!run() && loaded(menu, sizeof(menu)));

    /* Scattered: a fragmented file, and the folder past the root directory's first cluster. */
    format(PARTITION);
    uint32_t more = allocate(2);
    for (unsigned i = 0; i < ENTRIES; ++i)
        entry(2, i, "OTHER   BIN", 0x20, 0, 0);
    uint32_t mboot = folder(more, 1, "MBOOT      ");
    entry(more, 0, "OTHER   BIN", 0x20, 0, 0);
    file(mboot, 0, "MENU    BIN", menu, sizeof(menu), true);
    assert(!run() && loaded(menu, sizeof(menu)));

    /* A cluster number with high bits, as on a big card. */
    uint8_t small[SPC*512];
    image(small, sizeof(small));
    format(PARTITION);
    mboot = folder(2, 0, "MBOOT      ");
    high_lba = data_start+0x10000u*SPC;
    memcpy(disk+(SECTORS-SPC)*512, small, sizeof(small));
    entry(mboot, 0, "MENU    BIN", 0x20, 0x10002u, sizeof(small));
    assert(!run() && loaded(small, sizeof(small)));
    high_lba = 0;

    /* Things that look like it: long-name and deleted entries, and the other kind of entry with the name. */
    format(PARTITION);
    entry(2, 0, "MBOOT      ", 0x0f, 0, 0);
    entry(2, 1, "MBOOT      ", 0x20, 0, 0);
    entry(2, 2, "\xe5" "BOOT      ", 0x10, 0, 0);
    mboot = folder(2, 3, "MBOOT      ");
    entry(mboot, 0, "MENU    BIN", 0x10, 0, 0);
    entry(mboot, 1, "MENU    BIN", 0x08, 0, 0);
    file(mboot, 2, "MENU    BIN", menu, sizeof(menu), false);
    assert(!run() && loaded(menu, sizeof(menu)));

    /* Missing: no folder, only a root MENU.BIN, an empty folder, a file named MBOOT, another file. */
    format(PARTITION);
    assert(same(run(), NO_MENU));
    file(2, 0, "MENU    BIN", menu, sizeof(menu), false);
    assert(same(run(), NO_MENU));
    format(PARTITION);
    folder(2, 0, "MBOOT      ");
    assert(same(run(), NO_MENU));
    format(PARTITION);
    file(2, 0, "MBOOT      ", menu, sizeof(menu), false);
    assert(same(run(), NO_MENU));
    format(PARTITION);
    mboot = folder(2, 0, "MBOOT      ");
    file(mboot, 0, "STAGE2  BIN", menu, sizeof(menu), false);
    assert(same(run(), NO_MENU));

    /* The first unused entry ends the directory, whatever is left after it. */
    format(PARTITION);
    mboot = allocate(0);
    file(mboot, 0, "MENU    BIN", menu, sizeof(menu), false);
    entry(2, 1, "MBOOT      ", 0x10, mboot, 0);
    assert(same(run(), NO_MENU));

    /* A full directory with nothing after it ends at the end of its chain. */
    format(PARTITION);
    for (unsigned i = 0; i < ENTRIES; ++i)
        entry(2, i, "OTHER   BIN", 0x20, 0, 0);
    assert(same(run(), NO_MENU));

    /* A bad image. */
    uint8_t bad[sizeof(menu)];
    memcpy(bad, menu, sizeof(bad));
    bad[100] ^= 1;
    format(PARTITION);
    file(folder(2, 0, "MBOOT      "), 0, "MENU    BIN", bad, sizeof(bad), false);
    assert(same(run(), "BAD MENU.BIN"));
    memcpy(bad, menu, sizeof(bad));
    bad[sizeof(bad)-8] ^= 1;
    format(PARTITION);
    file(folder(2, 0, "MBOOT      "), 0, "MENU    BIN", bad, sizeof(bad), false);
    assert(same(run(), "BAD MENU.BIN"));
    format(PARTITION);
    file(folder(2, 0, "MBOOT      "), 0, "MENU    BIN", menu, 15, false);
    assert(same(run(), "BAD MENU.BIN"));
    format(PARTITION);
    mboot = folder(2, 0, "MBOOT      ");
    entry(mboot, 0, "MENU    BIN", 0x20, 2, SD_STAGE2_MAX+1);
    assert(same(run(), "BAD MENU.BIN"));
    /* A size past its chain. */
    format(PARTITION);
    mboot = folder(2, 0, "MBOOT      ");
    file(mboot, 0, "MENU    BIN", menu, sizeof(menu), false);
    put32(cluster(mboot)+28, sizeof(menu)+SPC*512*2);
    assert(same(run(), "BAD MENU.BIN"));

    /* Not a card this can read. */
    good_card(PARTITION);
    put16(disk+510, 0);
    assert(same(run(), "NO SD CARD"));
    good_card(PARTITION);
    put16(boot+17, 512); /* FAT12/16's root entry count */
    assert(same(run(), "SD NOT FAT32"));
    good_card(PARTITION);
    assert(init());
    read_fails = true;
    assert(same(load(), "NO SD CARD"));
    read_fails = false;

    puts("PASS: card probe, /MBOOT/MENU.BIN lookup and load, and refusals on generated FAT32 images");
    return 0;
}
