/* SPDX-License-Identifier: MIT */
#include "core.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAPACITY 0x100000u
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint8_t image[CAPACITY], saved[CAPACITY], workspace[CORE_WORK_SIZE], updater_output[CORE_UPDATE_MAX];
static unsigned assertions, fixture_groups;
struct model {
    unsigned programs, erases, operations, fail_at, reads;
    bool bad_read, bad_erase;
    uint32_t first_program, last_program, erase_address[256], erase_size[256];
};
static struct model model;
static bool model_read(void *ctx, uint32_t address, void *dst, uint32_t size)
{
    struct model *m = ctx;
    ++m->reads;
    CHECK(core_range(address, size, CAPACITY));
    if (m->bad_read)
        return false;
    memcpy(dst, image + address, size);
    return true;
}

static bool model_program(void *ctx, uint32_t address, const void *src, uint32_t size)
{
    struct model *m = ctx;
    const uint8_t *bytes = src;
    uint32_t i;
    CHECK(size && size <= 256 && size <= 256 - (address & 255));
    CHECK(core_range(address, size, CAPACITY));
    if (++m->operations == m->fail_at)
        return false;
    if (!m->programs)
        m->first_program = address;
    m->last_program = address;
    ++m->programs;
    for (i = 0; i < size; ++i) {
        CHECK((image[address + i] & bytes[i]) == bytes[i]);
        image[address + i] &= bytes[i];
    }
    return true;
}

static bool model_erase(void *ctx, uint32_t address, uint32_t size)
{
    struct model *m = ctx;
    CHECK(size == 4096 || size == 65536);
    CHECK(!(address & (size - 1)));
    CHECK(core_range(address, size, CAPACITY));
    if (++m->operations == m->fail_at)
        return false;
    CHECK(m->erases < 256);
    m->erase_address[m->erases] = address;
    m->erase_size[m->erases++] = size;
    if (!m->bad_erase)
        memset(image + address, 255, size);
    return true;
}

static struct core_flash flash = {
    &model, CAPACITY, model_read
};
static struct core_nor nor = {
    {
        &model, CAPACITY, model_read
    }, model_program, model_erase
};
static void reset_model(void) {
    memset(&model, 0, sizeof(model));
}

static bool load_fixture(const char *name)
{
    static const char *missing[4];
    static unsigned missing_count;
    char path[128];
    FILE *file;
    CHECK(snprintf(path, sizeof(path), "testdata/%s", name) > 0);
    file = fopen(path, "rb");
    if (!file && errno == ENOENT) {
        unsigned i;
        for (i = 0; i < missing_count; ++i)
            if (strlen(missing[i]) == strlen(name) && !memcmp(missing[i], name, strlen(name)))
                return false;
        CHECK(missing_count < sizeof(missing) / sizeof(missing[0]));
        missing[missing_count++] = name;
        printf("SKIP: %s not present (core fixture groups)\n", name);
        return false;
    }
    CHECK(file != NULL);
    CHECK(fread(image, 1, sizeof(image), file) == sizeof(image));
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
    reset_model();
    return true;
}

static void expect_hex(const uint8_t *data, uint32_t size, const char *hex)
{
    uint32_t i;
    for (i = 0; i < size; ++i) {
        unsigned expected;
        CHECK(sscanf(hex + i * 2, "%2x", &expected) == 1);
        CHECK(data[i] == expected);
    }
}

