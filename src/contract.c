/* SPDX-License-Identifier: MIT */
#include "contract.h"
#include <string.h>
void contract_config(uint8_t clock[52], uint8_t memory[60], const struct core_config *c) {
    memset(clock, 0, 52);
    memset(memory, 0, 60);
    core_put_le32(clock, ((uint32_t)c->high_current << 12) | ((uint32_t)c->one_pin << 13) | ((uint32_t)c->lsb_divider << 14));
    core_put_le32(clock+4, c->oscillator_hz);
    core_put_le32(clock+8, c->system_hz);
    clock[48] = c->reset_pin;
    clock[49] = c->reset_level;
    clock[50] = c->reset_hold;
    core_put_le32(memory+12, c->hsb_divider&15);
}

void contract_app(uint8_t out[24], uint32_t physical, uint16_t key, uint16_t trim, const uint8_t mac[8]) {
    core_put_le32(out, 0x01c7fe20);
    core_put_le32(out+4, physical);
    core_put_le32(out+8, 0x02000000);
    core_put_le16(out+12, key);
    core_put_le16(out+14, trim);
    memcpy(out+16, mac, 8);
}

void contract_updater(uint8_t out[24], const struct core_app *app, uint16_t key, uint32_t clock, uint32_t private_info) {
    memset(out, 0, 24);
    core_put_le32(out, app->entry);
    core_put_le32(out+4, app->base);
    core_put_le32(out+8, clock);
    core_put_le16(out+12, key);
    core_put_le32(out+20, private_info);
}

bool contract_consume_bypass(uint8_t retained[8]) {
    if (memcmp(retained, "BOOTANY", 8))
        return false;
    memset(retained, 0, 8);
    return true;
}

unsigned contract_update_choice(const uint8_t request[80], bool mounted) {
    return core_request_valid(request) ? 1:(mounted ? 0:2);
}
