/* SPDX-License-Identifier: MIT */
/* Installs firmware over USB MIDI, as a firmware's update loader does, so that browser installers
 * can reach it from a phone: docs/SPEC.md §11.6. The host answers our read requests with parts of
 * the package. The whole package is read and checked before anything is erased. */
#include "midi_update.h"
#include "core.h"
#define FRAME_MAX 640u
#define PACKET_MAX 512u
#define TRIES 5u
#define TRY_MS 2000u
#define FINISH 0xf0000000u
#define HEADER 0x400u
#define ENTRY 0x50u
#define MAX_SECTORS (0xff000u/MU_SECTOR)

static uint8_t frame[FRAME_MAX], decoded[PACKET_MAX+16], wire[FRAME_MAX];
static uint16_t sector_crc[MAX_SECTORS];
static uint32_t frame_size;
uint32_t mu_messages;
static bool collecting, frame_ready, erasing, download_asked;

void mu_rx(uint8_t b) {
    if (b == 0xf0) {
        frame_size = 0;
        collecting = !frame_ready;
    } else if (b == 0xf7) {
        frame_ready = collecting;
        collecting = false;
    } else if (collecting) {
        if (frame_size == FRAME_MAX)
            collecting = false;
        else
            frame[frame_size++] = b;
    }
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Sends cmd with its body, 8-to-7 packed. */
static void send_message(uint8_t cmd, const uint8_t *body, uint32_t size) {
    uint8_t head[6] = { 0, 0x59, cmd, (uint8_t)size, (uint8_t)(size >> 8) };
    uint32_t sum = 0, bits = 0, n = 0, at = 0, total = size+7;
    wire[n++] = 0xf0;
    for (uint32_t accumulator = 0; at < total || bits; ) {
        if (bits < 7 && at < total) {
            uint8_t byte;
            if (at < 6)
                byte = head[at];
            else if (at-6 < size) {
                byte = body[at-6];
                sum += byte;
            } else
                byte = (uint8_t)~sum;
            ++at;
            accumulator |= (uint32_t)byte << bits;
            bits += 8;
        }
        wire[n++] = accumulator&0x7f;
        accumulator >>= 7;
        bits = bits > 7 ? bits-7:0;
    }
    wire[n++] = 0xf7;
    mu_send(wire, n);
}

static void send_identity(void) {
    static const char id[27] = MU_IDENTITY;
    send_message(0x11, (const uint8_t *)id, sizeof(id));
}

static uint32_t unpack(void) {
    uint32_t out = 0, bits = 0, accumulator = 0;
    for (uint32_t i = 0; i < frame_size; ++i) {
        accumulator |= (uint32_t)(frame[i]&0x7f) << bits;
        bits += 7;
        if (bits >= 8) {
            if (out == sizeof(decoded))
                return 0;
            decoded[out++] = (uint8_t)accumulator;
            accumulator >>= 8;
            bits -= 8;
        }
    }
    return out;
}

enum { NOTHING, MESSAGE, ASK_UPGRADE, ASK_DOWNLOAD, ASK_RESTART };
/* The next message: its size is in *size, decoded and checked. */
static unsigned take(uint32_t *size) {
    if (!frame_ready)
        return NOTHING;
    unsigned kind = NOTHING;
    if (frame_size == 4 && frame[0] == 0x22 && frame[1] == 0x24 && frame[2] == 0x35) {
        if (frame[3] == 0x7f)
            kind = ASK_UPGRADE;
        else if (frame[3] == 0x7d)
            kind = ASK_DOWNLOAD;
        else if (frame[3] == 0x7e)
            kind = ASK_RESTART;
    } else {
        uint32_t n = unpack(), sum = 0;
        if (n >= 7 && !decoded[0] && decoded[1] == 0x59 && (decoded[3] | (uint32_t)decoded[4] << 8) == n-7 && !decoded[5]) {
            for (uint32_t i = 6; i < n; ++i)
                sum += decoded[i];
            if ((sum&0xff) == 0xff) {
                kind = MESSAGE;
                *size = n;
            }
        }
    }
    frame_ready = false;
    if (kind != NOTHING)
        ++mu_messages;
    return kind;
}

static bool ask(uint32_t address, uint32_t size, uint8_t *out) {
    uint8_t request[8] = { 0, (uint8_t)address, (uint8_t)(address >> 8), (uint8_t)(address >> 16),
        (uint8_t)(address >> 24), (uint8_t)size, (uint8_t)(size >> 8), 0 };
    for (unsigned tries = 0; tries < TRIES; ++tries) {
        send_message(0x30, request, sizeof(request));
        for (unsigned ms = 0; ms < TRY_MS; ++ms) {
            uint32_t n;
            mu_idle();
            unsigned kind = take(&n);
            /* Until something is erased, the key and the soft key still get out. */
            if (kind == ASK_DOWNLOAD) {
                download_asked = true;
                return false;
            }
            if (!erasing && mu_cancel())
                return false;
            if (kind != MESSAGE)
                continue;
            if (decoded[2] == 0x11 && n == 7)
                send_identity();
            else if (decoded[2] == 0x30 && n == size+15 && le32(decoded+7) == address
                    && (decoded[11] | (uint32_t)decoded[12] << 8) == size) {
                for (uint32_t i = 0; i < size; ++i)
                    out[i] = decoded[14+i];
                return true;
            }
        }
    }
    return false;
}

static bool ask_sector(uint32_t offset, uint8_t *out) {
    for (uint32_t at = 0; at < MU_SECTOR; at += PACKET_MAX)
        if (!ask(offset+at, PACKET_MAX, out+at))
            return false;
    return true;
}

/* flash.bin in the package, from its header. */
static enum mu_result find_flash(uint8_t *sector, uint32_t *offset, uint32_t *size, uint16_t *crc) {
    for (uint32_t at = 0; at < HEADER; at += PACKET_MAX)
        if (!ask(at, PACKET_MAX, sector+at))
            return MU_LOST;
    core_enc(0xffff, sector, 0x40);
    uint32_t count = sector[8] | (uint32_t)sector[9] << 8;
    if (!count || count > 11 || core_crc16(0, sector+2, 0x3e) != core_le16(sector)
            || core_crc16(0, sector+0x40, count*ENTRY) != core_le16(sector+2))
        return MU_BAD_PACKAGE;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *entry = sector+0x40+i*ENTRY;
        core_enc(0xffff, entry, ENTRY);
        if (!core_le16(entry)) {
            *crc = core_le16(entry+4);
            *offset = le32(entry+8);
            *size = le32(entry+12);
            if (*size <= MU_FIRST || *size > MU_LIMIT || *size%MU_SECTOR)
                return MU_BAD_PACKAGE;
            return MU_DONE;
        }
    }
    return MU_BAD_PACKAGE;
}