static void test_primitives(void)
{
    uint8_t bytes[64], unaligned[9];
    uint16_t s;
    uint32_t r, index;
    CHECK(core_crc16(0, "123456789", 9) == 0x31c3);
    CHECK(core_crc16(core_crc16(0, "1234", 4), "56789", 5) == 0x31c3);
    CHECK((core_crc32(UINT32_MAX, "123456789", 9) ^ UINT32_MAX) == 0xcbf43926);
    r = core_crc32(UINT32_MAX, "1234", 4);
    CHECK((core_crc32(r, "56789", 5) ^ UINT32_MAX) == 0xcbf43926);
    memset(bytes, 0, sizeof(bytes));
    s = core_enc(0xffff, bytes, 11);
    core_enc(s, bytes + 11, 21);
    expect_hex(bytes, 32, "ffdf9f1f1f3e7cf8f0c1a367cebd5b970f1e3c78d183274e9c1913264c983060");
    core_enc(0xffff, bytes, 32);
    CHECK(!memcmp(bytes, bytes + 32, 32));
    memset(bytes, 0, sizeof(bytes));
    core_enc(0x980f, bytes, 32);
    expect_hex(bytes, 32, "0f3f7efcd9b264c8b162c48810012346ad5a950b162c79d3a66dfbf6ecf9d3a6");
    memset(bytes, 0, sizeof(bytes));
    index = 0;
    s = core_usb_begin(UINT32_MAX);
    s = core_usb_stream(s, &index, bytes, 7);
    core_usb_stream(s, &index, bytes + 7, 25);
    expect_hex(bytes, 32, "2fa481cc4ec15deaeea2b6a6298ad4f70384ebe1cf8a8a14bde6dd1cd0f8463b");
    index = 0;
    core_usb_stream(core_usb_begin(UINT32_MAX), &index, bytes, 32);
    CHECK(!memcmp(bytes, bytes + 32, 32));
    core_put_le16(unaligned + 1, 0xabcd);
    CHECK(core_le16(unaligned + 1) == 0xabcd);
    core_put_be16(unaligned + 1, 0xabcd);
    CHECK(core_be16(unaligned + 1) == 0xabcd);
    core_put_le32(unaligned + 1, 0x89abcdef);
    CHECK(core_le32(unaligned + 1) == 0x89abcdef);
    core_put_be32(unaligned + 1, 0x89abcdef);
    CHECK(core_be32(unaligned + 1) == 0x89abcdef);
    CHECK(core_range(CAPACITY, 0, CAPACITY));
    CHECK(!core_range(UINT32_MAX, 2, CAPACITY));
    CHECK(!core_range(CAPACITY, 1, CAPACITY));
    CHECK(!core_range(1, UINT32_MAX, CAPACITY));
    reset_model();
    CHECK(core_read(&flash, CAPACITY, bytes, 0));
    CHECK(!model.reads);
    CHECK(!core_read(&flash, CAPACITY, bytes, 1));
    CHECK(!model.reads);
    ++assertions;
}

static uint32_t add_config(uint8_t *blob, uint32_t pos, const char *key, const void *value, uint8_t size)
{
    size_t key_size = strlen(key) + 1;
    blob[pos++] = size;
    memcpy(blob + pos, key, key_size);
    pos += (uint32_t)key_size;
    memcpy(blob + pos, value, size);
    return pos + size;
}

static uint32_t add_number(uint8_t *blob, uint32_t pos, const char *key, uint32_t value)
{
    uint8_t raw[4];
    core_put_le32(raw, value);
    return add_config(blob, pos, key, raw, 4);
}

static uint32_t minimal_config(uint8_t *blob)
{
    uint32_t p = 34;
    memset(blob, 0, 512);
    p = add_config(blob, p, "OSC", "OSC0", 4);
    p = add_number(blob, p, "OSC_FREQ", 24000000);
    p = add_number(blob, p, "SYS_CLK", 360000000);
    p = add_number(blob, p, "HSB_DIV", 1);
    return add_number(blob, p, "LSB_DIV", 2);
}

