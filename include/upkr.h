/* SPDX-License-Identifier: MIT */
#ifndef MB_UPKR_H
#define MB_UPKR_H
#include <stdbool.h>
#include <stdint.h>
#define STAGE1_HEADER 8u
#define STAGE1_PROBS 641u
/* Unpacks a packed stage 1 (docs/SPEC.md §4.4) to out, which has room for capacity bytes.
 * False if anything is wrong with it. probs is STAGE1_PROBS bytes of scratch. */
bool stage1_unpack(uint8_t *out, uint32_t capacity, const uint8_t *pack, uint32_t pack_size, uint8_t *probs);
#endif
