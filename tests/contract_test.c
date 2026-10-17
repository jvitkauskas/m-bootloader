/* SPDX-License-Identifier: MIT */
#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static void test_app(void)
{
    uint8_t out[24], mac[8] = {
        0, 1, 2, 3, 4, 5, 0x34, 0x12
    };
    const uint8_t expected[24] = {
        0x20, 0xfe, 0xc7, 1, 0, 0x40, 0, 0, 0, 0, 0, 2, 0x0f, 0x98, 0xcd, 0xab, 0, 1, 2, 3, 4, 5, 0x34, 0x12
    };
    memset(out, 0xa5, sizeof(out));
    contract_app(out, 0x4000, 0x980f, 0xabcd, mac);
    CHECK(!memcmp(out, expected, sizeof(out)));
    memset(mac, 255, sizeof(mac));
    contract_app(out, 0x20000, 0xffff, 0, mac);
    CHECK(core_le32(out + 4) == 0x20000 && core_le32(out + 8) == 0x02000000);
    CHECK(!memcmp(out + 16, mac, 8));
}

static void test_config(void)
{
    struct core_config c;
    uint8_t clock[52], memory[60], expected_clock[52], expected_memory[60];
    core_config_defaults(&c);
    memset(clock, 0xa5, sizeof(clock));
    memset(memory, 0xa5, sizeof(memory));
    memset(expected_clock, 0, sizeof(expected_clock));
    memset(expected_memory, 0, sizeof(expected_memory));
    expected_clock[1] = 0x80;
    /* LSB encoding 2 at bits17:14; compatibility OSC tag0. */
    core_put_le32(expected_clock + 4, 24000000);
    core_put_le32(expected_clock + 8, 360000000);
    expected_clock[48] = 17;
    expected_clock[50] = 8;
    expected_memory[12] = 1;
    contract_config(clock, memory, &c);
    CHECK(!memcmp(clock, expected_clock, sizeof(clock)));
    CHECK(!memcmp(memory, expected_memory, sizeof(memory)));
    /* Applied default voltage codes11/3 must not leak into absent parsed metadata. */
    CHECK(!core_le32(memory + 52));
    c.high_current = 1;
    c.one_pin = 1;
    c.reset_pin = 127;
    c.reset_level = 1;
    c.reset_hold = 99;
    contract_config(clock, memory, &c);
    CHECK(core_le32(clock) == 0xb000);
    for (unsigned i = 12; i < 48; ++i)
        CHECK(!clock[i]);
    /* §5.3: empty pin strings. */
    CHECK(clock[48] == 127 && clock[49] == 1 && clock[50] == 99 && !clock[51]);
    CHECK(!core_le32(memory + 52));
    for (unsigned i = 0; i < sizeof(memory); ++i)
        if (i != 12)
            CHECK(memory[i] == 0);
    CHECK(!core_le32(clock + 28));
    CHECK(!(core_le32(clock) & 0x0f00));
}

static void make_request(uint8_t request[80])
{
    memset(request, 0, 80);
    core_put_le16(request + 2, 0x5a0d);
    core_put_le16(request + 4, 0x5a01);
    core_put_le16(request + 6, 0x5441);
    memcpy(request + 8, "ota-test", 9);
    for (unsigned i = 0; i < 32; ++i)
        request[40 + i] = (uint8_t)(i * 7 + 1);
    core_put_le32(request + 72, 0xb9000);
    core_put_le16(request, core_crc16(0, request + 2, 78));
    CHECK(core_request_valid(request));
}

static void test_updater(void)
{
    struct core_app app;
    uint8_t out[24], retained[224], saved[80];
    memset(&app, 0, sizeof(app));
    app.base = 0x4000;
    app.entry = 0x02000120;
    make_request(retained + 8);
    memcpy(saved, retained + 8, 80);
    contract_updater(out, &app, 0x980f, 0x01c06000, 0x01c7fdb0);
    const uint8_t expected[24] = {
        0x20, 1, 0, 2, 0, 0x40, 0, 0, 0, 0x60, 0xc0, 1, 0x0f, 0x98, 0, 0, 0, 0, 0, 0, 0xb0, 0xfd, 0xc7, 1
    };
    CHECK(!memcmp(out, expected, 24));
    CHECK(!memcmp(retained + 8, saved, 80));
    app.base = app.entry = 0;
    app.table_base = 0x4000;
    app.metadata_valid = 1;
    contract_updater(out, &app, 0x980f, 0x01c06000, 0x01c06100);
    CHECK(!core_le32(out) && !core_le32(out + 4));
    CHECK(core_le32(out + 20) == 0x01c06100);
    for (unsigned i = 14; i < 20; ++i)
        CHECK(!out[i]);
}

static void test_retained(void)
{
    uint8_t retained[224], original[224];
    unsigned choice;
    memset(retained, 0x81, sizeof(retained));
    make_request(retained + 8);
    memcpy(retained, "BOOTANY", 8);
    memcpy(original, retained, sizeof(retained));
    CHECK(contract_consume_bypass(retained));
    CHECK(!contract_consume_bypass(retained));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!retained[i]);
    CHECK(!memcmp(retained + 8, original + 8, sizeof(retained) - 8));
    CHECK(core_request_valid(retained + 8));
    CHECK(contract_update_choice(retained + 8, true) == 1);
    CHECK(contract_update_choice(retained + 8, false) == 1);
    retained[80] ^= 1;
    CHECK(contract_update_choice(retained + 8, true) == 0);
    CHECK(contract_update_choice(retained + 8, false) == 2);
    memcpy(retained, "BOOTANY!", 8);
    memcpy(original, retained, sizeof(retained));
    CHECK(!contract_consume_bypass(retained) && !memcmp(retained, original, sizeof(retained)));
    /* §6.2 applies only after no request and a successful application mount. */
    memset(retained, 0, sizeof(retained));
    memcpy(retained, "UARTUPDATE", 11);
    CHECK(core_uart_request(retained));
    choice = contract_update_choice(retained + 8, true);
    CHECK(choice == 0 && core_uart_request(retained));
    CHECK(contract_update_choice(retained + 8, false) == 2);
    memset(retained, 0, 4);
    CHECK(!core_uart_request(retained));
    CHECK(!memcmp(retained + 4, "UPDATE", 7));
    memset(retained, 0, sizeof(retained));
    memcpy(retained, "UART_UPDATE_CUSTOM", 19);
    CHECK(core_uart_request(retained));
    retained[18] = 'x';
    CHECK(!core_uart_request(retained));
    retained[18] = 0;
    memset(retained, 0, 4);
    CHECK(!core_uart_request(retained));
    make_request(retained + 8);
    CHECK(contract_update_choice(retained + 8, true) == 1);
    CHECK(contract_update_choice(retained + 8, false) == 1);
}

int main(void)
{
    test_app();
    test_config();
    test_updater();
    test_retained();
    puts("contract: app/updater/config byte vectors, original private pointer, one-shot and §6.2 priority passed");
    return 0;
}
