/* SPDX-License-Identifier: MIT */
/* MIDI mode's protocol and flash rules, against a stand-in for a browser installer and the flash.
 * The stand-in frames its messages as web/fm1ota.js in the installers does. */
#include "midi_update.h"
#include "core.h"
#include <assert.h>
#include <setjmp.h>
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
/* A RAM app the stand-in offers, and what happened to it. */
uint8_t mu_test_app_ram[MU_APP_MAX];
static uint8_t app[MU_APP_MAX];
static uint32_t app_size, app_requests;
static unsigned apps_started, app_stop_after, app_cancel_after;
static bool app_first_read_seen;
static uint32_t app_first_read;
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
/* What the host-initiated commands answered. */
static uint8_t answer[700], asked[1200];
static int answered = -1;
static uint32_t answer_size, asked_size;

void mu_send(const uint8_t *data, uint32_t size) {
    static uint8_t u[700];
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
    if (u[2] != 0x30) {
        uint32_t len = u[3] | (uint32_t)u[4] << 8 | (uint32_t)u[5] << 16, sum = 0;
        assert(len+7 == n);
        for (uint32_t i = 6; i < n-1; ++i)
            sum += u[i];
        assert((uint8_t)~sum == u[n-1]);
        answered = u[2];
        answer_size = len;
        memcpy(answer, u+6, len);
        return;
    }
    assert(n == 15);
    uint32_t sum = 0;
    for (uint32_t i = 6; i < 14; ++i)
        sum += u[i];
    assert((uint8_t)~sum == u[14]);
    uint32_t address = u[7] | (uint32_t)u[8] << 8 | (uint32_t)u[9] << 16 | (uint32_t)u[10] << 24;
    uint32_t len = u[11] | (uint32_t)u[12] << 8 | (uint32_t)u[13] << 16;
    if (stop_answering)
        return;
    if (app_size) {
        assert(len <= 512 && address+len <= app_size);
        if (!app_first_read_seen) {
            app_first_read_seen = true;
            app_first_read = address;
        }
        ++app_requests;
        if (app_stop_after && app_requests > app_stop_after)
            return;
        if (app_cancel_after && app_requests == app_cancel_after) {
            static const uint8_t soft[] = { 0xf0, 0x22, 0x24, 0x35, 0x7d, 0xf7 };
            queue_bytes(soft, sizeof(soft));
            return;
        }
        respond(address, app+address, len);
        return;
    }
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
    if (asked_size) {
        queue_bytes(asked, asked_size);
        asked_size = 0;
        return false;
    }
    if (answered >= 0)
        return true;
    return cancel_now;
}

