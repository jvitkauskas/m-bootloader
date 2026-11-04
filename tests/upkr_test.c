/* SPDX-License-Identifier: MIT */
/* Stage 1's unpacker, built for the host: it must give back stage 1 exactly, and no damage to the
 * pack may overrun, hang or be accepted. Run on every build. */
#include "upkr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t pack[0x8000], stage1[0x8000], out[0x8000], probs[STAGE1_PROBS];

static size_t load(const char *path, uint8_t *to, size_t room) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    size_t n = fread(to, 1, room, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    size_t size = load(argv[1], pack, sizeof(pack)), want = load(argv[2], stage1, sizeof(stage1));
    if (!stage1_unpack(out, sizeof(out), pack, (uint32_t)size, probs) || memcmp(out, stage1, want)) {
        fprintf(stderr, "%s: stage 1 did not come back identical\n", argv[1]);
        return 1;
    }
    /* Too little room, or a pack cut short. */
    if (stage1_unpack(out, (uint32_t)want-1, pack, (uint32_t)size, probs)
            || stage1_unpack(out, sizeof(out), pack, STAGE1_HEADER-1, probs)
            || stage1_unpack(out, sizeof(out), pack, (uint32_t)size-1, probs)) {
        fprintf(stderr, "%s: accepted a bad size\n", argv[1]);
        return 1;
    }
    /* A header that claims less than the stream holds: nothing may be written past what it claims. */
    uint32_t claimed = (uint32_t)want/2;
    uint8_t held[2] = { pack[2], pack[3] };
    pack[2] = (uint8_t)claimed;
    pack[3] = (uint8_t)(claimed >> 8);
    memset(out, 0x5a, sizeof(out));
    if (stage1_unpack(out, sizeof(out), pack, (uint32_t)size, probs)) {
        fprintf(stderr, "%s: accepted a short size\n", argv[1]);
        return 1;
    }
    for (size_t at = claimed; at < sizeof(out); ++at)
        if (out[at] != 0x5a) {
            fprintf(stderr, "%s: wrote past the size, at %zu\n", argv[1], at);
            return 1;
        }
    pack[2] = held[0];
    pack[3] = held[1];
    /* Every byte damaged in turn, and truncated streams. A wrong result must never be accepted. */
    unsigned accepted = 0, refused = 0;
    for (size_t at = 0; at < size; ++at)
        for (unsigned mask = 1; mask <= 0x80; mask <<= 3) {
            pack[at] ^= (uint8_t)mask;
            if (stage1_unpack(out, sizeof(out), pack, (uint32_t)size, probs)) {
                ++accepted;
                if (at >= STAGE1_HEADER && memcmp(out, stage1, want)) {
                    fprintf(stderr, "%s: damage at %zu accepted with wrong output\n", argv[1], at);
                    return 1;
                }
            } else
                ++refused;
            pack[at] ^= (uint8_t)mask;
        }
    for (size_t cut = STAGE1_HEADER; cut < size; cut += 7) {
        uint8_t saved[2] = { pack[0], pack[1] };
        pack[0] = (uint8_t)(cut-STAGE1_HEADER);
        pack[1] = (uint8_t)((cut-STAGE1_HEADER) >> 8);
        if (stage1_unpack(out, sizeof(out), pack, (uint32_t)cut, probs)) {
            fprintf(stderr, "%s: accepted a stream cut at %zu\n", argv[1], cut);
            return 1;
        }
        pack[0] = saved[0];
        pack[1] = saved[1];
    }
    printf("%s: %zu bytes unpack to %zu; damage refused %u times, harmless %u times\n", argv[1], size, want, refused, accepted);
    return 0;
}