static void test_config(void)
{
    uint8_t blob[512];
    uint32_t n = minimal_config(blob), good;
    struct core_config c;
    CHECK(core_config_parse(&c, blob, n));
    CHECK(c.oscillator_tag == 0 && c.oscillator_hz == 24000000);
    CHECK(c.width == 4 && c.spi_divider == 1 && c.hsb_divider == 1 && c.lsb_divider == 2);
    good = n;
    /* §5.3: dropped keys are ignored, including unsupported value encodings. */
    static const char *const dropped[] = {
        "PORT_INPUT", "PORT_OUTPUT", "ERR_PORT_OUTPUT",
        "BOOT_DELAY", "UTRX", "UTTX", "DVDD", "VDDIO", "DCDC14"
    };
    struct core_config expected = c;
    for (unsigned i = 0; i < sizeof(dropped) / sizeof(dropped[0]); ++i) {
        n = add_config(blob, good, dropped[i], "invalid, deliberately ignored", 29);
        CHECK(core_config_parse(&c, blob, n));
        CHECK(!memcmp(&c, &expected, sizeof(c)));
        n = add_number(blob, good, dropped[i], UINT32_MAX);
        CHECK(core_config_parse(&c, blob, n));
        CHECK(!memcmp(&c, &expected, sizeof(c)));
    }
    n = add_number(blob, good, "ENABLE_SDRAM", 1);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_number(blob, good, "LSB_DIV", UINT32_MAX);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_number(blob, good, "SYS_CLK", 0);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_number(blob, good, "OSC_HC_EN", 1);
    n = add_number(blob, n, "OSC_1PIN_EN", 1);
    CHECK(core_config_parse(&c, blob, n));
    CHECK(c.high_current == 1 && c.one_pin == 1);
    n = add_number(blob, good, "OSC_HC_EN", 2);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_number(blob, good, "OSC_1PIN_EN", 2);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_config(blob, good, "SYS_CLK", "x", 1);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_config(blob, good, "SPI", "2_3_0_0", 7);
    CHECK(!core_config_parse(&c, blob, n));
    n = add_config(blob, good, "RESET", "PB01_08_0", 9);
    CHECK(core_config_parse(&c, blob, n));
    CHECK(c.reset_pin == 17 && c.reset_hold == 8);
    n = add_config(blob, good, "UNKNOWN", "abc", 3);
    CHECK(core_config_parse(&c, blob, n));
    CHECK(!core_config_parse(&c, blob, n - 1));
    CHECK(!core_config_parse(&c, blob, 4097));
    memset(blob + good, 'X', 20);
    blob[good] = 4;
    CHECK(!core_config_parse(&c, blob, good + 20));
    CHECK(!core_config_parse(&c, blob, 34));
    ++assertions;
}

static void encode_record(uint32_t address, uint32_t relative, bool app, uint16_t key, const char *name, uint32_t data_address, uint32_t size, uint8_t attr, uint16_t crc)
{
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    core_put_le16(raw + 2, crc);
    core_put_le32(raw + 4, data_address);
    core_put_le32(raw + 8, size);
    raw[12] = attr;
    memcpy(raw + 16, name, strlen(name) > 16 ? 16 : strlen(name));
    core_put_le16(raw, core_crc16(0, raw + 2, 30));
    core_enc(app ? (uint16_t)(key ^ (uint16_t)(relative >> 2)) : 0xffff, raw, 32);
    memcpy(image + address, raw, 32);
}

static void synthetic_bank(uint32_t base, uint16_t key, uint8_t fill)
{
    uint8_t data[32];
    memset(data, fill, sizeof(data));
    encode_record(base, 0, true, key, "app_area_head", 0x02000120, 0x1000, 0x83, 0xffff);
    encode_record(base + 32, 32, true, key, "app.bin", 0x120, 32, 0x82, core_crc16(0, data, 32));
    core_enc((uint16_t)(key ^ (0x120 >> 2)), data, 32);
    memcpy(image + base + 0x120, data, 32);
}

static void synthetic_info(uint32_t address, uint16_t version, bool valid)
{
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    core_put_le16(raw + 6, version);
    core_put_le16(raw, core_crc16(0, raw + 2, 30));
    if (!valid)
        raw[0] ^= 1;
    core_enc(0xffff, raw, 32);
    memcpy(image + address, raw, 32);
}

static void test_banks(void)
{
    struct core_app app;
    uint8_t raw[32];
    memset(image, 255, sizeof(image));
    reset_model();
    memset(raw, 0, 32);
    core_put_le32(raw + 8, CAPACITY - 4096);
    raw[13] = 16;
    core_put_le16(raw, core_crc16(0, raw + 2, 30));
    core_enc(0xffff, raw, 32);
    memcpy(image, raw, 32);
    encode_record(32, 0, false, 0, "uboot.boot", 0xa0, 16, 0, 0);
    encode_record(64, 0, false, 0, "app_dir_head", 0x10000, 0x3000, 0x81, 0);
    encode_record(96, 0, false, 0, "app_dir_head2", 0x20000, 0x3020, 0x81, 0);
    synthetic_bank(0x10000, 0x980f, 1);
    synthetic_bank(0x20000, 0x980f, 2);
    synthetic_info(0x3000, 10, true);
    synthetic_info(0x3020, 11, true);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x20000);
    CHECK(core_app_check(&flash, &app, workspace) == CORE_APP_OK);
    synthetic_info(0x3020, 10, true);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x10000);
    synthetic_info(0x3020, 9, true);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x10000);
    synthetic_info(0x3020, 11, false);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x10000);
    synthetic_info(0x3000, 10, false);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x20000);
    image[0x20000] ^= 1;
    CHECK(!core_app_mount(&flash, 0x980f, &app) && !app.base);
    synthetic_info(0x3000, 10, true);
    synthetic_info(0x3020, 65535, true);
    synthetic_bank(0x20000, 0x980f, 3);
    CHECK(core_app_mount(&flash, 0x980f, &app) && app.base == 0x20000);
    encode_record(0x20020, 32, true, 0x980f, "app.bin", UINT32_MAX, 16, 0x82, 0);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(app.state == CORE_APP_INCOMPLETE);
    synthetic_bank(0x20000, 0x980f, 3);
    encode_record(0x20020, 32, true, 0x980f, "app.bin", 0x120, 0, 0x82, 0);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(app.state == CORE_APP_NOT_FOUND);
    synthetic_bank(0x20000, 0x980f, 3);
    encode_record(0x20000, 0, true, 0x980f, "app_area_head", 0x04000120, 0x1000, 0x83, 0);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    ++assertions;
}

