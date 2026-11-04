/* SPDX-License-Identifier: MIT */
/* MIDI mode's protocol and flash rules, against a stand-in for a browser installer and the flash.
 * The stand-in frames its messages as web/fm1ota.js in the installers does. */
#include "midi_update.h"
#include "core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FLASH 0x100000u
#define APP_SIZE 0x93000u
#define PACKAGE_OFFSET 0x400u
#define KEY 0x980fu
static uint8_t flash[FLASH], original[FLASH], package[PACKAGE_OFFSET+FLASH], sector[MU_SECTOR];
static uint32_t package_size;
static uint8_t reply[1024];
static uint32_t reply_size;
/* What the stand-ins saw. */
static unsigned erases, programs, reads_served, order;
static unsigned last_program_order[FLASH/MU_SECTOR], first_erase_order[FLASH/MU_SECTOR];
static bool identity_seen, finish_seen, stop_answering, cancel_now, wrote_outside;
static int corrupt_at_pass2 = -1;
static uint32_t pass2_seen;
static char identity[32];
static bool answer_handshake, handshake_sent;
static unsigned cancel_after, soft_after, check_percent, write_percent;

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out) {
    uint32_t acc = 0, bits = 0, o = 0;
    for (uint32_t i = 0; i < n; ++i) {
        acc |= (uint32_t)in[i] << bits;
        bits += 8;
        while (bits >= 7) {
            out[o++] = acc&0x7f;
            acc >>= 7;
            bits -= 7;
        }
    }
    if (bits)
        out[o++] = acc&0x7f;
    return o;
}

static uint32_t unpack7(const uint8_t *in, uint32_t n, uint8_t *out) {
    uint32_t acc = 0, bits = 0, o = 0;
    for (uint32_t i = 0; i < n; ++i) {
        acc |= (uint32_t)(in[i]&0x7f) << bits;
        bits += 7;
        while (bits >= 8) {
            out[o++] = (uint8_t)acc;
            acc >>= 8;
            bits -= 8;
        }
    }
    return o;
}

static void queue_bytes(const uint8_t *data, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        reply[reply_size++] = data[i];
}

static void respond(uint32_t address, const uint8_t *data, uint32_t len) {
    static uint8_t body[1024], framed[1200];
    uint32_t total = len+8;
    uint8_t head[14] = { 0, 0x59, 0x30, (uint8_t)total, (uint8_t)(total >> 8), 0, 0, (uint8_t)address,
        (uint8_t)(address >> 8), (uint8_t)(address >> 16), (uint8_t)(address >> 24), (uint8_t)len, (uint8_t)(len >> 8), 0 };
    uint32_t sum = 0;
    memcpy(body, head, 14);
    memcpy(body+14, data, len);
    for (uint32_t i = 6; i < 14+len; ++i)
        sum += body[i];
    body[14+len] = (uint8_t)~sum;
    uint32_t n = pack7(body, 15+len, framed+1);
    framed[0] = 0xf0;
    framed[n+1] = 0xf7;
    queue_bytes(framed, n+2);
}

/* The device's message, as parseRequest and parseIdentity in fm1ota.js read it. */
void mu_send(const uint8_t *data, uint32_t size) {
    static uint8_t u[64];
    assert(data[0] == 0xf0 && data[size-1] == 0xf7);
    uint32_t n = unpack7(data+1, size-2, u);
    assert(u[0] == 0 && u[1] == 0x59);
    if (u[2] == 0x11) {
        assert(n == 34);
        uint32_t sum = 0;
        for (uint32_t i = 6; i < 33; ++i)
            sum += u[i];
        assert((uint8_t)~sum == u[33]);
        memcpy(identity, u+6, 27);
        identity_seen = true;
        return;
    }
    assert(u[2] == 0x30 && n == 15);
    uint32_t sum = 0;
    for (uint32_t i = 6; i < 14; ++i)
        sum += u[i];
    assert((uint8_t)~sum == u[14]);
    uint32_t address = u[7] | (uint32_t)u[8] << 8 | (uint32_t)u[9] << 16 | (uint32_t)u[10] << 24;
    uint32_t len = u[11] | (uint32_t)u[12] << 8 | (uint32_t)u[13] << 16;
    if (stop_answering)
        return;
    if (address == 0xf0000000u) {
        finish_seen = true;
        respond(address, (const uint8_t *)"success", 8);
        return;
    }
    assert(len <= 512 && address+len <= package_size);
    ++reads_served;
    if (cancel_after && reads_served == cancel_after)
        cancel_now = true;
    if (soft_after && reads_served == soft_after) {
        static const uint8_t soft[] = { 0xf0, 0x22, 0x24, 0x35, 0x7d, 0xf7 };
        queue_bytes(soft, sizeof(soft));
        return;
    }
    static uint8_t chunk[512];
    memcpy(chunk, package+address, len);
    /* A package that changes after the check: seen once it's being read for the second time. */
    if (corrupt_at_pass2 >= 0 && address == PACKAGE_OFFSET+(uint32_t)corrupt_at_pass2 && ++pass2_seen == 2)
        chunk[3] ^= 1;
    respond(address, chunk, len);
}

