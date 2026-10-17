/* SPDX-License-Identifier: MIT */
#include "loader.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FLASH_SIZE 0x100000u
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static struct loader_state loader;
static uint8_t flash_bytes[FLASH_SIZE], sent_bytes[FLASH_SIZE + 16], receive_data[65536];
static uint8_t command[31], answer[16];
static uint32_t sent_size, receive_size, receive_pos, sent_calls, receive_calls;
static uint32_t program_count, erase_count, mutation_after_receive, read_count, start_count, finish_count;
static uint32_t program_address[32], program_size[32], erase_address, erase_size;
static uint32_t pending_address, pending_size;
static uint8_t *pending_pointer;
static bool pending, read_failure, program_failure, erase_failure, init_success;
static uint32_t screen_init_count, screen_row_count, screen_progress_count, screen_board, screen_row;
static uint16_t screen_fg, screen_bg, screen_fill;
static uint8_t screen_text[20];
static unsigned screen_text_size, screen_percent, tests, fixture_groups;
struct event {
    char kind;
    uint32_t address, size;
    const void *pointer;
};
static struct event events[10000];
static unsigned event_count;
static void event(char kind, uint32_t address, uint32_t size, const void *pointer)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count].kind = kind;
    events[event_count].address = address;
    events[event_count].size = size;
    events[event_count++].pointer = pointer;
}

static int send_callback(const void *data, uint32_t size)
{
    CHECK(size && size <= 512);
    CHECK(size <= sizeof(sent_bytes) - sent_size);
    if (core_be16(command + 15) != 0xfd05)
        CHECK(data == answer);
    /* §10.1 ROM buffer. */
    if (pending)
        CHECK(data != pending_pointer);
    memcpy(sent_bytes + sent_size, data, size);
    sent_size += size;
    ++sent_calls;
    event('S', 0, size, data);
    return -123;
    /* Return convention intentionally unspecified. */
}

static int receive_callback(void *data, uint32_t size)
{
    CHECK(size && size <= 512);
    CHECK(size <= receive_size - receive_pos);
    memcpy(data, receive_data + receive_pos, size);
    receive_pos += size;
    ++receive_calls;
    event('R', 0, size, data);
    return -123;
}

static bool read_callback(void *ctx, uint32_t address, void *dst, uint32_t size)
{
    CHECK(ctx == flash_bytes);
    CHECK(core_range(address, size, FLASH_SIZE));
    CHECK(!pending);
    ++read_count;
    if (read_failure)
        return false;
    memcpy(dst, flash_bytes + address, size);
    return true;
}

static bool read_start(uint32_t address, void *dst, uint32_t size)
{
    CHECK(!pending);
    CHECK(size && size <= 4096);
    CHECK(core_range(address, size, FLASH_SIZE));
    if (read_failure)
        return false;
    pending = true;
    pending_address = address;
    pending_size = size;
    pending_pointer = dst;
    ++start_count;
    event('A', address, size, dst);
    return true;
}

static bool read_finish(void)
{
    CHECK(pending);
    memcpy(pending_pointer, flash_bytes + pending_address, pending_size);
    event('F', pending_address, pending_size, pending_pointer);
    pending = false;
    ++finish_count;
    return true;
}

static bool program_callback(void *ctx, uint32_t address, const void *src, uint32_t size)
{
    const uint8_t *bytes = src;
    uint32_t i;
    CHECK(ctx == flash_bytes);
    CHECK(core_range(address, size, FLASH_SIZE));
    CHECK(size && size <= 256 - (address & 255));
    CHECK(receive_pos == receive_size);
    CHECK(program_count < 32);
    program_address[program_count] = address;
    program_size[program_count++] = size;
    mutation_after_receive = receive_pos;
    if (program_failure)
        return false;
    for (i = 0; i < size; ++i) {
        CHECK((flash_bytes[address + i] & bytes[i]) == bytes[i]);
        flash_bytes[address + i] &= bytes[i];
    }
    event('P', address, size, src);
    return true;
}

