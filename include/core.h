/* SPDX-License-Identifier: MIT */
#ifndef MBOOT_CORE_H
#define MBOOT_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CORE_MAX_FILES 8
#define CORE_MAX_REGIONS 8
#define CORE_WORK_SIZE 4096
#define CORE_UPDATE_LOAD UINT32_C(0x01c0a800)
#define CORE_UPDATE_MAX UINT32_C(0x5b1c)
uint16_t core_le16(const void *p);
uint32_t core_le32(const void *p);
uint16_t core_be16(const void *p);
uint32_t core_be32(const void *p);
void core_put_le16(void *p, uint16_t v);
void core_put_le32(void *p, uint32_t v);
void core_put_be16(void *p, uint16_t v);
void core_put_be32(void *p, uint32_t v);
bool core_range(uint32_t address, uint32_t size, uint32_t limit);
uint16_t core_crc16(uint16_t remainder, const void *data, size_t size);
/* Raw IEEE streaming state: begin UINT32_MAX, finish XOR UINT32_MAX. */
uint32_t core_crc32(uint32_t state, const void *data, size_t size);
uint16_t core_enc(uint16_t state, void *data, size_t size);
uint16_t core_usb_begin(uint32_t key);
uint16_t core_usb_stream(uint16_t state, uint32_t *index, void *data, size_t size);
struct core_flash {
    void *ctx;
    uint32_t capacity;
    bool (*read)(void *ctx, uint32_t address, void *dst, uint32_t size);
};
bool core_read(const struct core_flash *f, uint32_t address, void *dst, uint32_t size);
bool core_app_read(const struct core_flash *f, uint32_t base, uint16_t key, uint32_t offset, void *dst, uint32_t size);
struct core_config {
    uint32_t oscillator_hz, system_hz;
    uint8_t width, spi_divider, read_mode, hsb_divider, lsb_divider;
    uint8_t oscillator_tag, high_current, one_pin, reset_pin, reset_level, reset_hold;
};
/* Explicit board recovery profile; parse replaces it only on complete success. */
void core_config_defaults(struct core_config *c);
bool core_config_parse(struct core_config *c, const uint8_t *blob, uint32_t size);
bool core_chipkey(const uint8_t prefix[34], uint16_t *key);
struct core_record {
    uint32_t address, length;
    uint16_t data_crc, index;
    uint8_t attribute;
    char name[17];
};
bool core_record_parse(struct core_record *r, const uint8_t raw[32]);
struct core_header {
    uint8_t bytes[32];
    uint8_t alignment;
    uint32_t filesystem_size;
};
bool core_header_read(const struct core_flash *f, struct core_header *h);
bool core_top_find(const struct core_flash *f, const char *name, struct core_record *r);
enum core_app_state {
    CORE_APP_NOT_FOUND, CORE_APP_OK, CORE_APP_BAD_CRC, CORE_APP_INCOMPLETE
};
struct core_app {
    uint32_t base, entry, extent, app_length, table_base;
    uint16_t key;
    uint8_t file_count, region_count, metadata_valid;
    enum core_app_state state;
    struct core_record files[CORE_MAX_FILES], regions[CORE_MAX_REGIONS];
};
/* Mount validates metadata/ranges/entry, check validates every file's data. */
bool core_app_mount(const struct core_flash *f, uint16_t key, struct core_app *app);
/* Returns the freshly decoded flash header even if no usable app is found. */
bool core_app_mount_header(const struct core_flash *f, uint16_t key, struct core_app *app, struct core_header *header);
enum core_app_state core_app_check(const struct core_flash *f, struct core_app *app, uint8_t workspace[CORE_WORK_SIZE]);
bool core_request_valid(const uint8_t request[80]);
bool core_uart_request(const uint8_t retained[19]);
/* Selects the first magic/CRC match, then checks its nonzero CRC/type.
 * count, if non-NULL, counts all magic/CRC matches independently of validity. */
bool core_request_scan(const struct core_flash *f, uint8_t alignment, uint8_t request[80], uint32_t *count);
bool core_lz4_block(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t previous_size, uint32_t block_size, uint32_t capacity);
struct core_updater {
    uint32_t load, size, stream_end;
    uint16_t crc;
};
bool core_updater_decode(const struct core_flash *f, const uint8_t request[80], uint8_t *output, uint32_t capacity, uint8_t workspace[CORE_WORK_SIZE], struct core_updater *u);
/* Declared extents with protected interior sectors excluded by the iterator. */
struct core_wipe {
    uint32_t start[2], end[2];
    uint8_t count;
};
bool core_wipe_plan(const struct core_flash *f, const struct core_app *app, bool smk, struct core_wipe *plan);
/* Returns the next safe transaction (4 KiB or 64 KiB), advancing cursor. */
bool core_wipe_next(const struct core_wipe *plan, uint32_t capacity, bool smk, uint32_t *cursor, uint32_t *address, uint32_t *size);
struct core_nor {
    struct core_flash flash;
    bool (*program)(void *ctx, uint32_t address, const void *src, uint32_t size);
    bool (*erase)(void *ctx, uint32_t address, uint32_t size);
};
enum core_guard_result {
    CORE_GUARD_UNCHANGED, CORE_GUARD_BACKED_UP,
    CORE_GUARD_RESTORED, CORE_GUARD_ERROR
};
enum core_guard_result core_guard(const struct core_nor *nor, bool smk, uint8_t workspace[CORE_WORK_SIZE]);
bool core_wipe_execute(const struct core_nor *nor, const struct core_wipe *plan, bool smk, uint8_t workspace[CORE_WORK_SIZE]);
#endif