/* The package's firmware must decrypt with this chip's key, and every byte must arrive intact. */
static enum mu_result check_flash(uint8_t *sector, uint32_t offset, uint32_t size, uint16_t crc, uint16_t key) {
    uint16_t whole = 0;
    if (!ask(offset+MU_FIRST, 32, sector))
        return MU_LOST;
    core_enc(key, sector, 32);
    if (core_crc16(0, sector+2, 30) != core_le16(sector))
        return MU_WRONG_KEY;
    for (uint32_t at = 0; at < size; at += MU_SECTOR) {
        if (!ask_sector(offset+at, sector))
            return MU_LOST;
        whole = core_crc16(whole, sector, MU_SECTOR);
        sector_crc[at/MU_SECTOR] = core_crc16(0, sector, MU_SECTOR);
        mu_progress(false, at*100/size);
    }
    return whole == crc ? MU_DONE:MU_BAD_PACKAGE;
}

static bool same(uint32_t at, const uint8_t *data) {
    for (uint32_t i = 0; i < MU_SECTOR; i += PACKET_MAX) {
        if (!mu_read(at+i, decoded, PACKET_MAX))
            return false;
        for (uint32_t j = 0; j < PACKET_MAX; ++j)
            if (decoded[j] != data[i+j])
                return false;
    }
    return true;
}

static enum mu_result write_sector(uint32_t offset, uint32_t at, uint8_t *sector, bool *written) {
    if (!ask_sector(offset+at, sector))
        return MU_LOST;
    if (core_crc16(0, sector, MU_SECTOR) != sector_crc[at/MU_SECTOR])
        return MU_BAD_PACKAGE;
    if (same(at, sector))
        return MU_DONE;
    *written = true;
    if (!mu_erase(at))
        return MU_FLASH_ERROR;
    for (uint32_t i = 0; i < MU_SECTOR; i += PACKET_MAX)
        if (!mu_program(at+i, sector+i, PACKET_MAX))
            return MU_FLASH_ERROR;
    return same(at, sector) ? MU_DONE:MU_FLASH_ERROR;
}