static bool erase_callback(void *ctx, uint32_t address, uint32_t size)
{
    CHECK(ctx == flash_bytes);
    CHECK(size == 4096 || size == 65536);
    CHECK(!(address & (size - 1)));
    CHECK(core_range(address, size, FLASH_SIZE));
    ++erase_count;
    erase_address = address;
    erase_size = size;
    if (erase_failure)
        return false;
    memset(flash_bytes + address, 255, size);
    event('E', address, size, NULL);
    return true;
}

static uint32_t jedec_callback(void) {
    return 0xeb6014;
}

static uint16_t key_callback(void) {
    return 0x980f;
}

static void uid_callback(uint8_t uid[8])
{
    static const uint8_t expected[8] = {
        0x81, 0, 0x23, 0x45, 0x67, 0x89, 0xab, 0xff
    };
    memcpy(uid, expected, 8);
}

static bool init_callback(unsigned board)
{
    CHECK(board == 1 || board == 2);
    ++screen_init_count;
    screen_board = board;
    return init_success;
}

static void row_callback(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned size)
{
    CHECK(row <= 14 && size <= 20);
    screen_row = row;
    screen_fg = fg;
    screen_bg = bg;
    screen_text_size = size;
    memcpy(screen_text, text, size);
    ++screen_row_count;
}

static void progress_callback(unsigned percent, uint16_t color)
{
    CHECK(percent <= 100);
    screen_percent = percent;
    screen_fill = color;
    ++screen_progress_count;
}

static struct loader_services services = {
    {
        {
            flash_bytes, FLASH_SIZE, read_callback
        }, program_callback, erase_callback
    },
    send_callback, receive_callback, read_start, read_finish, jedec_callback, key_callback,
    uid_callback, init_callback, row_callback, progress_callback, 0x01c20000, 96
};
static void reset_transfer(void)
{
    CHECK(!pending);
    sent_size = receive_size = receive_pos = sent_calls = receive_calls = 0;
    program_count = erase_count = mutation_after_receive = read_count = start_count = finish_count = 0;
    event_count = 0;
    read_failure = program_failure = erase_failure = false;
    memset(answer, 0xa5, sizeof(answer));
}

static void new_command(uint16_t op, bool in, uint32_t transfer)
{
    reset_transfer();
    memset(command, 0, sizeof(command));
    core_put_le32(command, 0x43425355);
    core_put_le32(command + 4, 0x12345678);
    core_put_le32(command + 8, transfer);
    command[12] = in ? 0x80 : 0;
    command[14] = 16;
    memset(command + 15, 255, 16);
    core_put_be16(command + 15, op);
}

static void dispatch(void)
{
    CHECK(loader_dispatch(&loader, command, answer) == 1);
    CHECK(!pending);
    CHECK(receive_pos == receive_size);
    if (core_be16(command + 15) == 0xfb04 || core_be16(command + 15) == 0xfc44) {
        CHECK(answer[0] == command[15] && answer[1] == command[16]);
        for (unsigned i = 2; i < 16; ++i)
            CHECK(!answer[i]);
    }
}

static void reply(uint16_t op)
{
    new_command(op, true, 16);
    dispatch();
    CHECK(sent_size == 16 && sent_calls == 1 && !receive_calls);
    CHECK(sent_bytes[0] == (uint8_t)(op >> 8) && sent_bytes[1] == (uint8_t)op);
}

static uint8_t take_status(void)
{
    reply(0xfc46);
    return sent_bytes[2];
}

static void expected_reply(const uint8_t expected[16]) {
    CHECK(!memcmp(sent_bytes, expected, 16));
}