static void test_lz4(void)
{
    uint8_t out[32], first[] = {
        0x40, 'A', 'B', 'C', 'D'
    };
    uint8_t history[] = {
        0x00, 4, 0, 0x10, '!'
    };
    uint8_t overlap[] = {
        0x10, 'x', 1, 0, 0x10, '!'
    };
    uint8_t invalid[] = {
        0x00, 0, 0
    }, truncated[] = {
        0xf0, 255
    };
    CHECK(core_lz4_block(first, sizeof(first), out, 0, 4, sizeof(out)));
    CHECK(core_lz4_block(history, sizeof(history), out, 4, 5, sizeof(out)));
    CHECK(!memcmp(out, "ABCDABCD!", 9));
    CHECK(core_lz4_block(overlap, sizeof(overlap), out, 0, 6, sizeof(out)));
    CHECK(!memcmp(out, "xxxxx!", 6));
    CHECK(!core_lz4_block(invalid, sizeof(invalid), out, 0, 4, sizeof(out)));
    CHECK(!core_lz4_block(truncated, sizeof(truncated), out, 0, 4, sizeof(out)));
    CHECK(!core_lz4_block(first, sizeof(first), out, 0, 3, sizeof(out)));
    CHECK(!core_lz4_block(first, sizeof(first), out, UINT32_MAX, 4, sizeof(out)));
    CHECK(!core_lz4_block(history, sizeof(history), out, 0, 5, sizeof(out)));
    ++assertions;
}

static bool wipe_expected(uint32_t address, uint32_t vm_start, uint32_t vm_end, uint32_t btif_start)
{
    return (address >= vm_start && address < vm_end) || (address >= btif_start && address < btif_start + 4096);
}

