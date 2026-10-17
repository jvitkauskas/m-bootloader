/* SPDX-License-Identifier: MIT */
/* Execute actual slot A/B entry and registered command handlers against the
 * same independent MMIO/NOR model as the complete boot simulation. */
#include <assert.h>
#include "loader.h"
#include "platform.h"
#include "boot_sim.h"
int loader_test_install_a(const uint32_t params[3]);
int loader_test_install_b(const uint32_t params[3]);
struct loader_state *loader_test_state_a(void);
struct loader_state *loader_test_state_b(void);
static uint32_t registration, params[3];
static uint8_t command[31], answer[16], output[65536], input[4096];
static unsigned output_size, input_size, input_position, callback_set;
static unsigned checks, installs;
static const char *current;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr,"%s: %s:%d: %s\n",current,__FILE__,__LINE__,#c); exit(1); } } while (0)
/* Required only if hal_reset's unused BOOT_SIM branch is linked by the host. */
void boot_sim_reset(void)
{
    assert(!"loader must never request reset");
    abort();
}

static int send_callback(const void *data, uint32_t size, unsigned set)
{
    sim_flush();
    CHECK(size && size <= 512 && size <= sizeof(output)-output_size);
    if (core_be16(command+15) != 0xfd05)
        CHECK(data == answer);
    memcpy(output+output_size, data, size);
    output_size += size;
    callback_set = set;
    return -123;
}

static int send_one(const void *data, uint32_t size)
{
    return send_callback(data, size, 1);
}

static int send_two(const void *data, uint32_t size)
{
    return send_callback(data, size, 2);
}

static int receive_callback(void *data, uint32_t size, unsigned set)
{
    CHECK(size && size <= 512 && size <= input_size-input_position);
    memcpy(data, input+input_position, size);
    input_position += size;
    callback_set = set;
    return -123;
}

static int receive_one(void *data, uint32_t size)
{
    return receive_callback(data, size, 1);
}

static int receive_two(void *data, uint32_t size)
{
    return receive_callback(data, size, 2);
}

static void new_command(uint16_t opcode, bool in, uint32_t count)
{
    memset(command, 0, sizeof(command));
    core_put_le32(command, 0x43425355);
    core_put_le32(command+8, count);
    command[12] = in ? 128:0;
    command[14] = 16;
    core_put_be16(command+15, opcode);
    memset(answer, 0xa5, sizeof(answer));
    output_size = input_size = input_position = callback_set = 0;
}

static void dispatch(void)
{
    int (*handler)(const uint8_t *, uint8_t *) = (void *)(uintptr_t)registration;
    CHECK(registration);
    CHECK(handler(command, answer) == 1);
    sim_flush();
    CHECK(!sim.err);
}

static void identity(uint32_t entry, unsigned set, unsigned mhz)
{
    new_command(0xfc40, true, 16);
    dispatch();
    CHECK(output_size == 16 && callback_set == set);
    CHECK(!memcmp(output+2, "MFLR" LOADER_VERSION, 7));
    CHECK(core_be32(output+10) == entry && output[14] == 15 && output[15] == mhz);
}

static void clean_state(struct loader_state *state, uint32_t entry, unsigned mhz)
{
    CHECK(!state->screen && !state->error);
    CHECK(state->io.entry == entry && state->io.cpu_mhz == mhz);
    CHECK(state->io.nor.flash.capacity == 0x100000);
    CHECK(state->io.nor.flash.read == flash_read);
    CHECK(state->io.nor.program == flash_program && state->io.nor.erase == flash_erase);
    CHECK(state->io.read_start == flash_dma_start && state->io.read_finish == flash_dma_finish);
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned n = 0; n < 4096; ++n)
            CHECK(!state->buffer[b][n]);
    for (unsigned n = 0; n < 256; ++n) {
        uint8_t byte = (uint8_t)n;
        CHECK(state->crc_table[n] == core_crc32(0, &byte, 1));
    }
}

static void install(bool a, unsigned set, uint8_t dirt)
{
    struct loader_state *state = a ? loader_test_state_a():loader_test_state_b();
    memset(state, dirt, sizeof(*state));
    params[0] = (uint32_t)(uintptr_t)(set == 1 ? send_one:send_two);
    params[1] = (uint32_t)(uintptr_t)(set == 1 ? receive_one:receive_two);
    params[2] = (uint32_t)(uintptr_t)&registration;
    registration = 0xdeadbeef;
    CHECK((a ? loader_test_install_a(params):loader_test_install_b(params)) == 0);
    sim_flush();
    CHECK(registration && registration != 0xdeadbeef);
    CHECK(!sim.err);
    clean_state(state, a ? 0x01c20000:0x01c28000, 96);
    identity(a ? 0x01c20000:0x01c28000, set, 96);
    ++installs;
}

