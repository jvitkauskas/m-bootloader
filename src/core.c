/* SPDX-License-Identifier: MIT */
#include "core.h"
static void bytes_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
}

static void bytes_zero(void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = 0;
}

static bool bytes_equal(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return false;
    return true;
}

static bool text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

uint16_t core_le16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)(b[0] | (uint16_t)b[1] << 8);
}

uint32_t core_le32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

uint16_t core_be16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)((uint16_t)b[0] << 8 | b[1]);
}

uint32_t core_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

void core_put_le16(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

void core_put_le32(void *p, uint32_t v)
{
    uint8_t *b = p;
    core_put_le16(b, (uint16_t)v);
    core_put_le16(b + 2, (uint16_t)(v >> 16));
}

void core_put_be16(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

void core_put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    core_put_be16(b, (uint16_t)(v >> 16));
    core_put_be16(b + 2, (uint16_t)v);
}

bool core_range(uint32_t address, uint32_t size, uint32_t limit)
{
    return address <= limit && size <= limit - address;
}

uint16_t core_crc16(uint16_t r, const void *data, size_t size)
{
    const uint8_t *b = data;
    while (size--) {
        unsigned i;
        r ^= (uint16_t)*b++ << 8;
        for (i = 0; i < 8; ++i)
            r = (uint16_t)((r << 1) ^ ((r & 0x8000) ? 0x1021 : 0));
    }
    return r;
}

uint32_t core_crc32(uint32_t r, const void *data, size_t size)
{
    const uint8_t *b = data;
    while (size--) {
        unsigned i;
        r ^= *b++;
        for (i = 0; i < 8; ++i)
            r = (r >> 1) ^ ((r & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return r;
}

uint16_t core_enc(uint16_t state, void *data, size_t size)
{
    uint8_t *b = data;
    while (size--) {
        *b++ ^= (uint8_t)state;
        state = (uint16_t)((state << 1) ^ ((state & 0x8000) ? 0x1021 : 0));
    }
    return state;
}

uint16_t core_usb_begin(uint32_t key)
{
    uint8_t high[2];
    core_put_le16(high, (uint16_t)(key >> 16));
    return core_crc16((uint16_t)key, high, 2);
}

uint16_t core_usb_stream(uint16_t state, uint32_t *index, void *data, size_t size)
{
    static const uint8_t entropy[16] = {
        0xc3, 0xcf, 0xc0, 0xe8, 0xce, 0xd2, 0xb0, 0xae, 0xc4, 0xe3, 0xa3, 0xac, 0xd3, 0xf1, 0xc1, 0xd6
    };
    uint8_t *b = data;
    while (size--) {
        state = core_crc16(state, entropy + (*index & 15), 1);
        ++*index;
        *b++ ^= (uint8_t)state;
    }
    return state;
}

bool core_read(const struct core_flash *f, uint32_t address, void *dst, uint32_t size)
{
    if (!core_range(address, size, f->capacity))
        return false;
    return !size || f->read(f->ctx, address, dst, size);
}

bool core_app_read(const struct core_flash *f, uint32_t base, uint16_t key, uint32_t offset, void *dst, uint32_t size)
{
    uint8_t line[32], *d = dst;
    if (base > f->capacity || !core_range(offset, size, f->capacity - base))
        return false;
    while (size) {
        uint32_t q = offset & ~UINT32_C(31), within = offset & 31;
        uint32_t n = 32 - within;
        if (!within && size >= 32) {
            uint32_t pos;
            n = size & ~UINT32_C(31);
            if (n > CORE_WORK_SIZE)
                n = CORE_WORK_SIZE;
            if (!core_read(f, base + offset, d, n))
                return false;
            for (pos = 0; pos < n; pos += 32)
            core_enc((uint16_t)(key ^ (uint16_t)((offset + pos) >> 2)), d + pos, 32);
            d += n;
            offset += n;
            size -= n;
            continue;
        }
        if (n > size)
            n = size;
        if (!core_read(f, base + q, line, 32))
            return false;
        core_enc((uint16_t)(key ^ (uint16_t)(q >> 2)), line, 32);
        bytes_copy(d, line + within, n);
        d += n;
        offset += n;
        size -= n;
    }
    return true;
}

void core_config_defaults(struct core_config *c)
{
    bytes_zero(c, sizeof(*c));
    c->oscillator_hz = 24000000;
    c->system_hz = 360000000;
    c->width = 4;
    c->spi_divider = 1;
    c->read_mode = 4;
    c->hsb_divider = 1;
    c->lsb_divider = 2;
    c->reset_pin = 17;
    c->reset_hold = 8;
}

static int decimal(uint8_t c) {
    return c >= '0' && c <= '9' ? c - '0' : -1;
}

static bool pin_parse(const uint8_t *v, uint32_t n, uint8_t *pin)
{
    int a, b;
    if (n < 4 || v[0] != 'P' || v[1] < 'A' || v[1] > 'H')
        return false;
    a = decimal(v[2]);
    b = decimal(v[3]);
    if (a < 0 || b < 0 || a * 10 + b > 15)
        return false;
    *pin = (uint8_t)((v[1] - 'A') * 16 + a * 10 + b);
    return true;
}

bool core_config_parse(struct core_config *c, const uint8_t *blob, uint32_t size)
{
    enum {
        K_SPI, K_OSC, K_RESET, K_OSC_FREQ, K_SYS, K_HSB, K_LSB,
        K_HC, K_ONE, K_SDRAM, K_COUNT
    };
    static const char names[] = "SPI\0OSC\0RESET\0OSC_FREQ\0SYS_CLK\0HSB_DIV\0LSB_DIV\0"
        "OSC_HC_EN\0OSC_1PIN_EN\0ENABLE_SDRAM";
    struct core_config parsed;
    uint32_t pos = 34, found = 0;
    core_config_defaults(c);
    core_config_defaults(&parsed);
    if (size < 34 || size > 4096)
        return false;
    while (pos < size) {
        uint32_t n = blob[pos++], begin = pos, value;
        unsigned id;
        const uint8_t *v;
        const char *name = names;
        if (!n || n == 255)
            break;
        while (pos < size && blob[pos])
            ++pos;
        if (pos == size)
            return false;
        if (pos - begin >= 32)
            return false;
        for (id = 0; id < K_COUNT; ++id) {
            if (text_equal((const char *)blob + begin, name))
                break;
            while (*name++) {
            }
        }
        ++pos;
        if (n > size - pos)
            return false;
        v = blob + pos;
        pos += n;
        if (id == K_SPI) {
            if (n == 8 && !v[7])
                --n;
            if (n != 7 || v[1] != '_' || v[3] != '_' || v[5] != '_' || (v[0] != '4' && v[0] != '1') || decimal(v[2]) < 0 || v[4] != '0' || decimal(v[6]) < 0)
                return false;
            parsed.width = (uint8_t)(v[0] - '0');
            parsed.spi_divider = (uint8_t)(v[2] - '0');
        } else if (id == K_OSC) {
            if ((n != 4 && !(n == 5 && !v[4])) || !bytes_equal(v, "OSC0", 4))
                return false;
            found |= 1;
        } else if (id == K_RESET) {
            int a, b;
            if (n == 10 && !v[9])
                --n;
            if (n != 9 || !pin_parse(v, n, &parsed.reset_pin) || v[4] != '_' || v[7] != '_')
                return false;
            a = decimal(v[5]);
            b = decimal(v[6]);
            if (a < 0 || b < 0 || (v[8] != '0' && v[8] != '1'))
                return false;
            parsed.reset_hold = (uint8_t)(a * 10 + b);
            parsed.reset_level = (uint8_t)(v[8] - '0');
        } else {
            if (id == K_COUNT)
                continue;
            if (n != 4)
                return false;
            value = core_le32(v);
            if (id >= K_HC && id <= K_ONE) {
                static const uint8_t offsets[] = {
                    offsetof(struct core_config, high_current), offsetof(struct core_config, one_pin)
                }, limits[] = {
                    1, 1
                };
                id -= K_HC;
                if (value > limits[id])
                    return false;
                *((uint8_t *)&parsed + offsets[id]) = (uint8_t)value;
                continue;
            }
            switch (id) {
            case K_OSC_FREQ:
                parsed.oscillator_hz = value;
                found |= 2;
                break;
            case K_SYS:
                parsed.system_hz = value;
                found |= 4;
                break;
            case K_HSB:
                if (value != 1)
                    return false;
                found |= 8;
                break;
            case K_LSB:
                if (value != 2)
                    return false;
                found |= 16;
                break;
            default:
                if (value)
                    return false;
                break;
            }
        }
    }
    if (found != 31 || parsed.oscillator_hz != 24000000 || parsed.system_hz != 360000000)
        return false;
    *c = parsed;
    return true;
}

bool core_chipkey(const uint8_t prefix[34], uint16_t *key)
{
    unsigned i;
    uint8_t sum = 0;
    uint16_t result = 0;
    if (core_crc16(0, prefix, 32) != core_le16(prefix + 32))
        return false;
    for (i = 0; i < 16; ++i)
        sum = (uint8_t)(sum + prefix[i]);
    if (sum >= 0xe0)
        sum = 0xaa;
    else if (sum <= 0x10)
        sum = 0x55;
    for (i = 0; i < 16; ++i)
        if ((prefix[16 + i] ^ prefix[15 - i]) < sum)
            result |= (uint16_t)(1u << i);
    *key = result;
    return true;
}

bool core_record_parse(struct core_record *r, const uint8_t raw[32])
{
    if (core_crc16(0, raw + 2, 30) != core_le16(raw))
        return false;
    r->data_crc = core_le16(raw + 2);
    r->address = core_le32(raw + 4);
    r->length = core_le32(raw + 8);
    r->attribute = raw[12];
    r->index = core_le16(raw + 14);
    bytes_copy(r->name, raw + 16, 16);
    r->name[16] = 0;
    return true;
}

bool core_header_read(const struct core_flash *f, struct core_header *h)
{
    h->alignment = 0;
    if (!core_read(f, 0, h->bytes, 32))
        return false;
    core_enc(0xffff, h->bytes, 32);
    if (core_crc16(0, h->bytes + 2, 30) != core_le16(h->bytes))
        return false;
    h->filesystem_size = core_le32(h->bytes + 8);
    if (h->filesystem_size > f->capacity)
        return false;
    h->alignment = h->bytes[13];
    return true;
}

bool core_top_find(const struct core_flash *f, const char *name, struct core_record *r)
{
    uint8_t raw[32];
    uint32_t offset, end = f->capacity;
    struct core_record candidate;
    for (offset = 32; offset < 32u * 65u && core_range(offset, 32, end); offset += 32) {
        if (!core_read(f, offset, raw, 32))
            return false;
        core_enc(0xffff, raw, 32);
        if (!core_record_parse(&candidate, raw))
            return false;
        if (candidate.address >= offset + 32 && candidate.address < end)
            end = candidate.address;
        if (text_equal(candidate.name, name)) {
            *r = candidate;
            return true;
        }
    }
    return false;
}

static bool info_version(const struct core_flash *f, uint32_t address, uint16_t *version)
{
    uint8_t raw[32];
    if (!core_read(f, address, raw, 32))
        return false;
    core_enc(0xffff, raw, 32);
    if (core_crc16(0, raw + 2, 30) != core_le16(raw))
        return false;
    *version = core_le16(raw + 6);
    return true;
}

bool core_app_mount(const struct core_flash *f, uint16_t key, struct core_app *app)
{
    return core_app_mount_header(f, key, app, 0);
}

bool core_app_mount_header(const struct core_flash *f, uint16_t key, struct core_app *app, struct core_header *header)
{
    struct core_record selected, secondary, area, record;
    struct core_header local_header;
    uint8_t raw[32];
    uint32_t offset, first_file = UINT32_MAX;
    bool have_app = false;
    unsigned i;
    if (!header)
        header = &local_header;
    bytes_zero(app, sizeof(*app));
    app->key = key;
    if (!core_header_read(f, header) || !core_top_find(f, "app_dir_head", &selected))
        return false;
    if (selected.length && selected.length != UINT32_MAX) {
        uint16_t primary_v = 0, secondary_v = 0;
        bool primary_ok = info_version(f, selected.length, &primary_v);
        if (core_top_find(f, "app_dir_head2", &secondary) && (!primary_ok || (secondary.length && secondary.length != UINT32_MAX && info_version(f, secondary.length, &secondary_v) && secondary_v > primary_v)))
            selected = secondary;
    }
    if ((selected.address & 31) || !core_app_read(f, selected.address, key, 0, raw, 32) || !core_record_parse(&area, raw) || area.attribute != 0x83 || !text_equal(area.name, "app_area_head"))
        return false;
    app->base = selected.address;
    app->extent = area.length;
    if (area.address >= 0x02000000 && !(area.address & 1) && area.address - 0x02000000 < f->capacity - selected.address)
        app->entry = area.address;
    for (i = 0, offset = 32; i < 8 && core_range(offset, 32, first_file); ++i, offset += 32) {
        if (!core_app_read(f, app->base, key, offset, raw, 32))
            goto invalid;
        if (!core_record_parse(&record, raw))
            break;
        if (record.attribute == 0x82) {
            if (record.address < offset + 32)
                goto invalid;
            if (!core_range(record.address, record.length, f->capacity - app->base)) {
                app->state = CORE_APP_INCOMPLETE;
                goto invalid;
            }
            if (record.address < first_file)
                first_file = record.address;
            app->files[app->file_count++] = record;
            if (text_equal(record.name, "app.bin") && record.length) {
                uint32_t entry_offset = app->entry - 0x02000000;
                if (have_app)
                    goto invalid;
                if (!app->entry || entry_offset < record.address || entry_offset - record.address >= record.length)
                    app->entry = 0;
                have_app = true;
                app->app_length = record.length;
            }
        } else if (record.attribute == 0x12 || record.attribute == 0x92) {
            app->regions[app->region_count++] = record;
        }
    }
    if (first_file < offset)
        goto invalid;
    app->metadata_valid = 1;
    app->table_base = app->base;
    if (!have_app || !app->entry) {
        app->base = app->entry = 0;
        return false;
    }
    app->state = CORE_APP_OK;
    return true;
    invalid:
    app->base = 0;
    app->entry = 0;
    return false;
}

static bool erased_tail(const struct core_flash *f, uint32_t end, uint8_t *scratch)
{
    unsigned i;
    if (!core_read(f, end - 32, scratch, 32))
        return false;
    for (i = 0; i < 32; ++i)
        if (scratch[i] != 255)
            return false;
    return true;
}

enum core_app_state core_app_check(const struct core_flash *f, struct core_app *app, uint8_t work[CORE_WORK_SIZE])
{
    unsigned i;
    enum core_app_state state = CORE_APP_OK;
    if (!app->base || app->base > f->capacity || !app->file_count || app->file_count > CORE_MAX_FILES || !app->entry)
        return app->state = CORE_APP_NOT_FOUND;
    for (i = 0; i < app->file_count; ++i) {
        const struct core_record *r = app->files + i;
        uint32_t pos = 0;
        uint16_t crc = 0;
        if (!core_range(r->address, r->length, f->capacity - app->base))
            return app->state = CORE_APP_INCOMPLETE;
        while (pos < r->length) {
            uint32_t n = r->length - pos;
            if (n > CORE_WORK_SIZE)
                n = CORE_WORK_SIZE;
            if (!core_app_read(f, app->base, app->key, r->address + pos, work, n))
                return app->state = CORE_APP_NOT_FOUND;
            crc = core_crc16(crc, work, n);
            pos += n;
        }
        if (crc != r->data_crc) {
            uint32_t start = app->base + r->address, end = start + r->length;
            uint32_t boundary = (start & ~UINT32_C(4095)) + 4096;
            if (state != CORE_APP_INCOMPLETE)
                state = CORE_APP_BAD_CRC;
            for (; boundary <= end; boundary += 4096) {
                if (boundary - start >= 32 && erased_tail(f, boundary, work))
                    state = CORE_APP_INCOMPLETE;
                if (end - boundary < 4096)
                    break;
            }
            if (r->length >= 32 && erased_tail(f, end, work))
                state = CORE_APP_INCOMPLETE;
        }
    }
    app->state = state;
    return state;
}

bool core_request_valid(const uint8_t request[80])
{
    uint16_t crc = core_crc16(0, request + 2, 78);
    return crc && core_le16(request) == crc && core_le16(request + 2);
}

bool core_uart_request(const uint8_t retained[19])
{
    return bytes_equal(retained, "UARTUPDATE", 11) || bytes_equal(retained, "UART_UPDATE_CUSTOM", 19);
}

bool core_request_scan(const struct core_flash *f, uint8_t alignment, uint8_t request[80], uint32_t *count)
{
    uint8_t raw[80];
    uint32_t step = alignment && alignment <= 16 ? (uint32_t)alignment * 256 : 4096;
    uint32_t boundary, matches = 0;
    if (f->capacity < step) {
        if (count)
            *count = 0;
        return false;
    }
    for (boundary = f->capacity - step; boundary > 4096; boundary -= step) {
        if (!core_read(f, boundary - 256, raw, 80))
            break;
        if (core_le16(raw + 6) == 0x5441 && core_crc16(0, raw + 2, 78) == core_le16(raw)) {
            if (!matches)
                bytes_copy(request, raw, 80);
            ++matches;
            if (!count)
                break;
        }
        if (boundary < step)
            break;
    }
    if (count)
        *count = matches;
    return matches && core_request_valid(request);
}

static bool lz4_length(const uint8_t *in, uint32_t size, uint32_t *pos, uint32_t *length)
{
    uint8_t extra;
    do {
        if (*pos == size)
            return false;
        extra = in[(*pos)++];
        if (*length > UINT32_MAX - extra)
            return false;
        *length += extra;
    } while (extra == 255);
    return true;
}

bool core_lz4_block(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t previous_size, uint32_t block_size, uint32_t capacity)
{
    uint32_t p = 0, produced = previous_size, end;
    if (!in_size || !block_size || !core_range(previous_size, block_size, capacity))
        return false;
    end = previous_size + block_size;
    while (p < in_size) {
        uint8_t token = in[p++];
        uint32_t literal = token >> 4, match, distance, i;
        if (literal == 15 && !lz4_length(in, in_size, &p, &literal))
            return false;
        if (literal > in_size - p || literal > end - produced)
            return false;
        bytes_copy(out + produced, in + p, literal);
        p += literal;
        produced += literal;
        if (p == in_size)
            return produced == end;
        if (in_size - p < 2)
            return false;
        distance = core_le16(in + p);
        p += 2;
        if (!distance || distance > produced)
            return false;
        match = (token & 15) + 4;
        if ((token & 15) == 15 && !lz4_length(in, in_size, &p, &match))
            return false;
        if (match > end - produced)
            return false;
        for (i = 0; i < match; ++i) {
            out[produced] = out[produced - distance];
            ++produced;
        }
    }
    return false;
}

bool core_updater_decode(const struct core_flash *f, const uint8_t request[80], uint8_t *out, uint32_t capacity, uint8_t work[CORE_WORK_SIZE], struct core_updater *u)
{
    uint8_t raw[32];
    struct core_record outer;
    uint32_t stream, length, pos, produced = 0, total;
    if (!core_request_valid(request) || !core_read(f, core_le32(request + 72), raw, 32) || !core_record_parse(&outer, raw))
        return false;
    stream = outer.address;
    length = outer.length;
    if (length < 64 || !core_range(stream, length, f->capacity) || !core_read(f, stream + 32, raw, 32) || core_crc16(0, raw + 2, 30) != core_le16(raw))
        return false;
    total = core_le32(raw + 4);
    if (core_le32(raw + 8) != CORE_UPDATE_LOAD || !total || total > CORE_UPDATE_MAX || total > capacity)
        return false;
    u->load = CORE_UPDATE_LOAD;
    u->size = total;
    u->crc = core_le16(raw + 2);
    u->stream_end = stream + length;
    for (pos = 64; pos < length; ) {
        uint32_t compressed, output;
        if (length - pos < 8 || !core_read(f, stream + pos, raw, 8))
            return false;
        compressed = core_le32(raw);
        output = core_le32(raw + 4);
        pos += 8;
        if (!compressed || compressed > CORE_WORK_SIZE || compressed > length - pos || !output || !core_range(produced, output, total) || !core_read(f, stream + pos, work, compressed) || !core_lz4_block(work, compressed, out, produced, output, capacity))
            return false;
        pos += compressed;
        produced += output;
    }
    return produced == total && core_crc16(0, out, total) == u->crc;
}

static bool overlaps(uint32_t a, uint32_t end, uint32_t b, uint32_t bend) {
    return a < bend && b < end;
}

static bool wipe_safe(uint32_t address, uint32_t size, uint32_t capacity, bool smk);
bool core_wipe_plan(const struct core_flash *f, const struct core_app *app, bool smk, struct core_wipe *plan)
{
    unsigned i, j;
    bytes_zero(plan, sizeof(*plan));
    if (!app->metadata_valid || !app->table_base || app->table_base > f->capacity || app->region_count > CORE_MAX_REGIONS || app->file_count > CORE_MAX_FILES || f->capacity < 0x5000)
        return false;
    for (i = 0; i < app->region_count; ++i) {
        const struct core_record *r = app->regions + i;
        uint32_t end;
        if (!text_equal(r->name, "VM") && !text_equal(r->name, "BTIF"))
            continue;
        if ((r->attribute != 0x12 && r->attribute != 0x92) || !r->length || ((r->address | r->length) & 4095) || !core_range(r->address, r->length, f->capacity))
            continue;
        end = r->address + r->length;
        for (j = 0; j < app->file_count; ++j) {
            const struct core_record *file = app->files + j;
            uint32_t start;
            if (!core_range(file->address, file->length, f->capacity - app->table_base))
                return false;
            start = app->table_base + file->address;
            if (overlaps(r->address, end, start, start + file->length))
                break;
        }
        if (j != app->file_count)
            continue;
        uint32_t start = r->address < 0x4000 ? 0x4000 : r->address;
        if (end > f->capacity - 0x1000)
            end = f->capacity - 0x1000;
        if (smk && start >= 0xc1000 && start < 0xc2000)
            start = 0xc2000;
        if (smk && end > 0xc1000 && end <= 0xc2000)
            end = 0xc1000;
        if (start >= end)
            continue;
        /* Interior protected sectors are subtracted by core_wipe_next. */
        if (plan->count == 2)
            return false;
        plan->start[plan->count] = start;
        plan->end[plan->count++] = end;
    }
    if (plan->count == 2) {
        if (plan->start[1] < plan->start[0]) {
            uint32_t v = plan->start[0];
            plan->start[0] = plan->start[1];
            plan->start[1] = v;
            v = plan->end[0];
            plan->end[0] = plan->end[1];
            plan->end[1] = v;
        }
        if (plan->start[1] < plan->end[0])
            return false;
    }
    return plan->count != 0;
}

static bool wipe_safe(uint32_t address, uint32_t size, uint32_t capacity, bool smk)
{
    if (capacity < 0x5000 || !core_range(address, size, capacity) || address < 0x4000 || address + size > capacity - 0x1000)
        return false;
    return !smk || !overlaps(address, address + size, 0xc1000, 0xc2000);
}

bool core_wipe_next(const struct core_wipe *plan, uint32_t capacity, bool smk, uint32_t *cursor, uint32_t *address, uint32_t *size)
{
    unsigned i;
    uint32_t p = *cursor;
    if (plan->count > 2 || (p & 4095))
        return false;
    for (i = 0; i < plan->count; ++i) {
        uint32_t start = plan->start[i], end = plan->end[i];
        if (((start | end) & 4095) || start >= end || end > capacity)
            return false;
        if (p < start)
            p = start;
        while (p < end) {
            uint32_t n = 4096;
            if (!(p & 65535) && end - p >= 65536 && wipe_safe(p, 65536, capacity, smk))
                n = 65536;
            if (wipe_safe(p, n, capacity, smk)) {
                *address = p;
                *size = n;
                *cursor = p + n;
                return true;
            }
            p += 4096;
        }
    }
    *cursor = p;
    return false;
}

static bool program_pages(const struct core_nor *nor, uint32_t address, const uint8_t *src, uint32_t size)
{
    while (size) {
        uint32_t n = 256 - (address & 255);
        if (n > size)
            n = size;
        if (!nor->program(nor->flash.ctx, address, src, n))
            return false;
        address += n;
        src += n;
        size -= n;
    }
    return true;
}

static bool verify_bytes(const struct core_flash *f, uint32_t address, const uint8_t *expected, uint32_t size)
{
    uint8_t raw[32];
    while (size) {
        uint32_t n = size > sizeof(raw) ? sizeof(raw) : size;
        if (!core_read(f, address, raw, n) || !bytes_equal(raw, expected, n))
            return false;
        address += n;
        expected += n;
        size -= n;
    }
    return true;
}

enum core_guard_result core_guard(const struct core_nor *nor, bool smk, uint8_t work[CORE_WORK_SIZE])
{
    const struct core_flash *f = &nor->flash;
    uint8_t *slot = work, *source = work + 0x23c;
    uint32_t base, i;
    bool valid, blank = true, source_valid;
    if (!smk)
        return CORE_GUARD_UNCHANGED;
    if (f->capacity < 0xc3000 || (f->capacity & 4095))
        return CORE_GUARD_ERROR;
    base = f->capacity - 4096;
    if (overlaps(base, base + 4096, 0xc1000, 0xc2000) || !core_read(f, base, slot, 0x23c) || !core_read(f, 0xc1000, source, 0x22c))
        return CORE_GUARD_ERROR;
    valid = bytes_equal(slot, "TRIMCOPY", 8) && core_le32(slot + 8) == 0xc1000 &&
        core_le16(slot + 12) == 0x22c && core_le16(slot + 14) == core_crc16(0, slot + 16, 0x22c) &&
        bytes_equal(slot + 16 + 0x224, "trimdat", 8);
    source_valid = bytes_equal(source + 0x224, "trimdat", 8);
    if (valid) {
        if (source_valid)
            return CORE_GUARD_UNCHANGED;
        if (!nor->erase(f->ctx, 0xc1000, 4096) || !program_pages(nor, 0xc1000, slot + 16, 0x22c) || !verify_bytes(f, 0xc1000, slot + 16, 0x22c))
            return CORE_GUARD_ERROR;
        return CORE_GUARD_RESTORED;
    }
    for (i = 0; i < 0x23c; ++i)
        if (slot[i] != 255)
            blank = false;
    if (!blank || !source_valid)
        return CORE_GUARD_UNCHANGED;
    bytes_copy(slot, "TRIMCOPY", 8);
    core_put_le32(slot + 8, 0xc1000);
    core_put_le16(slot + 12, 0x22c);
    core_put_le16(slot + 14, core_crc16(0, source, 0x22c));
    if (!program_pages(nor, base + 16, source, 0x22c) || !verify_bytes(f, base + 16, source, 0x22c) || !program_pages(nor, base, slot, 16) || !verify_bytes(f, base, slot, 16))
        return CORE_GUARD_ERROR;
    return CORE_GUARD_BACKED_UP;
}

bool core_wipe_execute(const struct core_nor *nor, const struct core_wipe *plan, bool smk, uint8_t work[CORE_WORK_SIZE])
{
    uint32_t cursor = 0, address, size;
    unsigned i;
    if (!plan->count || plan->count > 2)
        return false;
    for (i = 0; i < plan->count; ++i) {
        if (((plan->start[i] | plan->end[i]) & 4095) || plan->start[i] >= plan->end[i] || plan->end[i] > nor->flash.capacity || (i && plan->start[i] < plan->end[i - 1]))
            return false;
    }
    while (core_wipe_next(plan, nor->flash.capacity, smk, &cursor, &address, &size)) {
        uint32_t pos;
        /* Recheck at mutation time independently of UI and iterator. */
        if (!wipe_safe(address, size, nor->flash.capacity, smk) || !nor->erase(nor->flash.ctx, address, size))
            return false;
        for (pos = 0; pos < size; pos += CORE_WORK_SIZE) {
            unsigned i;
            if (!core_read(&nor->flash, address + pos, work, CORE_WORK_SIZE))
                return false;
            for (i = 0; i < CORE_WORK_SIZE; ++i)
                if (work[i] != 255)
                    return false;
        }
    }
    return true;
}

unsigned core_request_retire(const struct core_nor *nor, uint8_t alignment, uint32_t stream_end)
{
    static const uint8_t zero[2];
    uint8_t raw[80], outer[32];
    struct core_record r;
    uint32_t step = alignment && alignment <= 16 ? (uint32_t)alignment * 256 : 4096;
    uint32_t boundary, retired = 0;
    if (nor->flash.capacity < step)
        return 0;
    for (boundary = nor->flash.capacity - step; boundary > 4096; boundary -= step) {
        if (!core_read(&nor->flash, boundary - 256, raw, 80))
            break;
        if (core_le16(raw + 6) == 0x5441 && core_request_valid(raw) && core_read(&nor->flash, core_le32(raw + 72), outer, 32) && core_record_parse(&r, outer) && r.address + r.length == stream_end && nor->program(nor->flash.ctx, boundary - 256, zero, 2))
            ++retired;
        if (boundary < step)
            break;
    }
    return retired;
}

static bool boot_allocation_safe(uint32_t address, uint32_t length, uint32_t capacity)
{
    return address < capacity && length <= capacity - address && !(address < 0x38d0 && address + length > 0xa0);
}

bool core_boot_install(uint8_t area[CORE_BOOT_AREA], const uint8_t *container, uint32_t size, uint32_t capacity)
{
    uint8_t head[32], record[32], other[32];
    struct core_record r;
    uint32_t offset, config = 0;
    if (size < 0x80 || size > 0x3830 || core_le16(container) != 1 || core_le16(container + 2) != size - 16 || core_le32(container + 4) != 0x01c02000 || core_le32(container + 8) != 16 || core_crc16(0, container, 14) != core_le16(container + 14) || core_crc16(0, container + 16, size - 16) != core_le16(container + 12))
        return false;
    bytes_copy(head, area, 32);
    bytes_copy(record, area + 32, 32);
    core_enc(0xffff, head, 32);
    core_enc(0xffff, record, 32);
    if (core_crc16(0, head + 2, 30) != core_le16(head) || !core_record_parse(&r, record) || !text_equal(r.name, "uboot.boot") || r.address != 0xa0 || r.length < 16 || r.length > 0x3830)
        return false;
    /* A0 is the first data allocation, so only three more records fit. */
    for (offset = 64; offset < 0xa0; offset += 32) {
        bytes_copy(other, area + offset, 32);
        core_enc(0xffff, other, 32);
        if (!core_record_parse(&r, other))
            return false;
        bool sentinel = r.length == 0 || r.length == 0xffffffffu;
        if (text_equal(r.name, "app_dir_head") || text_equal(r.name, "app_dir_head2")) {
            /* Directory lengths are sentinels or bank-info addresses. */
            if (!boot_allocation_safe(r.address, 32, capacity) || (!sentinel && !boot_allocation_safe(r.length, 32, capacity)))
                return false;
        } else if (!boot_allocation_safe(r.address, sentinel ? 32 : r.length, capacity))
            return false;
        if (text_equal(r.name, "isd_config.ini")) {
            if (config || sentinel)
                return false;
            config = r.address;
        }
    }
    if (!config || 0xa0 + size > config)
        return false;
    core_put_le16(record + 2, core_crc16(0, container, size));
    core_put_le32(record + 4, 0xa0);
    core_put_le32(record + 8, size);
    core_put_le16(record, core_crc16(0, record + 2, 30));
    core_enc(0xffff, record, 32);
    bytes_copy(area + 32, record, 32);
    for (offset = 0xa0; offset < 0x38d0; ++offset)
        area[offset] = 0xff;
    bytes_copy(area + 0xa0, container, size);
    core_enc(0xffff, area + 0xa0, 16);
    core_enc(0xffff, area + 0xb0, size - 16);
    return true;
}
