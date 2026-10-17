/* SPDX-License-Identifier: MIT */
#ifndef MB_LOADER_H
#define MB_LOADER_H
#include "core.h"
#define LOADER_VERSION "1.2"
struct loader_services {
    struct core_nor nor;
    int (*send)(const void *, uint32_t);
    int (*receive)(void *, uint32_t);
    bool (*read_start)(uint32_t, void *, uint32_t);
    bool (*read_finish)(void);
    uint32_t (*jedec)(void);
    uint16_t (*key)(void);
    void (*uid)(uint8_t [8]);
    bool (*screen_init)(unsigned);
    void (*screen_row)(unsigned, uint16_t, uint16_t, const char *, unsigned);
    void (*screen_progress)(unsigned, uint16_t);
    uint32_t entry;
    uint8_t cpu_mhz;
};
struct loader_state {
    struct loader_services io;
    uint32_t crc_table[256];
    uint8_t buffer[2][4096];
    uint8_t error, screen;
};
void loader_init(struct loader_state *, const struct loader_services *);
int loader_dispatch(struct loader_state *, const uint8_t command[31], uint8_t answer[16]);
#endif