static void status_and_read(unsigned set)
{
    new_command(0xfc46, true, 16);
    dispatch();
    CHECK(!output[2] && callback_set == set);
    new_command(0xfd05, true, 8193);
    core_put_be32(command+17, 0x123);
    core_put_be16(command+21, 8193);
    dispatch();
    CHECK(output_size == 8193 && callback_set == set);
    CHECK(!memcmp(output, sim_nor+0x123, 8193));
    new_command(0xfc41, true, 16);
    core_put_be32(command+17, 0x123);
    core_put_be32(command+21, 8193);
    dispatch();
    CHECK(core_be32(output+2) == (core_crc32(UINT32_MAX, sim_nor+0x123, 8193) ^ UINT32_MAX));
}

static void fresh_out_callback(unsigned set)
{
    memset(sim_nor+0x4000, 0xff, 4096);
    new_command(0xfb04, false, 513);
    input_size = 513;
    for (unsigned n = 0; n < input_size; ++n)
        input[n] = (uint8_t)(17*n+3);
    core_put_be32(command+17, 0x400d);
    core_put_be16(command+21, 513);
    core_put_le16(command+24, core_crc16(0, input, 513));
    dispatch();
    CHECK(!output_size && input_position == 513 && callback_set == set);
    CHECK(!memcmp(sim_nor+0x400d, input, 513));
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    current = "power-on";
    sim_reset();
    memset(sim_nor, 0xff, NOR_MAX);
    nor_size = NOR_MAX;
    for (unsigned n = 0; n < NOR_MAX; ++n)
        sim_nor[n] = (uint8_t)(n*47 ^ (n >> 8));
    /* B must retain ROM's watchdog policy across every installation. */
    sim_p33_set(0x80, 0x31);
    sim_p33_set(9, 0xa5);
    sim_p33_set(6, 0xf8);
    sim_p33_set(5, 4);
    current = "slot A dirty upload at ROM 48 MHz";
    install(true, 1, 0xa5);
    CHECK((sim_reg_get(0x10014)&255) == 0x40);
    CHECK((sim_reg_get(0x10008)&0x30700) == 0x100);
    status_and_read(1);
    fresh_out_callback(1);
    uint32_t first_handler = registration;
    /* Seed live dirty state and show a different slot's BSS clear leaves it. */
    struct loader_state *a = loader_test_state_a();
    a->error = 7;
    a->screen = 1;
    memset(a->buffer, 0x5a, sizeof(a->buffer));
    a->crc_table[17] ^= 0x12345678;
    struct loader_state keep_a = *a;
    current = "slot B after A, chip already 96 MHz";
    install(false, 2, 0x3c);
    CHECK(registration != first_handler);
    CHECK(!memcmp(a, &keep_a, sizeof(keep_a)));
    status_and_read(2);
    fresh_out_callback(2);
    struct loader_state *b = loader_test_state_b();
    b->error = 2;
    b->screen = 1;
    memset(b->buffer, 0xc3, sizeof(b->buffer));
    struct loader_state keep_b = *b;
    current = "slot A reinstalled with fresh callback set";
    install(true, 2, 0xff);
    CHECK(registration == first_handler);
    CHECK(!memcmp(b, &keep_b, sizeof(keep_b)));
    status_and_read(2);
    fresh_out_callback(2);
    current = "slot B reinstalled with old callback set";
    install(false, 1, 0x69);
    status_and_read(1);
    fresh_out_callback(1);
    current = "same slot B installed again without clock reset";
    install(false, 2, 0x7e);
    status_and_read(2);
    CHECK(sim_p33_get(0x80) == 0x31);
    CHECK(sim_p33_get(9) == 0xa5 && sim_p33_get(6) == 0xf8 && sim_p33_get(5) == 4);
    CHECK(!sim.pll_seen && !sim.ncmd && !sim.phy_writes);
    printf("loader lifecycle: %u dirty installations, %u checks, two slots/ROM handlers/96 MHz passed\n", installs, checks);
    return 0;
}