static void test_identity(void)
{
    static const uint8_t key[16] = {
        0xfc, 9, 0, 0, 0, 0, 0x3c, 0x20, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const uint8_t device[16] = {
        0xfc, 10, 3, 0, 0x14, 0x60, 0xeb, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const uint8_t buffer[16] = {
        0xfc, 0x14, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const uint8_t identity[16] = {
        0xfc, 0x40, 'M', 'F', 'L', 'R', '1', '.', '2', 0, 1, 0xc2, 0, 0, 15, 96
    };
    reply(0xfc09);
    expected_reply(key);
    reply(0xfc0a);
    expected_reply(device);
    reply(0xfc14);
    expected_reply(buffer);
    reply(0xfc40);
    expected_reply(identity);
    reply(0xfc0b);
    CHECK(sent_bytes[2] == 0xeb && sent_bytes[3] == 0x60 && sent_bytes[4] == 0x14);
    reply(0xfc42);
    CHECK(sent_bytes[2] == 0x81 && sent_bytes[3] == 0 && sent_bytes[9] == 255);
    CHECK(!take_status());
    CHECK(!take_status());
    for (unsigned i = 0; i <= 16; ++i) {
        new_command(0xfc40, true, i);
        dispatch();
        CHECK(sent_size == i && sent_calls == (i ? 1u : 0u));
    }
    ++tests;
}

static void fixture(const char *name, uint32_t crc)
{
    char path[128];
    FILE *f;
    CHECK(snprintf(path, sizeof(path), "testdata/%s", name) > 0);
    f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        printf("SKIP: %s not present (loader raw CRC)\n", name);
        return;
    }
    CHECK(f);
    CHECK(fread(flash_bytes, 1, FLASH_SIZE, f) == FLASH_SIZE && fgetc(f) == EOF);
    CHECK(!fclose(f));
    new_command(0xfc41, true, 16);
    core_put_be32(command + 17, 0);
    core_put_be32(command + 21, FLASH_SIZE);
    dispatch();
    CHECK(sent_size == 16 && core_be32(sent_bytes + 2) == crc);
    CHECK(start_count == 256 && finish_count == 256);
    ++fixture_groups;
}

static void expect_overlap(void)
{
    bool saw_overlap = false;
    for (unsigned i = 0; i + 3 < event_count; ++i) {
        if (events[i].kind == 'F' && events[i + 1].kind == 'A' && events[i + 2].kind == 'S') {
            CHECK(events[i].pointer == events[i + 2].pointer);
            CHECK(events[i].pointer != events[i + 1].pointer);
            saw_overlap = true;
        }
    }
    CHECK(saw_overlap);
}

static void test_streaming(void)
{
    static const uint32_t sizes[] = {
        0, 1, 31, 32, 511, 512, 513, 4096, 4097, 16384, 65535
    };
    for (uint32_t i = 0; i < FLASH_SIZE; ++i)
        flash_bytes[i] = (uint8_t)((i * 47) ^ (i >> 8));
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        uint32_t n = sizes[k], address = 0x4003;
        new_command(0xfd05, true, n);
        core_put_be32(command + 17, address);
        core_put_be16(command + 21, (uint16_t)n);
        dispatch();
        CHECK(sent_size == n && !memcmp(sent_bytes, flash_bytes + address, n));
        CHECK(sent_calls == (n + 511) / 512);
        CHECK(start_count == (n + 4095) / 4096);
        if (n > 4096)
            expect_overlap();
        new_command(0xfc0e, true, 16);
        core_put_be32(command + 17, address);
        core_put_be16(command + 21, (uint16_t)n);
        dispatch();
        CHECK(sent_size == 16 && core_be16(sent_bytes + 2) == core_crc16(0, flash_bytes + address, n));
        new_command(0xfc41, true, 16);
        core_put_be32(command + 17, address);
        core_put_be32(command + 21, n);
        dispatch();
        CHECK(sent_size == 16 && core_be32(sent_bytes + 2) == (core_crc32(UINT32_MAX, flash_bytes + address, n) ^ UINT32_MAX));
        if (!n)
            CHECK(!start_count && !finish_count);
    }
    loader.io.read_start = NULL;
    loader.io.read_finish = NULL;
    new_command(0xfd05, true, 16384);
    core_put_be32(command + 17, 7);
    core_put_be16(command + 21, 16384);
    dispatch();
    CHECK(!start_count && read_count == 4 && sent_size == 16384 && !memcmp(sent_bytes, flash_bytes + 7, 16384));
    loader.io.read_start = read_start;
    loader.io.read_finish = read_finish;
    fixture("fm1-stock.bin", 0xf98496a0);
    fixture("smk37-stock.bin", 0x8e57bc96);
    fixture("smk37-snoop37.bin", 0x6475373c);
    fixture("smk37-baudgirl.bin", 0x1eabfb0a);
    ++tests;
}

static void prepare_write(uint32_t address, uint32_t length)
{
    new_command(0xfb04, false, length);
    receive_size = length;
    CHECK(length <= sizeof(receive_data));
    for (uint32_t i = 0; i < length; ++i)
        receive_data[i] = (uint8_t)(i * 7 + 19);
    core_put_be32(command + 17, address);
    core_put_be16(command + 21, (uint16_t)length);
    command[23] = 0;
    core_put_le16(command + 24, core_crc16(0, receive_data, length));
}

static void test_programming(void)
{
    static const uint32_t lengths[] = {
        0, 1, 255, 256, 257, 4096
    };
    for (unsigned alignment = 0; alignment < 2; ++alignment) {
        for (unsigned j = 0; j < sizeof(lengths) / sizeof(lengths[0]); ++j) {
            uint32_t n = lengths[j], address = 0x4000 + alignment * 13, done = 0;
            memset(flash_bytes + 0x3000, 255, 0x4000);
            prepare_write(address, n);
            dispatch();
            CHECK(!sent_size && !erase_count && receive_pos == n && !memcmp(flash_bytes + address, receive_data, n));
            CHECK(mutation_after_receive == n || !n);
            for (unsigned i = 0; i < program_count; ++i) {
                CHECK(program_address[i] == address + done);
                CHECK(program_size[i] == ((n - done < 256 - ((address + done) & 255)) ? n - done : 256 - ((address + done) & 255)));
                done += program_size[i];
            }
            CHECK(done == n);
            CHECK(flash_bytes[address - 1] == 255 && flash_bytes[address + n] == 255);
            CHECK(!take_status());
        }
    }
    /* Exact mixed-endian ASCII vector from §10.2. */
    prepare_write(0x4000, 9);
    memcpy(receive_data, "123456789", 9);
    core_put_le16(command + 24, 0x31c3);
    static const uint8_t write_cdb[16] = {
        0xfb, 4, 0, 0, 0x40, 0, 0, 9, 0, 0xc3, 0x31, 255, 255, 255, 255, 255
    };
    CHECK(!memcmp(command + 15, write_cdb, 16));
    memset(flash_bytes + 0x4000, 255, 256);
    dispatch();
    CHECK(!memcmp(flash_bytes + 0x4000, "123456789", 9));
    prepare_write(0x4000, 4097);
    dispatch();
    CHECK(!program_count && !erase_count && receive_pos == 4097);
    CHECK(loader.error == 1);
    reply(0xfc14);
    CHECK(loader.error == 1);
    CHECK(take_status() == 1);
    CHECK(!take_status());
    prepare_write(0x4000, 65535);
    dispatch();
    CHECK(!program_count && receive_calls == 128);
    CHECK(take_status() == 1);
    prepare_write(0x4000, 4096);
    command[24] ^= 1;
    dispatch();
    CHECK(!program_count && !erase_count && loader.error == 2);
    reply(0xfc40);
    CHECK(loader.error == 2);
    CHECK(take_status() == 2);
    CHECK(!take_status());
    prepare_write(0x4000, 256);
    command[24] ^= 1;
    dispatch();
    CHECK(loader.error == 2);
    prepare_write(0x6000, 256);
    memset(flash_bytes + 0x6000, 255, 256);
    dispatch();
    CHECK(loader.error == 2 && program_count == 1);
    CHECK(take_status() == 2);
    ++tests;
}

static void test_rejections(void)
{
    uint16_t ops[] = {
        0xfb00, 0xfb01
    };
    for (unsigned i = 0; i < 2; ++i) {
        new_command(ops[i], true, 16);
        core_put_be32(command + 17, 1);
        dispatch();
        CHECK(!erase_count);
        CHECK(take_status() == 3);
        new_command(ops[i], true, 16);
        core_put_be32(command + 17, FLASH_SIZE);
        dispatch();
        CHECK(!erase_count);
        CHECK(take_status() == 3);
        new_command(ops[i], true, 16);
        core_put_be32(command + 17, 0x10000);
        dispatch();
        CHECK(erase_count == 1);
        CHECK(erase_address == 0x10000 && erase_size == (i ? 4096u : 65536u));
        CHECK(sent_size == 16);
        CHECK(!take_status());
    }
    prepare_write(FLASH_SIZE - 1, 2);
    dispatch();
    CHECK(!program_count);
    CHECK(take_status() == 3);
    prepare_write(UINT32_MAX, 2);
    dispatch();
    CHECK(!program_count);
    CHECK(take_status() == 3);
    new_command(0xfd05, true, 2);
    core_put_be32(command + 17, UINT32_MAX);
    core_put_be16(command + 21, 2);
    dispatch();
    CHECK(!start_count && !read_count);
    CHECK(take_status() == 3);
    new_command(0xfc41, true, 16);
    core_put_be32(command + 17, 0x4000);
    core_put_be32(command + 21, UINT32_MAX);
    dispatch();
    CHECK(!start_count);
    CHECK(take_status() == 3);
    new_command(0xfc0e, true, 16);
    core_put_be32(command + 17, FLASH_SIZE);
    core_put_be16(command + 21, 1);
    dispatch();
    CHECK(!start_count);
    CHECK(take_status() == 3);
    prepare_write(0x4000, 8);
    core_put_be16(command + 21, 7);
    dispatch();
    CHECK(!program_count && receive_pos == 8);
    CHECK(take_status() == 7);
    prepare_write(0x4000, 8);
    command[14] = 15;
    dispatch();
    CHECK(!program_count && receive_pos == 8);
    CHECK(take_status() == 7);
    new_command(0xfb04, true, 8);
    core_put_be16(command + 21, 8);
    dispatch();
    CHECK(!program_count && !receive_calls);
    CHECK(take_status() == 7);
    new_command(0xfd05, false, 8);
    receive_size = 8;
    core_put_be16(command + 21, 8);
    dispatch();
    CHECK(receive_pos == 8 && !start_count);
    CHECK(take_status() == 7);
    new_command(0xfd05, true, 8);
    core_put_be16(command + 21, 7);
    dispatch();
    CHECK(!start_count);
    CHECK(take_status() == 7);
    new_command(0xfc40, true, 17);
    dispatch();
    CHECK(sent_size == 16);
    CHECK(take_status() == 7);
    for (unsigned i = 0; i < 2; ++i) {
        uint16_t forbidden = i ? 0xfc12 : 0xfb02;
        new_command(forbidden, true, 16);
        dispatch();
        CHECK(!erase_count && !program_count);
        CHECK(take_status() == 5);
        new_command(forbidden, false, 65535);
        receive_size = 65535;
        dispatch();
        CHECK(receive_pos == 65535 && !erase_count && !program_count);
        CHECK(take_status() == 5);
    }
    for (unsigned i = 0; i < 4; ++i) {
        static const uint16_t deferred[] = {
            0xfb06, 0xfd07, 0xfb08, 0x1234
        };
        new_command(deferred[i], true, 16);
        CHECK(!loader_dispatch(&loader, command, answer));
        CHECK(!sent_calls && !receive_calls && !erase_count && !program_count);
        for (unsigned j = 0; j < 16; ++j)
            CHECK(answer[j] == 0xa5);
    }
    new_command(0xfd05, true, 8);
    core_put_be32(command + 17, 0);
    core_put_be16(command + 21, 8);
    read_failure = true;
    dispatch();
    CHECK(!sent_calls);
    CHECK(take_status() == 4);
    prepare_write(0x7000, 256);
    memset(flash_bytes + 0x7000, 255, 256);
    program_failure = true;
    dispatch();
    CHECK(take_status() == 4);
    new_command(0xfb01, true, 16);
    core_put_be32(command + 17, 0x7000);
    erase_failure = true;
    dispatch();
    CHECK(take_status() == 4);
    ++tests;
}

static void prepare_text(unsigned row, uint32_t size)
{
    new_command(0xfc44, false, size);
    receive_size = size;
    for (uint32_t i = 0; i < size; ++i)
        receive_data[i] = (uint8_t)('A' + i % 26);
    command[17] = (uint8_t)row;
    core_put_be16(command + 18, 0xf81f);
    core_put_be16(command + 20, 0x07e0);
    core_put_be16(command + 22, (uint16_t)size);
}

static void init_screen(unsigned board)
{
    new_command(0xfc43, true, 16);
    command[17] = (uint8_t)board;
    dispatch();
}

static void test_display(void)
{
    init_success = true;
    screen_init_count = screen_row_count = screen_progress_count = 0;
    CHECK(!loader.screen);
    prepare_text(0, 20);
    dispatch();
    CHECK(!screen_row_count && receive_pos == 20);
    init_screen(0);
    CHECK(!screen_init_count && !sent_bytes[2]);
    init_screen(255);
    CHECK(!screen_init_count && !sent_bytes[2]);
    init_screen(1);
    CHECK(screen_init_count == 1 && screen_board == 1 && loader.screen && sent_bytes[2] == 1);
    prepare_text(14, 20);
    dispatch();
    CHECK(screen_row_count == 1 && screen_row == 14 && screen_fg == 0xf81f && screen_bg == 0x07e0);
    CHECK(screen_text_size == 20 && !memcmp(screen_text, receive_data, 20));
    prepare_text(0, 0);
    dispatch();
    CHECK(screen_row_count == 2 && !receive_calls && screen_text_size == 0);
    prepare_text(15, 20);
    dispatch();
    CHECK(screen_row_count == 2 && receive_pos == 20);
    CHECK(take_status() == 6);
    prepare_text(2, 65535);
    dispatch();
    CHECK(screen_row_count == 2 && receive_pos == 65535);
    CHECK(take_status() == 6);
    prepare_text(2, 21);
    dispatch();
    CHECK(screen_row_count == 2 && receive_pos == 21);
    CHECK(take_status() == 6);
    prepare_text(2, 8);
    core_put_be16(command + 22, 7);
    dispatch();
    CHECK(screen_row_count == 2 && receive_pos == 8);
    CHECK(take_status() == 7);
    init_screen(2);
    CHECK(screen_init_count == 2 && screen_board == 2 && loader.screen);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned percent = i == 0 ? 0 : i == 1 ? 100 : 255;
        new_command(0xfc45, true, 16);
        command[17] = (uint8_t)percent;
        core_put_be16(command + 18, 0x1234);
        dispatch();
        CHECK(screen_percent == (percent > 100 ? 100 : percent) && screen_fill == 0x1234);
    }
    CHECK(screen_progress_count == 3);
    init_screen(9);
    CHECK(!loader.screen && screen_init_count == 2);
    reply(0xfc45);
    CHECK(screen_progress_count == 3);
    init_success = false;
    init_screen(1);
    CHECK(!loader.screen && !sent_bytes[2]);
    reply(0xfc40);
    CHECK(sent_bytes[14] == 15);
    ++tests;
}

