/* SPDX-License-Identifier: MIT */
#ifndef MB_CONTRACT_H
#define MB_CONTRACT_H
#include "core.h"
/* Pointers on the wire are always little-endian 32-bit CPU addresses. */
struct contract_app_layout {
    uint32_t header, physical, logical;
    uint16_t key, trim;
    uint8_t mac[8];
};
struct contract_update_layout {
    uint32_t entry, physical, clock;
    uint16_t key, reserved;
    uint32_t zero, private_info;
};
struct contract_clock_layout {
    uint32_t flags, oscillator, system;
    char tx[8], rx[8];
    uint32_t baud;
    char output[8], input[8];
    uint8_t reset_pin, reset_level, reset_hold, reserved;
};
_Static_assert(sizeof(struct contract_app_layout) == 24, "app ABI size");
_Static_assert(offsetof(struct contract_app_layout, physical) == 4, "app physical placement");
_Static_assert(offsetof(struct contract_app_layout, logical) == 8, "app logical placement");
_Static_assert(offsetof(struct contract_app_layout, mac) == 16, "MAC placement");
_Static_assert(sizeof(struct contract_update_layout) == 24, "updater ABI size");
_Static_assert(offsetof(struct contract_update_layout, private_info) == 20, "private pointer placement");
_Static_assert(sizeof(struct contract_clock_layout) == 52, "clock ABI size");
_Static_assert(offsetof(struct contract_clock_layout, reset_pin) == 48, "reset placement");
void contract_config(uint8_t clock[52], uint8_t memory[60], const struct core_config *);
void contract_app(uint8_t out[24], uint32_t physical, uint16_t key, uint16_t trim, const uint8_t mac[8]);
void contract_updater(uint8_t out[24], const struct core_app *, uint16_t key, uint32_t clock, uint32_t private_info);
bool contract_consume_bypass(uint8_t retained[8]);
/* 1: original RAM request; 2: scan flash; 0: normal app path (UART may apply). */
unsigned contract_update_choice(const uint8_t request[80], bool mounted);
#endif