static void test_fixture(const char *name, uint32_t crc32, uint32_t app_length, uint16_t app_crc, uint32_t cfg_address, uint32_t vm_start, uint32_t vm_end, uint32_t btif, bool smk)
{
    struct core_header h;
    struct core_record config;
    struct core_config c;
    struct core_app app;
    struct core_wipe plan;
    uint16_t key;
    uint32_t i, cursor = 0, address, size, erased = 0;
    if (!load_fixture(name))
        return;
    memcpy(saved, image, sizeof(image));
    CHECK((core_crc32(UINT32_MAX, image, sizeof(image)) ^ UINT32_MAX) == crc32);
    for (i = 0xa0; i < 0x38d0; ++i)
        CHECK(image[i] == 255);
    CHECK(core_header_read(&flash, &h));
    CHECK(core_le16(h.bytes) == 0xdca1 && h.alignment == 16);
    CHECK(core_top_find(&flash, "isd_config.ini", &config));
    CHECK(config.address == 0x38d0 && config.length == 699);
    CHECK(core_read(&flash, config.address, workspace, config.length));
    CHECK(core_crc16(0, workspace, config.length) == config.data_crc);
    CHECK(core_chipkey(workspace, &key) && key == 0x980f);
    CHECK(core_config_parse(&c, workspace, config.length));
    CHECK(c.width == 4 && c.spi_divider == 1 && c.read_mode == 4 && !c.oscillator_tag);
    CHECK(core_app_mount(&flash, key, &app));
    CHECK(app.base == 0x4000 && app.entry == 0x02000120);
    CHECK(app.file_count == 2 && app.app_length == app_length && app.files[0].data_crc == app_crc);
    CHECK(app.files[1].address == cfg_address && app.files[1].length == 383 && app.files[1].data_crc == 0x2e75);
    CHECK(core_app_check(&flash, &app, workspace) == CORE_APP_OK);
    CHECK(core_app_read(&flash, app.base, key, 0x11f, workspace, 37));
    CHECK(!core_app_read(&flash, app.base, key, UINT32_MAX, workspace, 2));
    image[app.base + cfg_address + 12] ^= 1;
    CHECK(core_app_check(&flash, &app, workspace) == CORE_APP_BAD_CRC);
    image[app.base + cfg_address + 12] ^= 1;
    memset(image + app.base + app.files[0].address + app_length - 32, 255, 32);
    CHECK(core_app_check(&flash, &app, workspace) == CORE_APP_INCOMPLETE);
    memcpy(image, saved, sizeof(image));
    CHECK(core_app_mount(&flash, key, &app));
    CHECK(core_wipe_plan(&flash, &app, smk, &plan));
    CHECK(plan.count == 2);
    CHECK(plan.start[0] == vm_start || plan.start[1] == vm_start);
    while (core_wipe_next(&plan, CAPACITY, smk, &cursor, &address, &size)) {
        CHECK(wipe_expected(address, vm_start, vm_end, btif));
        for (i = 0; i < size; i += 4096)
            CHECK(wipe_expected(address + i, vm_start, vm_end, btif));
        erased += size;
    }
    CHECK(erased == vm_end - vm_start + 4096);
    CHECK(core_wipe_execute(&nor, &plan, smk, workspace));
    CHECK(!model.programs);
    for (i = 0; i < CAPACITY; ++i)
        CHECK(image[i] == (wipe_expected(i, vm_start, vm_end, btif) ? 255 : saved[i]));
    memcpy(image, saved, sizeof(image));
    reset_model();
    model.bad_erase = true;
    memset(image + plan.start[0], 0, 4096);
    CHECK(!core_wipe_execute(&nor, &plan, smk, workspace));
    memcpy(image, saved, sizeof(image));
    reset_model();
    model.fail_at = 2;
    CHECK(!core_wipe_execute(&nor, &plan, smk, workspace));
    reset_model();
    CHECK(core_wipe_execute(&nor, &plan, smk, workspace));
    for (i = 0; i < CAPACITY; ++i)
        CHECK(image[i] == (wipe_expected(i, vm_start, vm_end, btif) ? 255 : saved[i]));
    memcpy(image, saved, sizeof(image));
    reset_model();
    ++assertions;
    ++fixture_groups;
}

