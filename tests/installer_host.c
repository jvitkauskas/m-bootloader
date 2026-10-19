/* SPDX-License-Identifier: MIT */
/* Host driver for tests/fwsc_test.py, running the C the chip runs:
 *   install IMAGE CONTAINER OUT   core_boot_install on IMAGE's boot area
 *   decode STREAM OUT             core_updater_decode on STREAM staged in flash
 *   retire IMAGE END OUT          core_request_retire for the stream ending at END */
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAPACITY 0x100000u
#define STAGED 0xb9000u
static uint8_t flash[CAPACITY];

static size_t load(const char *path, void *out, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    size_t n = fread(out, 1, limit, f);
    fclose(f);
    return n;
}

static void save(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        perror(path);
        exit(2);
    }
    fclose(f);
}

static bool flash_read(void *ctx, uint32_t address, void *out, uint32_t size) {
    (void)ctx;
    if (address > CAPACITY || size > CAPACITY - address)
        return false;
    memcpy(out, flash + address, size);
    return true;
}

/* NOR: programming only clears bits. */
static bool flash_program(void *ctx, uint32_t address, const void *data, uint32_t size) {
    (void)ctx;
    if (address > CAPACITY || size > CAPACITY - address)
        return false;
    for (uint32_t i = 0; i < size; ++i)
        flash[address + i] &= ((const uint8_t *)data)[i];
    return true;
}

static bool same(const char *a, const char *b) {
    return strlen(a) == strlen(b) && !memcmp(a, b, strlen(a));
}

int main(int argc, char **argv) {
    static uint8_t container[0x4000], out[CORE_UPDATE_MAX], work[CORE_WORK_SIZE];
    if (argc == 5 && same(argv[1], "install")) {
        if (load(argv[2], flash, CAPACITY) != CAPACITY)
            return 2;
        size_t size = load(argv[3], container, sizeof(container));
        if (!core_boot_install(flash, container, (uint32_t)size, CAPACITY))
            return 1;
        save(argv[4], flash, CAPACITY);
        return 0;
    }
    if (argc == 4 && same(argv[1], "decode")) {
        /* As the firmware stages it: the outer record over the stream's own,
         * holding the absolute address and the whole length (§6.3). */
        memset(flash, 0xff, CAPACITY);
        size_t size = load(argv[2], flash + STAGED, CAPACITY - STAGED);
        uint8_t *record = flash + STAGED, request[80] = {0};
        core_put_le32(record + 4, STAGED);
        core_put_le32(record + 8, (uint32_t)size);
        core_put_le16(record, core_crc16(0, record + 2, 30));
        core_put_le16(request + 2, 0x5a0d);
        core_put_le32(request + 72, STAGED);
        core_put_le16(request, core_crc16(0, request + 2, 78));
        struct core_flash f = {0, CAPACITY, flash_read};
        struct core_updater u;
        if (!core_updater_decode(&f, request, out, sizeof(out), work, &u) || u.load != CORE_UPDATE_LOAD || u.stream_end != STAGED + size)
            return 1;
        save(argv[3], out, u.size);
        return 0;
    }
    if (argc == 5 && same(argv[1], "retire")) {
        if (load(argv[2], flash, CAPACITY) != CAPACITY)
            return 2;
        struct core_nor nor = {{0, CAPACITY, flash_read}, flash_program, 0};
        struct core_header header;
        if (!core_header_read(&nor.flash, &header) || !core_request_retire(&nor, header.alignment, (uint32_t)strtoul(argv[3], 0, 0)))
            return 1;
        save(argv[4], flash, CAPACITY);
        return 0;
    }
    fprintf(stderr, "usage: installer-host install IMAGE CONTAINER OUT | decode STREAM OUT | retire IMAGE END OUT\n");
    return 2;
}