static enum mu_result install(uint16_t key, uint8_t *sector, bool *written) {
    uint32_t offset = 0, size = 0;
    uint16_t crc = 0;
    enum mu_result r = find_flash(sector, &offset, &size, &crc);
    if (r == MU_DONE)
        r = check_flash(sector, offset, size, crc, key);
    if (r != MU_DONE)
        return r;
    /* The firmware's first sector goes last: until then, what's in flash can't pass as firmware. */
    mu_progress(false, 100);
    mu_progress(true, 0);
    *written = erasing = true;
    if (!mu_erase(MU_FIRST))
        return MU_FLASH_ERROR;
    for (uint32_t at = MU_FIRST+MU_SECTOR; at < size; at += MU_SECTOR) {
        if (at >= MU_KEEP_FROM && at < MU_KEEP_TO)
            continue;
        r = write_sector(offset, at, sector, written);
        if (r != MU_DONE)
            return r;
        mu_progress(true, at*100/size);
    }
    r = write_sector(offset, MU_FIRST, sector, written);
    if (r != MU_DONE)
        return r;
    mu_progress(true, 100);
    /* The host's "success" isn't needed: it only has to see that we finished. */
    (void)ask(FINISH, 8, sector);
    return MU_DONE;
}

static void send_refusal(void) {
    static const uint8_t refused = 1;
    send_message(0, &refused, 1);
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8*i));
}

/* What's answering: a bootloader or a RAM stage, not a firmware or its update loader. */
static void send_info(uint16_t key, const struct mu_unit *unit) {
    static uint8_t info[64];
    uint32_t n = 28;
    for (unsigned i = 0; i < 5; ++i)
        info[i] = (uint8_t)"MBOOT"[i];
    info[5] = 1;
    info[6] = unit->kind;
    info[7] = 0;
    info[8] = (uint8_t)key;
    info[9] = (uint8_t)(key >> 8);
    put_le32(info+10, unit->capacity);
    put_le32(info+14, unit->jedec);
    for (unsigned i = 0; i < 8; ++i)
        info[18+i] = unit->uid[i];
    info[26] = info[27] = 0;
    for (const char *v = unit->version; *v && n < sizeof(info)-1; ++v)
        info[n++] = (uint8_t)*v;
    info[n++] = 0;
    send_message(0x12, info, n);
}

/* Sends count bytes of raw flash from address, or the CRC-32 the chip computes of them. Both body
 * layouts start with what stock firmware's read uses: provider (0, the flash), address, count. */
static void serve_read(uint8_t cmd, uint8_t *sector, uint32_t n) {
    struct mu_unit unit;
    mu_unit_info(&unit);
    uint32_t address = le32(decoded+7), count = decoded[11] | (uint32_t)decoded[12] << 8 | (uint32_t)decoded[13] << 16;
    if (n != 15 || decoded[6] || !count || address >= unit.capacity || count > unit.capacity-address
            || (cmd == 0x23 && count > PACKET_MAX)) {
        send_refusal();
        return;
    }
    for (unsigned i = 0; i < 8; ++i)
        decoded[i] = decoded[6+i];
    if (cmd == 0x23) {
        if (mu_read(address, decoded+8, count))
            send_message(cmd, decoded, 8+count);
        else
            send_refusal();
        return;
    }
    uint32_t crc = UINT32_MAX;
    for (uint32_t at = 0; at < count; at += MU_SECTOR) {
        uint32_t part = count-at < MU_SECTOR ? count-at:MU_SECTOR;
        if (!mu_read(address+at, sector, part)) {
            send_refusal();
            return;
        }
        crc = core_crc32(crc, sector, part);
        mu_idle();
    }
    put_le32(decoded+8, crc^UINT32_MAX);
    send_message(cmd, decoded, 12);
}

enum mu_result mu_run(uint16_t key, uint8_t *sector, bool *written) {
    frame_ready = collecting = erasing = download_asked = false;
    *written = false;
    for (; ; ) {
        uint32_t n;
        mu_idle();
        unsigned kind = take(&n);
        if (kind == ASK_UPGRADE) {
            enum mu_result r = install(key, sector, written);
            if (download_asked)
                return MU_DOWNLOAD;
            /* Stopped with the key, before anything was erased. */
            return r == MU_LOST && !*written && mu_cancel() ? MU_CANCELLED:r;
        }
        if (kind == ASK_DOWNLOAD)
            return MU_DOWNLOAD;
        if (kind == ASK_RESTART)
            return MU_RESTART;
        if (kind == MESSAGE) {
            if (decoded[2] == 0x11 && n == 7)
                send_identity();
            else if (decoded[2] == 0x12 && n == 7) {
                struct mu_unit unit;
                mu_unit_info(&unit);
                send_info(key, &unit);
            } else if (decoded[2] == 0x23 || decoded[2] == 0x24)
                serve_read(decoded[2], sector, n);
            else if (decoded[2] != 0x30)
                send_refusal();
        }
        if (mu_cancel())
            return MU_CANCELLED;
    }
}