static void test_fixture_updater(void)
{
    uint8_t request[80], first[80];
    uint32_t count;
    struct core_updater u;
    if (!load_fixture("smk37-stock.bin"))
        return;
    memcpy(saved, image, sizeof(image));
    CHECK(core_request_scan(&flash, 16, request, &count) && count >= 1);
    CHECK(core_request_valid(request));
    CHECK(core_le16(request + 2) == 0x5a0d && core_le32(request + 72) == 0xb9000);
    memcpy(first, request, 80);
    CHECK(core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    CHECK(u.load == CORE_UPDATE_LOAD && u.size == 23324 && u.crc == 0xb295 && u.stream_end == 0xb9000 + 19969);
    CHECK(core_crc16(0, updater_output, u.size) == 0xb295);
    request[3] ^= 1;
    CHECK(!core_request_valid(request));
    request[3] ^= 1;
    memset(request + 2, 0, 2);
    core_put_le16(request, core_crc16(0, request + 2, 78));
    CHECK(!core_request_valid(request));
    memcpy(request, first, 80);
    image[0xb9000] ^= 1;
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    memcpy(image, saved, sizeof(image));
    image[0xb9020] ^= 1;
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    memcpy(image, saved, sizeof(image));
    core_put_le32(image + 0xb9040, UINT32_MAX);
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    memcpy(image, saved, sizeof(image));
    core_put_le32(image + 0xb9044, UINT32_MAX);
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    memcpy(image, saved, sizeof(image));
    core_put_le32(image + 0xb9028, CORE_UPDATE_LOAD + 4);
    core_put_le16(image + 0xb9020, core_crc16(0, image + 0xb9022, 30));
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    memcpy(image, saved, sizeof(image));
    CHECK(!core_updater_decode(&flash, request, updater_output, 10, workspace, &u));
    core_put_le32(request + 72, UINT32_MAX);
    core_put_le16(request, core_crc16(0, request + 2, 78));
    CHECK(!core_updater_decode(&flash, request, updater_output, sizeof(updater_output), workspace, &u));
    ++assertions;
}

static void test_requests(void)
{
    uint8_t request[80], first[80] = {0}, uart[19];
    uint32_t count;
    reset_model();
    core_put_le16(first + 2, 0x5a0d);
    core_put_le16(first + 6, 0x5441);
    core_put_le32(first + 72, 0xb9000);
    core_put_le16(first, core_crc16(0, first + 2, 78));
    CHECK(core_request_valid(first));
    memcpy(request, first, 80);
    request[3] ^= 1;
    CHECK(!core_request_valid(request));
    core_put_le16(request + 2, 0);
    core_put_le16(request, core_crc16(0, request + 2, 78));
    CHECK(!core_request_valid(request));
    memset(image, 255, sizeof(image));
    memcpy(image + 0xfef00, first, 80);
    CHECK(core_request_scan(&flash, 16, request, NULL));
    CHECK(!memcmp(request, first, 80));
    memcpy(image + 0xfdf00, first, 80);
    /* Older valid record must not win. */
    core_put_le16(image + 0xfef02, 0);
    core_put_le16(image + 0xfef00, core_crc16(0, image + 0xfef02, 78));
    CHECK(!core_request_scan(&flash, 16, request, NULL));
    CHECK(!core_le16(request + 2));
    CHECK(!core_request_scan(&flash, 16, request, &count));
    CHECK(count == 2);
    CHECK(!core_le16(request + 2));
    memset(image + 0xfdf00, 255, 80);
    memset(image + 0xfef00, 255, 80);
    memcpy(image + 0xfff00, first, 80);
    CHECK(!core_request_scan(&flash, 16, request, NULL));
    memset(image, 255, sizeof(image));
    memcpy(image + 0xfd00, first, 80);
    CHECK(core_request_scan(&flash, 1, request, NULL));
    memset(uart, 0, sizeof(uart));
    memcpy(uart, "UARTUPDATE", 11);
    CHECK(core_uart_request(uart));
    uart[10] = '!';
    CHECK(!core_uart_request(uart));
    memcpy(uart, "UART_UPDATE_CUSTOM", 19);
    CHECK(core_uart_request(uart));
    memset(uart, 0, 4);
    CHECK(!core_uart_request(uart));
    ++assertions;
}

static void test_mount_header_refresh(void)
{
    struct core_header initial, fresh;
    struct core_app app;
    uint8_t changed[32], encoded[32];
    if (!load_fixture("fm1-stock.bin"))
        return;
    CHECK(core_header_read(&flash, &initial));
    memset(&fresh, 0xa5, sizeof(fresh));
    CHECK(core_app_mount_header(&flash, 0x980f, &app, &fresh));
    CHECK(!memcmp(fresh.bytes, initial.bytes, 32));
    CHECK(fresh.alignment == initial.alignment);
    CHECK(fresh.filesystem_size == initial.filesystem_size);
    memcpy(changed, initial.bytes, 32);
    changed[4] ^= 0x5a;
    changed[13] = 8;
    core_put_le16(changed, core_crc16(0, changed + 2, 30));
    memcpy(encoded, changed, 32);
    core_enc(0xffff, encoded, 32);
    memcpy(image, encoded, 32);
    CHECK(core_app_mount_header(&flash, 0x980f, &app, &fresh));
    CHECK(!memcmp(fresh.bytes, changed, 32));
    CHECK(fresh.alignment == 8);
    CHECK(memcmp(fresh.bytes, initial.bytes, 32) != 0);
    CHECK(app.base == 0x4000 && app.entry == 0x02000120);
    image[0] ^= 1;
    CHECK(!core_app_mount_header(&flash, 0x980f, &app, &fresh));
    CHECK(fresh.alignment == 0);
    CHECK(!app.base && !app.entry && !app.metadata_valid);
    ++assertions;
}

static void test_wipe_safety(void)
{
    struct core_app app;
    struct core_wipe plan;
    uint32_t cursor, address, size, i, total;
    if (!load_fixture("smk37-stock.bin"))
        return;
    CHECK(core_app_mount(&flash, 0x980f, &app));
    app.regions[0].address = 0;
    app.regions[0].length = CAPACITY;
    memcpy(app.regions[0].name, "VM", 3);
    app.regions[0].attribute = 0x12;
    app.region_count = 1;
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    app.file_count = 0;
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.start[0] == 0x4000 && plan.end[0] == 0xff000);
    memset(&plan, 0, sizeof(plan));
    plan.count = 1;
    plan.end[0] = CAPACITY;
    cursor = 0;
    total = 0;
    while (core_wipe_next(&plan, CAPACITY, true, &cursor, &address, &size)) {
        for (i = 0; i < size; i += 4096)
            CHECK(address + i >= 0x4000 && address + i < 0xff000 && address + i != 0xc1000);
        total += size;
    }
    CHECK(total == CAPACITY - 0x6000);
    app.regions[0].address = 0xc0000;
    app.regions[0].length = 0x3000;
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    cursor = 0;
    CHECK(core_wipe_next(&plan, CAPACITY, true, &cursor, &address, &size));
    CHECK(address == 0xc0000 && size == 0x1000);
    CHECK(core_wipe_next(&plan, CAPACITY, true, &cursor, &address, &size));
    CHECK(address == 0xc2000 && size == 0x1000);
    CHECK(!core_wipe_next(&plan, CAPACITY, true, &cursor, &address, &size));
    memcpy(saved, image, sizeof(image));
    CHECK(core_wipe_execute(&nor, &plan, true, workspace));
    for (i = 0; i < CAPACITY; ++i)
        CHECK(image[i] == ((i >= 0xc0000 && i < 0xc1000) || (i >= 0xc2000 && i < 0xc3000) ? 255 : saved[i]));
    CHECK(!model.programs);
    app.regions[0].address = 0xfe000;
    app.regions[0].length = 0x2000;
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.end[0] == 0xff000);
    app.regions[0].address = 0x1001;
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    app.regions[0].address = 0x1000;
    app.regions[0].length = UINT32_MAX;
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    app.regions[0].address = 0x8000;
    app.regions[0].length = 0x1000;
    app.regions[0].attribute = 0x82;
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    app.regions[0].attribute = 0x12;
    app.regions[1] = app.regions[0];
    app.region_count = 2;
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    memcpy(app.regions[1].name, "BTIF", 5);
    app.regions[1].address = 0xf0000;
    app.regions[0].address = 0x8001;
    /* Invalid VM must not hide valid BTIF. */
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.count == 1 && plan.start[0] == 0xf0000);
    app.regions[0].address = CAPACITY;
    app.regions[0].length = 0x1000;
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.count == 1);
    app.regions[0].address = 0x8000;
    app.regions[0].attribute = 0x82;
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.count == 1);
    model.erases = 0;
    memset(&plan, 0, sizeof(plan));
    plan.count = 1;
    plan.start[0] = 0x1234;
    plan.end[0] = 0x4000;
    CHECK(!core_wipe_execute(&nor, &plan, true, workspace));
    CHECK(!model.erases);
    ++assertions;
}