void mu_idle(void) {
    /* One message at a time, as the real stream arrives. */
    if (reply_size) {
        for (uint32_t i = 0; i < reply_size; ++i)
            mu_rx(reply[i]);
        reply_size = 0;
    }
}

/* Called on every pass of mu_run's loop, so it can send from inside. */
bool mu_cancel(void) {
    static const uint8_t hs[] = { 0xf0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7f, 0xf7 };
    if (answer_handshake) {
        answer_handshake = false;
        queue_bytes(hs, sizeof(hs));
        handshake_sent = true;
        return false;
    }
    if (handshake_sent && identity_seen) {
        handshake_sent = false;
        return true;
    }
    return cancel_now;
}

bool mu_read(uint32_t address, void *out, uint32_t size) {
    assert(address+size <= FLASH);
    memcpy(out, flash+address, size);
    return true;
}

static void note_write(uint32_t address, uint32_t size) {
    if (address < MU_FIRST || address+size > MU_LIMIT)
        wrote_outside = true;
}

bool mu_erase(uint32_t address) {
    assert(!(address%MU_SECTOR));
    note_write(address, MU_SECTOR);
    memset(flash+address, 0xff, MU_SECTOR);
    ++erases;
    if (!first_erase_order[address/MU_SECTOR])
        first_erase_order[address/MU_SECTOR] = ++order;
    return true;
}

bool mu_program(uint32_t address, const void *data, uint32_t size) {
    note_write(address, size);
    const uint8_t *p = data;
    for (uint32_t i = 0; i < size; ++i)
        flash[address+i] &= p[i];
    ++programs;
    last_program_order[address/MU_SECTOR] = ++order;
    return true;
}

void mu_progress(bool writing, unsigned percent) {
    assert(percent <= 100);
    /* The check finishes before writing starts, and nothing is erased until it has. */
    if (writing) {
        assert(check_percent == 100);
        write_percent = percent;
    } else {
        assert(!erases && !write_percent);
        check_percent = percent;
    }
}

/* An M-UPGRADE package: its header, then flash.bin with the firmware encrypted for the key. */
static void make_package(uint16_t key, uint32_t app_size) {
    memset(package, 0, sizeof(package));
    uint8_t *f = package+PACKAGE_OFFSET;
    for (uint32_t i = 0; i < app_size; ++i)
        f[i] = (uint8_t)(i*31+(i >> 9));
    uint8_t first[32];
    for (unsigned i = 0; i < 32; ++i)
        first[i] = (uint8_t)(i*7);
    uint16_t c = core_crc16(0, first+2, 30);
    first[0] = (uint8_t)c;
    first[1] = (uint8_t)(c >> 8);
    core_enc(key, first, 32);
    memcpy(f+MU_FIRST, first, 32);
    uint8_t entry[0x50] = { 0 };
    uint16_t d = core_crc16(0, f, app_size);
    entry[4] = (uint8_t)d;
    entry[5] = (uint8_t)(d >> 8);
    entry[8] = PACKAGE_OFFSET&0xff;
    entry[9] = PACKAGE_OFFSET >> 8;
    entry[12] = (uint8_t)app_size;
    entry[13] = (uint8_t)(app_size >> 8);
    entry[14] = (uint8_t)(app_size >> 16);
    uint8_t *h = package;
    memset(h, 0, 0x40);
    h[8] = 1;
    /* The entries' CRC is over them as stored, encrypted. */
    core_enc(0xffff, entry, 0x50);
    memcpy(h+0x40, entry, 0x50);
    uint16_t ce = core_crc16(0, entry, 0x50);
    h[2] = (uint8_t)ce;
    h[3] = (uint8_t)(ce >> 8);
    uint16_t ch = core_crc16(0, h+2, 0x3e);
    h[0] = (uint8_t)ch;
    h[1] = (uint8_t)(ch >> 8);
    core_enc(0xffff, h, 0x40);
    package_size = PACKAGE_OFFSET+app_size;
}

static void reset_all(void) {
    for (uint32_t i = 0; i < FLASH; ++i)
        original[i] = (uint8_t)(i*13+(i >> 12)+0x5a);
    memcpy(flash, original, FLASH);
    erases = programs = reads_served = order = pass2_seen = 0;
    memset(last_program_order, 0, sizeof(last_program_order));
    memset(first_erase_order, 0, sizeof(first_erase_order));
    identity_seen = finish_seen = stop_answering = cancel_now = wrote_outside = false;
    corrupt_at_pass2 = -1;
    cancel_after = soft_after = check_percent = write_percent = 0;
    reply_size = 0;
}

static void ask_upgrade(void) {
    static const uint8_t upgrade[] = { 0xf0, 0x22, 0x24, 0x35, 0x7f, 0xf7 };
    queue_bytes(upgrade, sizeof(upgrade));
}

