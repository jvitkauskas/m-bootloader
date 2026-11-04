/* SPDX-License-Identifier: MIT */
/* Unpacks stage 1: Upkr with two parity contexts, after upkr's c_unpacker (public domain,
 * Dennis Ranke), then the call filter of tools/pack_stage1.py. Input and output are bounded,
 * so a bad stream can't run away: docs/SPEC.md §4.4. */
#include "upkr.h"
#define HAS_OFFSET 512u
#define OFFSET 513u
#define LENGTH 577u

struct upkr {
    const uint8_t *in, *end;
    uint8_t *probs;
    uint32_t state;
};

static uint32_t bit(struct upkr *s, uint32_t context) {
    while (s->state < 4096) {
        if (s->in == s->end)
            s->state = 4096;
        else
            s->state = s->state << 8 | *s->in++;
    }
    uint32_t prob = s->probs[context], low = s->state&255, high = s->state >> 8;
    if (low < prob) {
        s->state = prob*high+low;
        s->probs[context] = (uint8_t)(prob+((256-prob+8) >> 4));
        return 1;
    }
    s->state = (256-prob)*high+low-prob;
    s->probs[context] = (uint8_t)(prob-((prob+8) >> 4));
    return 0;
}

/* 0 if it doesn't fit: no real number is 0. */
static uint32_t number(struct upkr *s, uint32_t context) {
    uint32_t n = 0, at = 0;
    while (bit(s, context)) {
        if (at > 30)
            return 0;
        n |= bit(s, context+1) << at++;
        context += 2;
    }
    return n | 1u << at;
}

/* The bytes produced, or 0xffffffff if the stream is bad. */
static uint32_t unpack(uint8_t *out, uint32_t capacity, const uint8_t *in, const uint8_t *end, uint8_t *probs) {
    struct upkr s = { in, end, probs, 0 };
    uint32_t at = 0, offset = 0, after_match = 0, i;
    for (i = 0; i < STAGE1_PROBS; ++i)
        probs[i] = 128;
    for (; ; ) {
        uint32_t base = (at&1) << 8;
        if (bit(&s, base)) {
            if (after_match || bit(&s, HAS_OFFSET)) {
                uint32_t n = number(&s, OFFSET);
                if (n == 1)
                    return at;
                offset = n-1;
            }
            uint32_t n = number(&s, LENGTH);
            if (!offset || offset > at || !n || n > capacity-at)
                return UINT32_MAX;
            for (; n; --n, ++at)
                out[at] = out[at-offset];
            after_match = 1;
        } else {
            uint32_t c = 1;
            while (c < 256)
                c = c << 1 | bit(&s, base+c);
            if (at == capacity)
                return UINT32_MAX;
            out[at++] = (uint8_t)c;
            after_match = 0;
        }
    }
}

bool stage1_unpack(uint8_t *out, uint32_t capacity, const uint8_t *pack, uint32_t pack_size, uint8_t *probs) {
    if (pack_size < STAGE1_HEADER)
        return false;
    uint32_t packed = pack[0] | (uint32_t)pack[1] << 8, size = pack[2] | (uint32_t)pack[3] << 8;
    uint32_t want = pack[4] | (uint32_t)pack[5] << 8, crc = 0, i;
    if (packed > pack_size-STAGE1_HEADER || size > capacity
            || unpack(out, size, pack+STAGE1_HEADER, pack+STAGE1_HEADER+packed, probs) != size)
        return false;
    /* A relative call's target, in halfwords, was made absolute. */
    for (i = 0; i+4 <= size; ) {
        if (out[i] == 0xbf && out[i+1] == 0xea) {
            uint32_t v = (out[i+2] | (uint32_t)out[i+3] << 8)-(i >> 1);
            out[i+2] = (uint8_t)v;
            out[i+3] = (uint8_t)(v >> 8);
            i += 4;
        } else
            i += 2;
    }
    for (i = 0; i < size; ++i) {
        crc ^= (uint32_t)out[i] << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = crc&0x8000 ? (crc << 1 ^ 0x1021)&0xffff:(crc << 1)&0xffff;
    }
    return crc == want;
}