static void test_wipe_without_app(void)
{
    struct core_app app;
    struct core_wipe plan;
    if (!load_fixture("smk37-stock.bin"))
        return;
    memcpy(saved, image, sizeof(image));
    encode_record(0x4020, 32, true, 0x980f, "other.bin", 0x120, 619704, 0x82, 0xf0da);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(!app.base && !app.entry && app.metadata_valid);
    CHECK(app.table_base == 0x4000 && app.state == CORE_APP_NOT_FOUND);
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    CHECK(plan.count == 2);
    memcpy(image, saved, sizeof(image));
    encode_record(0x4020, 32, true, 0x980f, "app.bin", 0x120, 0, 0x82, 0);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(!app.base && app.metadata_valid && app.table_base == 0x4000);
    CHECK(core_wipe_plan(&flash, &app, true, &plan));
    memcpy(image, saved, sizeof(image));
    image[0x4000] ^= 1;
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(!app.metadata_valid && !app.table_base);
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    memcpy(image, saved, sizeof(image));
    encode_record(0x4020, 32, true, 0x980f, "app.bin", UINT32_MAX, 32, 0x82, 0);
    CHECK(!core_app_mount(&flash, 0x980f, &app));
    CHECK(!app.metadata_valid);
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    memcpy(image, saved, sizeof(image));
    image[0x4040] ^= 1;
    CHECK(core_app_mount(&flash, 0x980f, &app));
    CHECK(!core_wipe_plan(&flash, &app, true, &plan));
    ++assertions;
}

