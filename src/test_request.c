/* SPDX-License-Identifier: MIT */
/* For a test build's bootloader: a program for M-Flasher's run-ram that leaves a one-shot request in retained RAM and
 * resets (docs/SPEC.md §11.9). */
#include "platform.h"
#define RETAINED ((volatile uint8_t *)(uintptr_t)0x01c7fd80)

__attribute__((section(".entry"), used, noreturn)) void test_request_entry(void) {
    static const char request[8] = {'M', 'B', 'T', 'E', 'S', 'T', TEST_REQUEST, 0};
    irq_off();
    for (unsigned i = 0; i < sizeof request; ++i)
        RETAINED[i] = (uint8_t)request[i];
    sync_cpu();
    REG(0x10000) |= BIT(4);
    sync_cpu();
    for (; ; ) {
    }
}
