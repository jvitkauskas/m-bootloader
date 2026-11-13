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
/* USB DISK's side: CRC mode, the card's CSD, an old byte-addressed card, and faults to inject. The CRCs here are
 * worked out by long division, as the SD specification states them, not as the driver does. */
static bool crc_mode, sdsc, status_fails, stuck;
static uint8_t csd[16];
/* sticking_reads: reads that fail and leave the card refusing everything until it's restarted, as m-apps' AGENTS.md
 * tells of one card. */
static unsigned bad_command_crcs, corrupt_reads, rejected_writes, sticking_reads, writing, write_len;
static uint32_t write_at, writes;
static uint8_t write_buffer[514];

static unsigned crc_divide(const uint8_t *p, unsigned bytes, unsigned width, unsigned poly) {
    unsigned r = 0, top = 1u << width;
    for (unsigned i = 0; i < bytes*8+width; ++i) {
        r = r << 1 | (i < bytes*8 ? (unsigned)(p[i/8] >> (7-i%8)&1):0);
        if (r&top)
            r ^= poly;
    }
    return r;
}

static unsigned crc7_of(const uint8_t *p, unsigned n) {
    return crc_divide(p, n, 7, 0x89);
}

static unsigned crc16_of(const uint8_t *p, unsigned n) {
    return crc_divide(p, n, 16, 0x11021);
}

/* A field of the CSD, by its bit positions in the specification (bit 127 first). */
static void csd_field(unsigned high, unsigned low, uint32_t value) {
    for (unsigned bit = low; bit <= high; ++bit) {
        uint8_t mask = (uint8_t)(1u << (bit%8));
        if (value >> (bit-low)&1)
            csd[15-bit/8] |= mask;
        else
            csd[15-bit/8] &= (uint8_t)~mask;
    }
}

static void answer(uint8_t b) {
    reply[reply_len++] = b;
}

static void answer_block(const uint8_t *p, unsigned n, bool corrupt) {
    unsigned check = crc16_of(p, n);
    answer(0xfe);
    memcpy(reply+reply_len, p, n);
    if (corrupt)
        reply[reply_len+n/2] ^= 0x10;
    reply_len += n;
    answer((uint8_t)(check >> 8));
    answer((uint8_t)check);
}

static void card_command(void) {
    unsigned c = command[0]&0x3f;
    uint32_t arg = (uint32_t)command[1] << 24 | (uint32_t)command[2] << 16 | (uint32_t)command[3] << 8 | command[4];
    reply_pos = reply_len = 0;
    if (command[5] != (crc7_of(command, 5) << 1 | 1)) {
        ++bad_command_crcs;
        if (crc_mode || c == 0 || c == 8) {
            answer(0x08);
            return;
        }
    }
    if (c == 0) {
        crc_mode = stuck = false;
        answer(1);
    } else if (stuck)
        answer(4);
    else if (c == 55)
        answer(1);
    else if (c == 8) {
        if (sdsc) {
            answer(5); /* an old card, for which it's illegal */
            return;
        }
        answer(1);
        for (unsigned i = 0; i < 4; ++i)
            answer(i == 2 ? 1:i == 3 ? 0xaa:0);
    } else if (c == 41)
        answer(0);
    else if (c == 58) {
        answer(0);
        answer(sdsc ? 0:0x40); /* high capacity: addressed by sector */
        for (unsigned i = 0; i < 3; ++i)
            answer(0);
    } else if (c == 59) {
        crc_mode = arg&1;
        answer(0);
    } else if (c == 16)
        answer(arg == 512 ? 0:0x40);
    else if (c == 9) {
        answer(0);
        answer_block(csd, 16, false);
    } else if (c == 13) {
        answer(0);
        answer(status_fails ? 0x40:0);
    } else if (c == 17 || c == 24) {
        if (sdsc) {
            assert(!(arg&511));
            arg >>= 9;
        }
        /* Keeps one far cluster of a big card in the image. */
        if (high_lba && arg >= high_lba)
            arg = arg-high_lba+SECTORS-SPC;
        if (read_fails || arg >= SECTORS || (c == 17 && sticking_reads && sticking_reads--)) {
            stuck = c == 17 && !read_fails && arg < SECTORS;
            answer(4);
            return;
        }
        answer(0);
        if (c == 24) {
            writing = 1;
            write_at = arg;
            return;
        }
        answer_block(disk+arg*512, 512, corrupt_reads && corrupt_reads--);
    } else
        answer(4);
}

/* A written block: a token, the data and its CRC, then the card's verdict and a busy while it programs. */
static void write_byte(uint8_t in) {
    if (writing == 1) {
        if (in == 0xfe)
            writing = 2, write_len = 0;
        return;
    }
    write_buffer[write_len++] = in;
    if (write_len < sizeof(write_buffer))
        return;
    writing = 0;
    bool good = crc16_of(write_buffer, 512) == ((unsigned)write_buffer[512] << 8 | write_buffer[513]);
    if (good && rejected_writes) {
        --rejected_writes;
        good = false;
    }
    if (good) {
        memcpy(disk+write_at*512, write_buffer, 512);
        ++writes;
    }
    reply_pos = reply_len = 0;
    answer(good ? 0x05:0x0b);
    for (unsigned i = 0; i < 3; ++i)
        answer(0);
}

