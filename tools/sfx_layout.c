/* SPDX-License-Identifier: MIT */
/* The self-extracting installer's layout (docs/SPEC.md §6.3), worked out with the unpacker the unit runs (src/upkr.c),
 * built for the host:
 *   sfx-layout place installer.bin installer.pack   where the pack starts, as an address, so that unpacking it in place
 *                                                   at 0x01C0A800 never writes over what is still to be read
 *   sfx-layout check installer.bin sfx.bin           the image as the unit gets it, unpacked as the unit does: the
 *                                                   installer must come back byte for byte */
#include "upkr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOAD 0x01c0a800u
#define WINDOW 0xc000u          /* the RAM a staged updater has, to 0x01C16800 */
#define SAFETY 32u              /* more than the least gap that works */
static uint8_t installer[WINDOW], pack[WINDOW], image[WINDOW], ram[WINDOW], probs[STAGE1_PROBS];

static int same(const char *a, const char *b) {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

static size_t load(const char *path, uint8_t *to, size_t room) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    size_t n = fread(to, 1, room, f);
    if (!feof(f) || ferror(f)) {
        fprintf(stderr, "%s: too big\n", path);
        exit(2);
    }
    fclose(f);
    return n;
}

/* The pack laid out with its end at installer size + gap, as the unit has it: does it unpack? */
static int fits(size_t size, size_t packed, size_t gap) {
    size_t end = size+gap;
    if (end > WINDOW || packed > end)
        return 0;
    memset(ram, 0, sizeof(ram));
    memcpy(ram+end-packed, pack, packed);
    return stage1_unpack(ram, (uint32_t)size, ram+end-packed, (uint32_t)packed, probs) && !memcmp(ram, installer, size);
}

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    size_t size = load(argv[2], installer, sizeof(installer));
    if (same(argv[1], "place")) {
        size_t packed = load(argv[3], pack, sizeof(pack)), gap = 0;
        while (gap < 0x1000 && !fits(size, packed, gap))
            gap += 4;
        if (gap >= 0x1000 || !fits(size, packed, gap+SAFETY)) {
            fprintf(stderr, "%s: no place unpacks it in place\n", argv[3]);
            return 1;
        }
        /* where the pack starts: its end SAFETY bytes past the least gap, the start word aligned */
        size_t start = (size+gap+SAFETY-packed+3)&~(size_t)3;
        printf("0x%08x\n", (unsigned)(LOAD+start));
        return 0;
    }
    if (same(argv[1], "check")) {
        size_t n = load(argv[3], image, sizeof(image));
        /* the pack is where the image's header says: found by its 8-byte header, which names the installer's size */
        for (size_t at = 4; at+STAGE1_HEADER <= n; at += 4) {
            uint32_t packed = image[at] | (uint32_t)image[at+1] << 8, out = image[at+2] | (uint32_t)image[at+3] << 8;
            if (out != size || at+STAGE1_HEADER+packed > n)
                continue;
            memset(ram, 0, sizeof(ram));
            memcpy(ram, image, n);
            if (stage1_unpack(ram, (uint32_t)size, ram+at, (uint32_t)(STAGE1_HEADER+packed), probs)
                    && !memcmp(ram, installer, size)) {
                printf("%s: unpacks in place to %s (%zu bytes, the pack at +%zu, the image %zu bytes)\n", argv[3],
                    argv[2], size, at, n);
                return 0;
            }
        }
        fprintf(stderr, "%s: does not unpack to %s in place\n", argv[3], argv[2]);
        return 1;
    }
    return 2;
}