void mu_unit_info(struct mu_unit *unit) {
    static const uint8_t uid[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unit->jedec = 0x856014;
    unit->capacity = FLASH;
    memcpy(unit->uid, uid, 8);
    unit->kind = MU_STAGE;
    unit->version = "test";
}

bool mu_read(uint32_t address, void *out, uint32_t size) {
    assert(address+size <= FLASH);
    memcpy(out, flash+address, size);
    return true;
}

static void note_write(uint32_t address, uint32_t size) {
    if (address < MU_FIRST || address+size > MU_LIMIT || (address+size > MU_KEEP_FROM && address < MU_KEEP_TO))
        wrote_outside = true;
}

/* A power cut: at flash operation cut_at, half of it happens, and then nothing else does. */
static unsigned cut_at, flash_ops;
static jmp_buf cut;

static void maybe_cut(uint32_t address, const uint8_t *data, uint32_t size) {
    if (!cut_at || ++flash_ops != cut_at)
        return;
    for (uint32_t i = 0; i < size/2; ++i) {
        if (data)
            flash[address+i] &= data[i];
        else
            flash[address+i] = 0xff;
    }
    longjmp(cut, 1);
}

bool mu_erase(uint32_t address) {
    assert(!(address%MU_SECTOR));
    note_write(address, MU_SECTOR);
    maybe_cut(address, 0, MU_SECTOR);
    memset(flash+address, 0xff, MU_SECTOR);
    ++erases;
    if (!first_erase_order[address/MU_SECTOR])
        first_erase_order[address/MU_SECTOR] = ++order;
    return true;
}

bool mu_program(uint32_t address, const void *data, uint32_t size) {
    note_write(address, size);
    maybe_cut(address, data, size);
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

/* Whether the bootloader would start what's in flash, by its own checks. */
static bool core_read_flash(void *ctx, uint32_t address, void *out, uint32_t size) {
    (void)ctx;
    if (address > FLASH || size > FLASH-address)
        return false;
    memcpy(out, flash+address, size);
    return true;
}

static bool would_start(uint16_t key) {
    static uint8_t work[CORE_WORK_SIZE];
    static struct core_app app;
    static struct core_header header;
    struct core_flash f = { .read = core_read_flash, .capacity = FLASH };
    memset(&app, 0, sizeof(app));
    return core_app_mount_header(&f, key, &app, &header) && core_app_check(&f, &app, work) == CORE_APP_OK;
}

/* Cuts power at every flash operation in turn (every step-th, for a big image). Each time the boot and
 * key areas are intact, a part-written firmware can't be started, and installing again finishes it. */
static unsigned power_cuts(uint32_t size, uint16_t key, unsigned step, bool real) {
    static uint8_t old[FLASH], want[FLASH];
    bool written;
    volatile unsigned tried = 0;
    memcpy(old, flash, FLASH);
    memcpy(want, package+PACKAGE_OFFSET, size);
    for (volatile unsigned at = 1; ; at += step) {
        memcpy(flash, old, FLASH);
        cut_at = at;
        flash_ops = 0;
        cancel_after = soft_after = check_percent = write_percent = 0;
        erases = programs = 0;
        finish_seen = false;
        reply_size = 0;
        ask_upgrade();
        if (!setjmp(cut)) {
            enum mu_result r = mu_run(key, sector, &written);
            assert(r == MU_DONE);
            cut_at = 0;
            break;                      /* the install finished before this cut */
        }
        cut_at = 0;
        ++tried;
        assert(!memcmp(flash, old, MU_FIRST) && !memcmp(flash+MU_LIMIT, old+MU_LIMIT, FLASH-MU_LIMIT));
        if (real)
            assert(!would_start(key));
        else
            assert(memcmp(flash+MU_FIRST, want+MU_FIRST, MU_SECTOR));
        /* Every so often, go on and finish it, from the flash as the cut left it. */
        if (tried%(real ? 40:1) == 0) {
            finish_seen = false;
            check_percent = write_percent = erases = programs = 0;
            reply_size = 0;
            ask_upgrade();
            assert(mu_run(key, sector, &written) == MU_DONE && finish_seen);
            assert(!memcmp(flash+MU_FIRST, want+MU_FIRST, size-MU_FIRST));
            assert(!memcmp(flash, old, MU_FIRST));
            if (real)
                assert(would_start(key));
        }
    }
    return tried;
}

/* M-VAVE's packages aren't public: this part runs where they're in testdata. */
static uint32_t load_fwsc(const char *path, uint8_t *out, uint32_t *flash_offset, uint32_t *flash_size) {
    static uint8_t raw[0x200000];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t size = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    uint32_t n = 0, o = 0;
    while (n < 20 || raw[n*48+47] == 0x7d)
        ++n;
    for (uint32_t i = 0; i < n; ++i, o += 47)
        memcpy(out+o, raw+i*48, 47);
    memcpy(out+o, raw+n*48, size-n*48);
    o += (uint32_t)size-n*48;
    uint8_t head[0x40], entry[0x50];
    memcpy(head, out, 0x40);
    core_enc(0xffff, head, 0x40);
    for (uint32_t i = 0; i < (uint32_t)(head[8] | head[9] << 8); ++i) {
        memcpy(entry, out+0x40+i*0x50, 0x50);
        core_enc(0xffff, entry, 0x50);
        if (!core_le16(entry)) {
            *flash_offset = entry[8] | (uint32_t)entry[9] << 8 | (uint32_t)entry[10] << 16;
            *flash_size = entry[12] | (uint32_t)entry[13] << 8 | (uint32_t)entry[14] << 16;
        }
    }
    return o;
}

/* A host's request, framed as it would be, and the run it gets. */
static enum mu_result host_asks(uint8_t cmd, const uint8_t *body, uint32_t size) {
    static uint8_t plain[64];
    bool written;
    uint32_t sum = 0;
    uint8_t head[6] = { 0, 0x59, cmd, (uint8_t)size };
    memcpy(plain, head, 6);
    memcpy(plain+6, body, size);
    for (uint32_t i = 0; i < size; ++i)
        sum += body[i];
    plain[6+size] = (uint8_t)~sum;
    asked[0] = 0xf0;
    uint32_t n = pack7(plain, 7+size, asked+1);
    asked[n+1] = 0xf7;
    asked_size = n+2;
    answered = -1;
    enum mu_result r = mu_run(KEY, sector, &written);
    assert(!written && !erases && !programs);
    return r;
}

void mu_start_app(void) {
    ++apps_started;
    /* It really has what was offered, and a host hears before USB stops. */
    assert(answered == 0x25 && !memcmp(mu_test_app_ram, app, app_size-8));
}

static void make_app(uint32_t size, uint8_t board, uint8_t format) {
    for (uint32_t i = 0; i < size-8; ++i)
        app[i] = (uint8_t)(i*7+(i >> 9));
    uint16_t crc = core_crc16(0, app, size-8);
    memcpy(app+size-8, "APP1", 4);
    app[size-4] = (uint8_t)crc;
    app[size-3] = (uint8_t)(crc >> 8);
    app[size-2] = board;
    app[size-1] = format;
    app_size = size;
    app_requests = 0;
    apps_started = 0;
    app_stop_after = app_cancel_after = 0;
    app_first_read_seen = false;
}

static enum mu_result offer_app(const char *magic, uint32_t size) {
    uint8_t m[8] = { (uint8_t)magic[0], (uint8_t)magic[1], (uint8_t)magic[2], 0, (uint8_t)size,
        (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24) };
    answered = -1;
    return host_asks(0x25, m, 8);
}

static void test_apps(void) {
    reset_all();
#ifdef BOARD_SMK
    const uint8_t mine = 2, other = 1;
#else
    const uint8_t mine = 1, other = 2;
#endif
    /* It says it can. */
    assert(host_asks(0x12, 0, 0) == MU_CANCELLED);
    assert(answered == 0x12 && answer[7] == MU_RUNS_APPS);
    /* Any size, for this board or any, reads the trailer first and runs the app. */
    static const uint32_t sizes[] = { 16, 17, 512, 513, 520, 0x8000, MU_APP_MAX };
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
        for (unsigned any = 0; any < 2; ++any) {
            make_app(sizes[i], any ? 0:mine, 1);
            assert(offer_app("RUN", sizes[i]) == MU_CANCELLED);
            assert(apps_started == 1 && app_first_read == sizes[i]-8);
            assert(answer_size == 8 && !memcmp(answer, "RUN", 4));
            assert(app_requests == 1+(sizes[i]+511)/512);
        }
    /* A wrong trailer costs that one request, and starts nothing. */
    make_app(0x1000, other, 1);
    assert(offer_app("RUN", 0x1000) == MU_CANCELLED && !apps_started && answered == 0 && answer[0] == 1);
    assert(app_requests == 1);
    make_app(0x1000, mine, 2);
    assert(offer_app("RUN", 0x1000) == MU_CANCELLED && !apps_started && answered == 0 && app_requests == 1);
    make_app(0x1000, mine, 1);
    app[0x1000-8] = 'S';
    assert(offer_app("RUN", 0x1000) == MU_CANCELLED && !apps_started && answered == 0 && app_requests == 1);
    /* A wrong CRC is only seen at the end. */
    make_app(0x1000, mine, 1);
    app[0x123] ^= 1;
    assert(offer_app("RUN", 0x1000) == MU_CANCELLED && !apps_started && answered == 0 && answer[0] == 1);
    /* Sizes outside the limits, and a request that isn't one, aren't asked about. */
    make_app(0x1000, mine, 1);
    assert(offer_app("RUN", 15) == MU_CANCELLED && !apps_started && answered == 0 && !app_requests);
    assert(offer_app("RUN", MU_APP_MAX+1) == MU_CANCELLED && !apps_started && answered == 0 && !app_requests);
    assert(offer_app("RUX", 0x1000) == MU_CANCELLED && !apps_started && answered == 0 && !app_requests);
    static const uint8_t longer[9] = { 'R', 'U', 'N', 0, 0, 0x10, 0, 0, 0 };
    answered = -1;
    assert(host_asks(0x25, longer, 9) == MU_CANCELLED && !apps_started && answered == 0 && !app_requests);
    /* A host that stops partway starts nothing. */
    make_app(0x1000, mine, 1);
    app_stop_after = 3;
    assert(offer_app("RUN", 0x1000) == MU_CANCELLED && !apps_started && answered == 0);
    /* Download mode still gets out while loading. */
    make_app(0x1000, mine, 1);
    app_cancel_after = 3;
    assert(offer_app("RUN", 0x1000) == MU_DOWNLOAD && !apps_started);
    /* Nothing here goes near the flash. */
    assert(!erases && !programs && !memcmp(flash, original, FLASH));
    app_size = 0;
    answered = -1;
}

static void read_request(uint8_t cmd, uint8_t provider, uint32_t address, uint32_t count) {
    uint8_t m[8] = { provider, (uint8_t)address, (uint8_t)(address >> 8), (uint8_t)(address >> 16),
        (uint8_t)(address >> 24), (uint8_t)count, (uint8_t)(count >> 8), (uint8_t)(count >> 16) };
    assert(host_asks(cmd, m, 8) == MU_CANCELLED);
}

static void test_host_commands(void) {
    reset_all();
    for (uint32_t i = 0; i < FLASH; ++i)
        flash[i] = (uint8_t)(i*13+(i >> 8)+(i >> 16));
    /* What's answering. */
    assert(host_asks(0x12, 0, 0) == MU_CANCELLED);
    assert(answered == 0x12 && !memcmp(answer, "MBOOT", 5) && answer[5] == 1 && answer[6] == MU_STAGE);
    assert(core_le16(answer+8) == KEY && !memcmp(answer+28, "test", 5));
    assert(answer[10] == 0 && answer[11] == 0 && answer[12] == 0x10);
    assert(answer[14] == 0x14 && answer[15] == 0x60 && answer[16] == 0x85 && answer[18] == 1 && answer[25] == 8);
    /* Raw flash, the boot area and the key area too, each with what was asked echoed. */
    static const uint32_t places[][2] = { {0, 512}, {0x3f00, 512}, {0xff000, 512}, {FLASH-1, 1}, {FLASH-512, 512}, {0x12345, 7} };
    for (unsigned i = 0; i < sizeof(places)/sizeof(places[0]); ++i) {
        read_request(0x23, 0, places[i][0], places[i][1]);
        assert(answered == 0x23 && answer_size == 8+places[i][1]);
        assert(answer[0] == 0 && answer[1] == (uint8_t)places[i][0] && answer[5] == (uint8_t)places[i][1]);
        assert(!memcmp(answer+8, flash+places[i][0], places[i][1]));
    }
    /* Refused, with the common error. */
    static const uint32_t bad[][3] = { {0x23, 0, 0}, {0x23, 0, 513}, {0x23, FLASH, 1}, {0x23, FLASH-1, 2}, {0x23, 0xffffff00u, 512},
        {0x24, 0, 0}, {0x24, FLASH, 1}, {0x24, 1, FLASH}, {0x24, 0xfffffff0u, 32} };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        read_request((uint8_t)bad[i][0], 0, bad[i][1], bad[i][2]);
        assert(answered == 0 && answer_size == 1 && answer[0] == 1);
    }
    read_request(0x23, 1, 0, 16);
    assert(answered == 0 && answer[0] == 1);
    /* A body of another length isn't taken for a request. */
    static const uint8_t longer[9] = { 0, 0, 0, 0, 0, 16, 0, 0, 0 };
    assert(host_asks(0x23, longer, 9) == MU_CANCELLED);
    assert(answered == 0 && answer[0] == 1);
    /* The chip's CRC-32 of a range, across sectors. */
    read_request(0x24, 0, 0, FLASH);
    assert(answered == 0x24 && answer_size == 12);
    assert((uint32_t)(answer[8] | answer[9] << 8 | answer[10] << 16 | (uint32_t)answer[11] << 24) == (core_crc32(UINT32_MAX, flash, FLASH)^UINT32_MAX));
    read_request(0x24, 0, 0x1234, 0x2345);
    assert((uint32_t)(answer[8] | answer[9] << 8 | answer[10] << 16 | (uint32_t)answer[11] << 24) == (core_crc32(UINT32_MAX, flash+0x1234, 0x2345)^UINT32_MAX));
    /* The commands that erase and write on stock firmware are refused here. */
    static const uint8_t erase[5] = { 0, 0, 0x40, 0, 0 };
    static const uint8_t program[12] = { 0, 0, 0x40, 0, 0, 4, 0, 0, 1, 2, 3, 4 };
    assert(host_asks(0x21, erase, 5) == MU_CANCELLED && answered == 0 && answer[0] == 1);
    assert(host_asks(0x22, program, 12) == MU_CANCELLED && answered == 0 && answer[0] == 1);
    /* A restart. */
    bool written;
    static const uint8_t restart[] = { 0xf0, 0x22, 0x24, 0x35, 0x7e, 0xf7 };
    answered = -1;
    memcpy(asked, restart, sizeof(restart));
    asked_size = sizeof(restart);
    assert(mu_run(KEY, sector, &written) == MU_RESTART && !written && !erases);
    answered = -1;
}

int main(void) {
    bool written;
    test_host_commands();
    test_apps();
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
    /* The largest package allowed stays below what must be kept. */
    make_package(KEY, MU_LIMIT);
    reset_all();
    ask_upgrade();
    assert(mu_run(KEY, sector, &written) == MU_DONE && !wrote_outside && outside_same(MU_LIMIT));
#ifdef BOARD_SMK
    /* The SMK-37's Bluetooth pairing and calibration stay out of reach, even in a package covering them. */
    assert(!memcmp(flash+MU_KEEP_FROM, original+MU_KEEP_FROM, MU_KEEP_TO-MU_KEEP_FROM));
    assert(!memcmp(flash+MU_KEEP_TO, package+PACKAGE_OFFSET+MU_KEEP_TO, MU_LIMIT-MU_KEEP_TO));
#endif
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
    assert(!memcmp(identity, MU_IDENTITY, sizeof(MU_IDENTITY)));
    /* Power cuts, on a small package. */
    make_package(KEY, 0x20000);
    reset_all();
    unsigned cuts = power_cuts(0x20000, KEY, 1, false);
    /* And with real firmware, from one firmware to another. */
    static uint8_t old_image[0x200000];
    uint32_t old_offset = 0, old_size = 0, new_offset = 0, new_size = 0;
    uint32_t have_old = load_fwsc("testdata/fm1-015.fwsc", old_image, &old_offset, &old_size);
    uint32_t have_new = load_fwsc("testdata/smk37-016.fwsc", package, &new_offset, &new_size);
    if (have_old && have_new) {
        package_size = have_new;
        assert(new_offset == PACKAGE_OFFSET);
        reset_all();
        memcpy(flash, old_image+old_offset, old_size);
        assert(would_start(KEY));
        unsigned real = power_cuts(new_size, KEY, 1, true);
        printf("midi update: %u power cuts on real firmware\n", real);
    } else
        printf("SKIP: M-VAVE's packages not present (power cuts on real firmware)\n");
    printf("midi update: install, refusals, limits, order and %u power cuts passed\n", cuts);
    return 0;
}