static uint8_t card_byte(uint8_t in) {
    if (!present || !selected)
        return 0xff;
    /* The command's answer goes out before the data comes in. */
    if (writing == 1 && reply_pos < reply_len)
        return reply[reply_pos++];
    if (writing) {
        write_byte(in);
        return 0xff;
    }
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

/* SPEC §11.5, stated here so that a wrong pin in the code can't pass. */
#ifdef BOARD_SMK
#define IS_CS(port, pin) ((port) == 2 && (pin) == 8)
#define IS_DO(port, pin) ((port) == 1 && (pin) == 5)
#else
#define IS_CS(port, pin) ((port) == 2 && (pin) == 7)
#define IS_DO(port, pin) ((port) == 2 && (pin) == 8)
#endif

void gpio_level(unsigned port, unsigned pin, bool high) {
    assert(IS_CS(port, pin) || IS_DO(port, pin));
    if (IS_CS(port, pin)) {
        selected = high;
        reply_pos = reply_len = 0;
        command_len = 0;
        writing = 0;
    }
}

void gpio_output(unsigned port, unsigned pin, bool high) {
    assert(IS_DO(port, pin));
    (void)high;
}

void gpio_input(unsigned port, unsigned pin, bool up, bool down) {
    assert(IS_DO(port, pin));
    assert(up && !down);
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

static uint8_t scratch[512];

static const char *run(void) {
    sb = scratch;
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

/* USB DISK's access: the size from the CSD, sectors read and written with CRCs both ways, faults retried, and the card
 * left without CRCs, as the card item and other programs expect it. */
static void disk_tests(void) {
    static const uint8_t check[9] = "123456789";
    uint8_t got[512], put[512];
    /* The specification's own values, so the long division above is what it says. */
    static const uint8_t cmd0[5] = {0x40, 0, 0, 0, 0}, cmd8[5] = {0x48, 0, 0, 1, 0xaa};
    assert((crc7_of(cmd0, 5) << 1 | 1) == 0x95 && (crc7_of(cmd8, 5) << 1 | 1) == 0x87);
    assert(crc16_of(check, 9) == 0x31c3);
    good_card(PARTITION);
    present = true;
    bad_command_crcs = 0;
    /* A high-capacity card: CSD version 2, C_SIZE 3 is 4 x 512 KiB. */
    memset(csd, 0, sizeof(csd));
    csd_field(127, 126, 1);
    csd_field(69, 48, 3);
    assert(sd_disk_open(60) == 4096 && crc_mode);
    assert(sd_disk(PARTITION, got, false) && !memcmp(got, disk+PARTITION*512, 512));
    for (unsigned i = 0; i < 512; ++i)
        put[i] = (uint8_t)(i*7+3);
    writes = 0;
    assert(sd_disk(100, put, true) && writes == 1 && !memcmp(disk+100*512, put, 512));
    assert(sd_disk(100, got, false) && !memcmp(got, put, 512));
    /* A read that arrives damaged, or a write the card turns down, is tried again, after restarting the card. */
    corrupt_reads = 1;
    assert(sd_disk(100, got, false) && !memcmp(got, put, 512) && !corrupt_reads && crc_mode);
    put[0] ^= 0xff;
    rejected_writes = 1;
    writes = 0;
    assert(sd_disk(101, put, true) && writes == 1 && !memcmp(disk+101*512, put, 512) && crc_mode);
    /* A card left refusing everything after a failed read needs restarting, which the retry does. */
    sticking_reads = 1;
    assert(sd_disk(100, got, false) && !memcmp(got, disk+100*512, 512) && !stuck && crc_mode);
    /* One that keeps failing is reported, not taken as done. */
    corrupt_reads = 3;
    assert(!sd_disk(100, got, false));
    corrupt_reads = 0;
    rejected_writes = 3;
    writes = 0;
    assert(!sd_disk(102, put, true) && !writes);
    rejected_writes = 0;
    /* A card that only says after the write that it failed. */
    status_fails = true;
    assert(!sd_disk(103, put, true));
    status_fails = false;
    /* Past the end of the card. */
    assert(!sd_disk(SECTORS, got, false));
    assert(!bad_command_crcs);
    sd_disk_close();
    assert(!crc_mode && !selected);
    /* A big card's size, from all 22 bits of C_SIZE: 0x3b37ff+1 x 512 KiB. */
    csd_field(69, 48, 0x3b37ff);
    assert(sd_disk_open(60) == 0x3b3800u*1024 && crc_mode);
    sd_disk_close();
    /* The card item still reads it afterwards. */
    assert(!run() && loaded(menu, sizeof(menu)));
    /* An old card, addressed by byte, and its version 1 CSD: 4096 x 2^(7+2) blocks of 2^9 bytes is 1 GiB. */
    sdsc = true;
    memset(csd, 0, sizeof(csd));
    csd_field(83, 80, 9);
    csd_field(73, 62, 4095);
    csd_field(49, 47, 7);
    assert(sd_disk_open(60) == 2097152 && crc_mode);
    assert(sd_disk(PARTITION, got, false) && !memcmp(got, disk+PARTITION*512, 512));
    writes = 0;
    assert(sd_disk(104, put, true) && writes == 1 && !memcmp(disk+104*512, put, 512));
    assert(!bad_command_crcs);
    sd_disk_close();
    sdsc = false;
    /* No card, and one whose CSD arrives damaged: nothing to serve. */
    present = false;
    assert(!sd_disk_open(60));
    sd_disk_close();
    present = true;
    corrupt_reads = 0;
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

    disk_tests();

    puts("PASS: card probe, /MBOOT/MENU.BIN lookup and load, and refusals on generated FAT32 images; USB DISK's sectors "
         "with CRCs both ways, faults retried, an old card");
    return 0;
}
