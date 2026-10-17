/* SPDX-License-Identifier: MIT */
#include "loader.h"
#include "platform.h"
#include "display.h"
#include <string.h>
extern uint8_t __bss_start[], __bss_end[];
static struct loader_state loader;
static int handler(const uint8_t *cbw, uint8_t *answer) {
    return loader_dispatch(&loader, cbw, answer);
}

static uint16_t key(void) {
    return (uint16_t)(hal_efuse(1, 3) >> 8);
}

__attribute__((section(".entry"), used))
int loader_entry(const uint32_t *params) {
    int (*send)(const void *, uint32_t) = (void *)(uintptr_t)params[0];
    int (*receive)(void *, uint32_t) = (void *)(uintptr_t)params[1];
    uint32_t *slot = (void *)(uintptr_t)params[2];
    memset(__bss_start, 0, (size_t)(__bss_end-__bss_start));
    /* Keep ROM's watchdog policy; B never takes A's power/exception path. */
    hal_fault_clear();
    unsigned mhz = hal_clock_b();
    flash_init(1, 1);
    struct loader_services io = {
        .nor = {
            {
                0, flash_capacity(), flash_read
            }, flash_program, flash_erase
        },
        .send = send, .receive = receive, .read_start = flash_dma_start, .read_finish = flash_dma_finish,
        .jedec = flash_jedec, .key = key, .uid = flash_uid, .screen_init = display_init,
        .screen_row = display_row, .screen_progress = display_progress,
        .entry = LOADER_ADDRESS, .cpu_mhz = (uint8_t)mhz
    };
    loader_init(&loader, &io);
    sync_cpu();
    *slot = (uint32_t)(uintptr_t)handler;
    sync_cpu();
    return 0;
}