static bool untouched(void) {
    return !erases && !programs && !memcmp(flash, original, FLASH);
}

static bool outside_same(uint32_t size) {
    return !memcmp(flash, original, MU_FIRST) && !memcmp(flash+size, original+size, FLASH-size);
}

int main(void) {
    bool written;
    make_package(KEY, APP_SIZE);
    /* A clean install: only [0x4000, size) changes, and the first sector is finished last. */
    reset_all();
    ask_upgrade();
    enum mu_result r = mu_run(KEY, sector, &written);
    assert(r == MU_DONE && written && finish_seen && !wrote_outside && write_percent == 100);
    assert(!memcmp(flash+MU_FIRST, package+PACKAGE_OFFSET+MU_FIRST, APP_SIZE-MU_FIRST));
    assert(outside_same(APP_SIZE));
    unsigned first_at = MU_FIRST/MU_SECTOR;
    assert(first_erase_order[first_at] == 1);
    for (uint32_t s = first_at+1; s < APP_SIZE/MU_SECTOR; ++s)
        assert(last_program_order[s] < last_program_order[first_at]);
    /* Installing it again rewrites only the first sector, erased up front and again to write it. */
    erases = programs = check_percent = write_percent = 0;
    finish_seen = false;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_DONE && finish_seen);
    assert(erases == 2 && !memcmp(flash+MU_FIRST, package+PACKAGE_OFFSET+MU_FIRST, APP_SIZE-MU_FIRST));
    /* The wrong chip key: nothing erased. */
    reset_all();
    make_package(0x1234, APP_SIZE);
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_WRONG_KEY && !written && untouched());
    /* Damage anywhere, found by the whole-package check before anything is erased. */
    make_package(KEY, APP_SIZE);
    reset_all();
    package[PACKAGE_OFFSET+0x20000] ^= 1;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && !written && untouched());
    /* A bad header. */
    make_package(KEY, APP_SIZE);
    reset_all();
    package[3] ^= 1;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && untouched());
    /* Sizes that would reach what it must not write, or that aren't whole sectors. */
    make_package(KEY, MU_LIMIT+MU_SECTOR);
    reset_all();
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && untouched());
    make_package(KEY, APP_SIZE+1);
    reset_all();
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && untouched());
    make_package(KEY, MU_FIRST);
    reset_all();
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && untouched());
    /* The largest package allowed stays below the key area. */
    make_package(KEY, MU_LIMIT);
    reset_all();
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_DONE && !wrote_outside && outside_same(MU_LIMIT));
    /* A sector that arrives different the second time isn't written. */
    make_package(KEY, APP_SIZE);
    reset_all();
    corrupt_at_pass2 = 0x20000;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_BAD_PACKAGE && written);
    assert(memcmp(flash+0x20000, package+PACKAGE_OFFSET+0x20000, MU_SECTOR));
    assert(!memcmp(flash, original, MU_FIRST));
    /* A host that stops answering. */
    reset_all();
    stop_answering = true;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_LOST && !written && untouched());
    /* The soft key. */
    reset_all();
    static const uint8_t soft[] = { 0xf0, 0x22, 0x24, 0x35, 0x7d, 0xf7 };
    queue_bytes(soft, sizeof(soft));
    assert(mu_run(KEY, sector, &written) == MU_DOWNLOAD && untouched());
    /* Junk, and a too-long frame, are ignored. */
    reset_all();
    static uint8_t junk[900];
    memset(junk, 0x11, sizeof(junk));
    junk[0] = 0xf0;
    junk[sizeof(junk)-1] = 0xf7;
    queue_bytes(junk, sizeof(junk));
    static const uint8_t bad_sum[] = { 0xf0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x41, 0x7f, 0xf7 };
    queue_bytes(bad_sum, sizeof(bad_sum));
    cancel_now = true;
    assert(mu_run(KEY, sector, &written) == MU_CANCELLED && !identity_seen && untouched());
    /* Before anything is erased, the key and the soft key still get out. */
    make_package(KEY, APP_SIZE);
    reset_all();
    cancel_after = 100;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_CANCELLED && !written && untouched());
    reset_all();
    soft_after = 100;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_DOWNLOAD && !written && untouched());
    /* Once it's erasing, the key does nothing: stopping there would leave no firmware. */
    reset_all();
    cancel_after = (APP_SIZE/512)+50;
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_DONE && written);
    assert(!memcmp(flash+MU_FIRST, package+PACKAGE_OFFSET+MU_FIRST, APP_SIZE-MU_FIRST));
    /* The handshake installers send (fm1ota.js HS_QUERY) gets the identity they look for. */
    reset_all();
    answer_handshake = true;
    assert(mu_run(KEY, sector, &written) == MU_CANCELLED && identity_seen);
    assert(!memcmp(identity, "ota-FM-1_900", 13));
    printf("midi update: install, refusals, limits and order passed\n");
    return 0;
}