static void test_reinstallation(void)
{
    memset(&loader, 0xa5, sizeof(loader));
    loader_init(&loader, &services);
    CHECK(!loader.error && !loader.screen && loader.io.nor.flash.ctx == flash_bytes);
    CHECK(loader.io.send == send_callback && loader.io.receive == receive_callback);
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned i = 0; i < 4096; ++i)
            CHECK(!loader.buffer[b][i]);
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t byte = (uint8_t)i;
        CHECK(loader.crc_table[i] == core_crc32(0, &byte, 1));
    }
    reply(0xfc40);
    CHECK(core_be32(sent_bytes + 10) == services.entry && sent_bytes[15] == 96);
    init_success = true;
    init_screen(2);
    loader.error = 2;
    struct loader_services other = services;
    other.entry = 0x01c28000;
    other.cpu_mhz = 0;
    loader_init(&loader, &other);
    CHECK(!loader.screen && !loader.error);
    reply(0xfc40);
    CHECK(core_be32(sent_bytes + 10) == other.entry && !sent_bytes[15]);
    loader_init(&loader, &services);
    ++tests;
}

int main(void)
{
    loader_init(&loader, &services);
    test_identity();
    test_streaming();
    test_programming();
    test_rejections();
    test_display();
    test_reinstallation();
    printf("loader: %u groups passed (ROM callbacks, raw NOR, every opcode, %u fixture CRCs)\n", tests, fixture_groups);
    return 0;
}