static void guard_fixture(void)
{
    uint32_t i;
    memset(image, 255, sizeof(image));
    for (i = 0; i < 0x22c; ++i)
        image[0xc1000 + i] = (uint8_t)(i * 37 + 5);
    memcpy(image + 0xc1224, "trimdat", 8);
    image[0xfff00] = 0x12;
    image[0xfff40] = 0x34;
    reset_model();
}

static void test_guard(void)
{
    unsigned failure;
    uint8_t record[0x22c];
    guard_fixture();
    memcpy(record, image + 0xc1000, sizeof(record));
    CHECK(core_guard(&nor, false, workspace) == CORE_GUARD_UNCHANGED && !model.operations);
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_BACKED_UP);
    CHECK(!model.erases && model.first_program == 0xff010 && model.last_program == 0xff000);
    CHECK(!memcmp(image + 0xff000, "TRIMCOPY", 8));
    CHECK(core_le32(image + 0xff008) == 0xc1000);
    CHECK(core_le16(image + 0xff00c) == 0x22c && core_le16(image + 0xff00e) == core_crc16(0, record, sizeof(record)));
    CHECK(!memcmp(image + 0xff010, record, sizeof(record)));
    CHECK(image[0xfff00] == 0x12 && image[0xfff40] == 0x34);
    memcpy(saved, image, sizeof(image));
    reset_model();
    image[0xc1000] ^= 1;
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_UNCHANGED && !model.operations);
    image[0xc1224] = 0;
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_RESTORED);
    CHECK(model.erases == 1 && model.erase_address[0] == 0xc1000 && model.erase_size[0] == 4096);
    CHECK(!memcmp(image + 0xc1000, record, sizeof(record)));
    CHECK(!memcmp(image + 0xff000, saved + 0xff000, 4096));
    for (failure = 1; failure <= 4; ++failure) {
        memcpy(image, saved, sizeof(image));
        image[0xc1224] = 0;
        reset_model();
        model.fail_at = failure;
        CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_ERROR);
        CHECK(!memcmp(image + 0xff000, saved + 0xff000, 4096));
        reset_model();
        CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_RESTORED);
        CHECK(!memcmp(image + 0xc1000, record, sizeof(record)));
    }
    for (failure = 1; failure <= 4; ++failure) {
        guard_fixture();
        model.fail_at = failure;
        CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_ERROR);
        reset_model();
        if (failure == 1)
            CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_BACKED_UP);
        else
            CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_UNCHANGED && !model.operations);
    }
    guard_fixture();
    image[0xff000] = 0;
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_UNCHANGED && !model.operations);
    memcpy(image, saved, sizeof(image));
    image[0xc1224] = 0;
    image[0xff00e] ^= 1;
    reset_model();
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_UNCHANGED && !model.operations);
    guard_fixture();
    model.bad_read = true;
    CHECK(core_guard(&nor, true, workspace) == CORE_GUARD_ERROR && !model.operations);
    ++assertions;
}

int main(void)
{
    test_primitives();
    test_config();
    test_banks();
    test_lz4();
    test_fixture("fm1-stock.bin", 0xf98496a0, 581564, 0x5d48, 0x8e0dc, 0x93000, 0xe9000, 0xe9000, false);
    test_fixture("smk37-stock.bin", 0x8e57bc96, 619704, 0xf0da, 0x975d8, 0x9d000, 0xc0000, 0xc0000, true);
    test_fixture("smk37-snoop37.bin", 0x6475373c, 619520, 0x6283, 0x97520, 0x9d000, 0xc0000, 0xc0000, true);
    test_fixture("smk37-baudgirl.bin", 0x1eabfb0a, 748520, 0xfbdb, 0xb6d08, 0xd9000, 0xe9000, 0xc0000, true);
    test_fixture_updater();
    test_requests();
    test_mount_header_refresh();
    test_wipe_safety();
    test_wipe_without_app();
    test_guard();
    printf("core: %u groups passed (%u private images checked, malformed inputs, NOR faults)\n", assertions, fixture_groups);
    return 0;
}
