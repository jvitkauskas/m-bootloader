/* SPDX-License-Identifier: MIT */
/* Full boot/menu integration: this translation unit includes the production
 * boot.c so tests can reset its private BSS between simulated power cycles.
 * The register/NOR model and scenario approach are adapted from the other
 * clean-room implementation (a), under MIT; no original implementation is used.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/mman.h>
#include "boot_sim.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../src/boot.c"
#pragma GCC diagnostic pop
#define CHECK(c) do { ++t_checks; if (!(c)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); ++t_failures; } } while (0)
#define RET(off) ((uint8_t *)(uintptr_t)(0x01c7fd80u+(off)))
enum {
    END_APP = 1, END_UPDATE, END_RESET, END_TIMEOUT
};
static unsigned t_checks, t_failures, t_cases;
static jmp_buf t_escape;
static unsigned t_app_calls, t_update_calls, t_resets;
static bool t_app_returns, t_update_returns;
static bool t_bad_quad_on_return;
static void (*t_update_hook)(void);
static uint8_t t_app_object[24], t_app_header[32], t_reset_retained[256];
static uint8_t t_update_object[24], t_update_clock[52], t_update_memory[60];
static uint32_t t_app_entry, t_update_entry, t_update_end;
static uint32_t t_sfc[5], t_pin_snapshot[2][8], t_phy_snapshot;
static unsigned t_init_at_update, t_init_at_app;
static uint8_t t_request_crc_at_update[2];
static uint8_t t_rom_param[8];
static uint32_t t_device[4];
static struct {
    unsigned mask;
    uint64_t from, to;
} t_keys[64];
static unsigned t_nkeys;
static uint8_t t_fixture_cache[4][0x100000];
static bool t_fixture_present[4];
static const char *const t_fixture_names[] = {
    "fm1-stock", "smk37-stock", "smk37-snoop37", "smk37-baudgirl"
};
static const unsigned t_offsets[] = {
    0, 8, 12, 16, 20, 24, 28, 32
};
void boot_sim_call_app(uint32_t entry, void *object)
{
    sim_flush();
    ++t_app_calls;
    t_app_entry = entry;
    t_init_at_app = sim.init_count;
    memcpy(t_app_object, object, sizeof(t_app_object));
    memcpy(t_app_header, RET(160), sizeof(t_app_header));
    t_sfc[0] = sim_rd(0x40200);
    t_sfc[1] = sim_rd(0x40204);
    t_sfc[2] = sim_rd(0x4020c);
    t_sfc[3] = sim_rd(0x40300);
    t_sfc[4] = sim_rd(0x40304);
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned r = 0; r < 8; ++r)
            t_pin_snapshot[p][r] = sim_reg_get(0x50000+(p ? 7:0)*0x40+t_offsets[r]);
    t_phy_snapshot = sim_reg_get(0x16a0c);
    if (!t_app_returns)
        longjmp(t_escape, END_APP);
    if (t_bad_quad_on_return)
        sim.bad_quad = 1;
}

void boot_sim_call_updater(uint32_t entry, void *object, uint32_t end)
{
    sim_flush();
    ++t_update_calls;
    t_update_entry = entry;
    t_update_end = end;
    t_init_at_update = sim.init_count;
    memcpy(t_update_object, object, sizeof(t_update_object));
    memcpy(t_update_clock, clock_meta, sizeof(t_update_clock));
    memcpy(t_update_memory, memory_meta, sizeof(t_update_memory));
    memcpy(t_request_crc_at_update, RET(2), 2);
    if (t_update_hook)
        t_update_hook();
    if (!t_update_returns)
        longjmp(t_escape, END_UPDATE);
}

void boot_sim_reset(void)
{
    sim_flush();
    ++t_resets;
    memcpy(t_reset_retained, RET(0), sizeof(t_reset_retained));
    longjmp(t_escape, END_RESET);
}

static void t_timeout(void)
{
    longjmp(t_escape, END_TIMEOUT);
}

static unsigned t_keyfn(unsigned long long now)
{
    unsigned result = 0;
    for (unsigned i = 0; i < t_nkeys; ++i)
        if (now >= t_keys[i].from && now < t_keys[i].to)
            result |= t_keys[i].mask;
    return result;
}

static void t_key(unsigned mask, unsigned from, unsigned to)
{
    assert(t_nkeys < 64);
    t_keys[t_nkeys].mask = mask;
    t_keys[t_nkeys].from = (uint64_t)from*1000;
    t_keys[t_nkeys++].to = (uint64_t)to*1000;
}

static int t_rom_read(void *param, void *dest, uint32_t offset, uint32_t size)
{
    assert(core_range(offset, size, nor_size));
    memcpy(dest, sim_nor+offset, size);
    if (((uint8_t *)param)[1] == 1) {
        assert(!(size%32));
        for (unsigned i = 0; i < size; i += 32)
            core_enc(0xffff, (uint8_t *)dest+i, 32);
    }
    return 0;
}

static void t_reset_boot_state(void)
{
    memset(work, 0, sizeof(work));
    memset(request, 0, sizeof(request));
    memset(clock_meta, 0, sizeof(clock_meta));
    memset(memory_meta, 0, sizeof(memory_meta));
    memset(update_meta, 0, sizeof(update_meta));
    memset(&config, 0, sizeof(config));
    memset(&app, 0, sizeof(app));
    memset(&header, 0, sizeof(header));
    memset(&nor, 0, sizeof(nor));
    jedec = unit_key = trim = 0;
    memset(mac, 0, sizeof(mac));
    effective_width = 0;
    mounted = xip_attempted = screen_ready = false;
    reason = NULL;
}

static void t_prep(unsigned fixture)
{
    t_reset_boot_state();
    sim_reset();
    if (fixture < 4) {
        assert(t_fixture_present[fixture]);
        nor_load(t_fixture_cache[fixture], 0x100000);
    } else {
        /* Public malformed-image case: no fixture bytes are used. */
        memset(sim_nor, 0xff, 0x100000);
        nor_size = 0x100000;
        nor_budget = -1;
        nor_clear_log();
    }
    sim.board = SMK ? 2:1;
    sim.keyfn = t_keyfn;
    sim.on_timeout = t_timeout;
    sim.limit = 12000000;
    t_nkeys = 0;
    t_app_calls = t_update_calls = t_resets = 0;
    t_app_returns = t_update_returns = false;
    t_bad_quad_on_return = false;
    t_update_hook = NULL;
    memset(t_app_object, 0, sizeof(t_app_object));
    memset(t_reset_retained, 0, sizeof(t_reset_retained));
    memset(RET(0), 0, 256);
    memset((void *)(uintptr_t)CORE_UPDATE_LOAD, 0, CORE_UPDATE_MAX);
    memcpy((void *)(uintptr_t)0x01c02050, "1.0-abcdef0-dirty", sizeof("1.0-abcdef0-dirty"));
    memset(t_rom_param, 0, sizeof(t_rom_param));
    t_device[0] = (uint32_t)(uintptr_t)t_rom_param;
    t_device[1] = 0;
    t_device[2] = 1;
    t_device[3] = (uint32_t)(uintptr_t)t_rom_read;
    sim_p33_set(0x80, 0x31);
    sim_p33_set(9, 0xa5);
    sim_p33_set(6, 0xf8);
    sim_p33_set(5, 4);
    for (unsigned i = 0; i < 16; ++i)
        sim_p33_set(0xd0+i, 0xff);
    for (unsigned i = 0; i < 3; ++i)
        sim_p33_set(0xad+i, 0xff);
    sim.fuse[0][2] = 0x1234;
    sim.fuse[0][3] = UINT32_MAX;
    sim.fuse[1][3] = 0x00980f00;
    ++t_cases;
}

static int t_go(void)
{
    int end = setjmp(t_escape);
    if (!end) {
        boot_main(t_device);
        assert(!"boot_main returned");
    }
    CHECK(!sim.err);
    if (sim.err)
        fprintf(stderr, "simulator: %s\n", sim.errmsg);
    return end;
}

static bool t_usb_reset(void)
{
    return !memcmp(t_reset_retained, "usb_update_mode", 16);
}

static unsigned t_own_fixture(void)
{
    return SMK ? 1:0;
}

/* Decode the panel's actual RGB565 framebuffer using Appendix A's glyphs. */
static const char t_characters[] = " -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t t_glyphs[][5] = {
    {
        0, 0, 0, 0, 0
    }, {
        8, 8, 8, 8, 8
    }, {
        0, 0x60, 0x60, 0, 0
    }, {
        0x60, 0x10, 8, 4, 3
    }, {
        0, 0x36, 0x36, 0, 0
    },
    {
        0, 8, 0x14, 0x22, 0x41
    }, {
        0x41, 0x22, 0x14, 8, 0
    }, {
        8, 8, 0x3e, 8, 8
    }, {
        2, 1, 0x51, 9, 6
    },
    {
        0x3e, 0x51, 0x49, 0x45, 0x3e
    }, {
        0, 0x42, 0x7f, 0x40, 0
    }, {
        0x42, 0x61, 0x51, 0x49, 0x46
    },
    {
        0x21, 0x41, 0x45, 0x4b, 0x31
    }, {
        0x18, 0x14, 0x12, 0x7f, 0x10
    }, {
        0x27, 0x45, 0x45, 0x45, 0x39
    },
    {
        0x3c, 0x4a, 0x49, 0x49, 0x30
    }, {
        1, 0x71, 9, 5, 3
    }, {
        0x36, 0x49, 0x49, 0x49, 0x36
    }, {
        6, 0x49, 0x49, 0x29, 0x1e
    },
    {
        0x7e, 9, 9, 9, 0x7e
    }, {
        0x7f, 0x49, 0x49, 0x49, 0x36
    }, {
        0x3e, 0x41, 0x41, 0x41, 0x22
    },
    {
        0x7f, 0x41, 0x41, 0x41, 0x3e
    }, {
        0x7f, 0x49, 0x49, 0x49, 0x41
    }, {
        0x7f, 9, 9, 9, 1
    },
    {
        0x3e, 0x41, 0x49, 0x49, 0x7a
    }, {
        0x7f, 8, 8, 8, 0x7f
    }, {
        0, 0x41, 0x7f, 0x41, 0
    },
    {
        0x20, 0x40, 0x41, 0x3f, 1
    }, {
        0x7f, 8, 0x14, 0x22, 0x41
    }, {
        0x7f, 0x40, 0x40, 0x40, 0x40
    },
    {
        0x7f, 2, 0x0c, 2, 0x7f
    }, {
        0x7f, 4, 8, 0x10, 0x7f
    }, {
        0x3e, 0x41, 0x41, 0x41, 0x3e
    },
    {
        0x7f, 9, 9, 9, 6
    }, {
        0x3e, 0x41, 0x51, 0x21, 0x5e
    }, {
        0x7f, 9, 0x19, 0x29, 0x46
    },
    {
        0x46, 0x49, 0x49, 0x49, 0x31
    }, {
        1, 1, 0x7f, 1, 1
    }, {
        0x3f, 0x40, 0x40, 0x40, 0x3f
    },
    {
        0x1f, 0x20, 0x40, 0x20, 0x1f
    }, {
        0x3f, 0x40, 0x38, 0x40, 0x3f
    }, {
        0x63, 0x14, 8, 0x14, 0x63
    },
    {
        3, 4, 0x78, 4, 3
    }, {
        0x61, 0x51, 0x49, 0x45, 0x43
    }
};
static const char *t_row(unsigned row)
{
    static char text[21];
    for (unsigned c = 0; c < 20; ++c) {
        uint16_t bg = sim.fb[(row*16+15)*240+c*12+11];
        text[c] = '?';
        for (unsigned g = 0; g < sizeof(t_glyphs)/sizeof(t_glyphs[0]); ++g) {
            bool match = true;
            for (unsigned y = 0; y < 16 && match; ++y)
            for (unsigned x = 0; x < 12; ++x) {
                bool on = x < 10 && y < 14 && (t_glyphs[g][x/2]&(1u << (y/2)));
                if (on == (sim.fb[(row*16+y)*240+c*12+x] == bg)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                text[c] = t_characters[g];
                break;
            }
        }
    }
    text[20] = 0;
    for (unsigned n = 20; n && text[n-1] == ' '; --n)
        text[n-1] = 0;
    return text;
}

static bool t_row_is(unsigned row, const char *wanted)
{
    const char *got = t_row(row);
    bool equal = strlen(got) == strlen(wanted) && !memcmp(got, wanted, strlen(wanted));
    if (!equal)
        fprintf(stderr, "row %u: '%s', wanted '%s'\n", row, got, wanted);
    return equal;
}

static void t_damage_app(void)
{
    sim_nor[0x9000] ^= 0x10;
}

static void t_no_scan_request(void)
{
    memset(sim_nor+0xbef00, 0xff, 80);
}

static void t_ram_request(void)
{
    memcpy(RET(8), sim_nor+0xbef00, 80);
    assert(core_request_valid(RET(8)));
}

static void t_open_menu(void)
{
    t_key(KEY_SELECT, 0, 1200);
}

static void t_pick_bypass(void)
{
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 6100);
}

static void t_normal(void)
{
    for (unsigned f = SMK ? 1:0; f < (SMK ? 4u:1u); ++f) {
        if (!t_fixture_present[f])
            continue;
        t_prep(f);
        sim.jedec = SMK ? 0xeb6014:0x856014;
        sim.limit = 1000000;
        CHECK(t_go() == END_APP);
        CHECK(sim.startup_us > 0 && sim.startup_us < 100000);
        CHECK(t_app_calls == 1 && !t_update_calls && !t_resets);
        CHECK(t_app_entry == 0x02000120);
        CHECK(core_le32(t_app_object) == 0x01c7fe20);
        CHECK(core_le32(t_app_object+4) == 0x4000);
        CHECK(core_le32(t_app_object+8) == 0x02000000);
        CHECK(core_le16(t_app_object+12) == 0x980f);
        CHECK(core_le16(t_app_object+14) == (uint16_t)(0x1234 | (UINT32_MAX&0xfff238fc)));
        CHECK(!memcmp(t_app_object+16, t_fixture_cache[f]+0xfff40, 8));
        uint8_t decoded[32];
        memcpy(decoded, t_fixture_cache[f], 32);
        core_enc(0xffff, decoded, 32);
        CHECK(!memcmp(decoded, t_app_header, 32));
        CHECK(t_sfc[0] == 0x00980381 && t_sfc[1] == 1 && t_sfc[2] == 0x4000);
        CHECK(t_sfc[3] == 1 && t_sfc[4] == 0x980f);
        CHECK((sim_reg_get(0x1eee008)&0x300) == 0x300);
        CHECK(!sim.ncmd && sim.now < 700000);
        CHECK(sim_p33_get(0x80) == 0x21);
        CHECK(sim_p33_get(9) == 0xab && sim_p33_get(6) == 0xfb && sim_p33_get(5) == 4);
        for (unsigned i = 0; i < 16; ++i)
            CHECK(!sim_p33_get(0xd0+i));
        for (unsigned i = 0; i < 3; ++i)
            CHECK(!sim_p33_get(0xad+i));
        CHECK(sim_reg_get(0x119a0) == 0x45400203);
        CHECK(sim_reg_get(0x119a4) == 0x3f503026);
        CHECK(sim_reg_get(0x119a8) == 0x0940022b);
        CHECK(sim_reg_get(0x119ac) == 0x0750310c);
        CHECK(sim.dma_dummy_quad > 0);
        if (SMK)
            CHECK(!memcmp(sim_nor+0xff010, t_fixture_cache[f]+0xc1000, 0x22c));
        else
            CHECK(!nor_nlog);
        CHECK(!core_le32(RET(216)) && !core_le32(RET(220)) && !core_le32(RET(224)));
        if (SMK)
            CHECK(!sim.phy_writes);
    }
}

static void t_keys_and_menu(void)
{
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 1400);
    CHECK(t_go() == END_RESET && t_usb_reset());
    CHECK(!sim.ncmd && !t_app_calls && !nor_nlog);
    CHECK(!sim.init_count && !sim.dma_reads);
    CHECK(sim.now >= 1000000 && sim.now < 1200000);
    if (SMK)
        CHECK(!sim.phy_writes);
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY | KEY_SELECT, 0, 1400);
    CHECK(t_go() == END_RESET && t_usb_reset());
    CHECK(!sim.ncmd);
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 700);
    CHECK(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 400);
    t_key(KEY_RECOVERY, 415, 1400);
    CHECK(t_go() == END_RESET && t_usb_reset());
    t_prep(t_own_fixture());
    t_key(KEY_RECOVERY, 0, 400);
    t_key(KEY_RECOVERY, 450, 1400);
    CHECK(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 700);
    CHECK(t_go() == END_APP);
    CHECK(!sim.ncmd);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 400);
    t_key(KEY_SELECT, 415, 1400);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && sim.panel_on && !t_app_calls);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 400);
    t_key(KEY_SELECT, 450, 1400);
    CHECK(t_go() == END_APP && !sim.ncmd);
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 5000);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_keyfn(sim.now) == KEY_SELECT);
    CHECK(!t_app_calls && !t_update_calls && !t_resets);
    CHECK(t_row_is(0, SMK ? "SMK-37 M-BOOTLOADER":"FM-1 M-BOOTLOADER"));
    CHECK(t_row_is(1, "1.0-ABCDEF0-DIRTY"));
    CHECK(t_row_is(3, "APP OK"));
    CHECK(t_row_is(8, "> BOOT APP"));
    CHECK(t_row_is(9, "  DOWNLOAD MODE"));
    CHECK(t_row_is(10, "  WIPE USER DATA"));
    CHECK(t_row_is(13, ""));
    CHECK(t_row_is(14, SMK ? "< > MOVE SEQ PLAY OK":"OCT-/+ MOVE PLAY OK"));
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 2000);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && !t_update_calls && !t_resets);
    CHECK(t_row_is(8, "> BOOT APP"));
    t_prep(t_own_fixture());
    t_key(KEY_SELECT, 0, 2000);
    t_key(KEY_SELECT, 2300, 2400);
    CHECK(t_go() == END_APP);
    CHECK(t_app_calls == 1 && !t_update_calls && !t_resets);
    t_prep(t_own_fixture());
    t_open_menu();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls);
    CHECK(t_row_is(0, SMK ? "SMK-37 M-BOOTLOADER":"FM-1 M-BOOTLOADER"));
    CHECK(t_row_is(3, "APP OK"));
    CHECK(t_row_is(8, "> BOOT APP"));
    CHECK(t_row_is(9, "  DOWNLOAD MODE"));
    CHECK(t_row_is(10, "  WIPE USER DATA"));
    CHECK(sim.panel_on && sim.fb_pix >= 240*240);
    CHECK(sim.lit_us[0] && sim.lit_us[1] && sim.lit_us[2] && !sim.lit_us[3]);
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_SELECT, 2000, 2100);
    CHECK(t_go() == END_APP);
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    CHECK(t_go() == END_RESET && t_usb_reset());
    CHECK(t_row_is(3, "DOWNLOAD MODE"));
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1602);
    sim.limit = 2300000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_row_is(8, "> BOOT APP"));
}

static void t_bad_apps_and_bypass(void)
{
    t_prep(t_own_fixture());
    t_damage_app();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(2, ""));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  BOOT ANYWAY"));
    t_prep(t_own_fixture());
    memset(sim_nor+0x24000, 0xff, 0x1000);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_row_is(3, "APP INCOMPLETE"));
    CHECK(t_row_is(2, ""));
    t_prep(t_own_fixture());
    sim_nor[0x60] ^= 1;
    t_no_scan_request();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_row_is(3, "APP NOT FOUND"));
    CHECK(t_row_is(2, ""));
    CHECK(!t_app_calls && !t_update_calls);
    t_prep(t_own_fixture());
    t_damage_app();
    memcpy(RET(0), "BOOTANY", 8);
    CHECK(t_go() == END_APP);
    CHECK(!core_le32(RET(0)) && !core_le32(RET(4)));
    uint8_t consumed[8];
    memcpy(consumed, RET(0), 8);
    t_prep(t_own_fixture());
    t_damage_app();
    memcpy(RET(0), consumed, 8);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && !t_app_calls);
    t_prep(t_own_fixture());
    t_damage_app();
    t_pick_bypass();
    CHECK(t_go() == END_RESET);
    CHECK(!t_app_calls && sim.xip_count == 1);
    CHECK(!memcmp(t_reset_retained, "BOOTANY", 8));
    t_prep(t_own_fixture());
    t_damage_app();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 4000);
    t_key(KEY_PREV, 3200, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT && !t_app_calls && !t_resets);
    t_prep(t_own_fixture());
    t_app_returns = true;
    t_key(KEY_SELECT, 2500, 2600);
    CHECK(t_go() == END_RESET);
    CHECK(t_app_calls == 1 && sim.xip_count == 1 && !t_usb_reset());
    t_prep(t_own_fixture());
    t_damage_app();
    t_app_returns = true;
    memcpy(RET(0), "BOOTANY", 8);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && t_app_calls == 1);
    CHECK(!core_le32(RET(0)) && !core_le32(RET(4)));
    CHECK(t_row_is(2, "APP EXITED"));
}

static void t_updater_damage(void)
{
    t_damage_app();
    /* An updater owns the hardware and may change it. Reinitialization must
     * replace these values before any application read or XIP attempt. */
    sim_wr(0x11c00, 0);
    sim_wr(0x13000, 0x12345678);
}

static void t_updater_repair(void)
{
    memcpy(sim_nor+0x60, t_fixture_cache[1]+0x60, 32);
    uint8_t changed[32];
    memcpy(changed, sim_nor, 32);
    core_enc(0xffff, changed, 32);
    changed[16] ^= 1;
    core_put_le16(changed, core_crc16(0, changed+2, 30));
    core_enc(0xffff, changed, 32);
    memcpy(sim_nor, changed, 32);
    sim_wr(0x11c00, 0);
}

static void t_updates(void)
{
    t_prep(1);
    t_ram_request();
    CHECK(t_go() == END_UPDATE);
    CHECK(t_update_calls == 1 && !t_app_calls);
    CHECK(t_update_entry == CORE_UPDATE_LOAD && t_update_end > 0xb9040);
    CHECK(core_crc16(0, (void *)(uintptr_t)CORE_UPDATE_LOAD, 23324) == 0xb295);
    CHECK(core_le32(t_update_object) == 0x02000120);
    CHECK(core_le16(t_update_object+12) == 0x980f);
    CHECK(core_le32(t_update_object+20) == 0x01c7fdb0);
    CHECK(!core_le16(t_request_crc_at_update));
    CHECK(!memcmp(t_update_clock+28, "", 1));
    t_prep(1);
    t_ram_request();
    t_update_returns = true;
    CHECK(t_go() == END_APP);
    CHECK(t_update_calls == 1 && t_app_calls == 1);
    CHECK(!core_le16(RET(8)));
    CHECK(t_init_at_app > t_init_at_update);
    t_prep(1);
    t_ram_request();
    t_update_returns = true;
    t_update_hook = t_updater_damage;
    sim.limit = 3500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_update_calls == 1 && !t_app_calls);
    CHECK(!core_le16(RET(8)));
    CHECK(t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(2, "UPDATE RETURNED"));
    CHECK(!sim.xip_count);
    CHECK(sim.init_count > t_init_at_update);
    t_prep(1);
    t_ram_request();
    core_put_le32(RET(8+72), 0xb8000);
    core_put_le16(RET(8), core_crc16(0, RET(10), 78));
    CHECK(t_go() == END_APP);
    CHECK(!t_update_calls && !core_le16(RET(8)));
    CHECK(sim.init_count >= 3);
    t_prep(1);
    t_ram_request();
    core_put_le32(RET(8+72), 0xfffffff0);
    core_put_le16(RET(8), core_crc16(0, RET(10), 78));
    t_damage_app();
    sim.limit = 3500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_update_calls && !t_app_calls && !core_le16(RET(8)));
    CHECK(t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(2, "UPDATER REFUSED"));
    CHECK(!sim.xip_count);
    t_prep(1);
    sim_nor[0x60] ^= 1;
    CHECK(t_go() == END_UPDATE);
    CHECK(t_update_calls == 1 && !core_le32(t_update_object));
    CHECK(core_le32(t_update_object+20) == (uint32_t)(uintptr_t)memory_meta);
    t_prep(1);
    sim_nor[0x60] ^= 1;
    t_update_returns = true;
    sim.limit = 3500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_update_calls == 1 && !t_app_calls);
    CHECK(t_row_is(3, "APP NOT FOUND"));
    CHECK(sim.init_count > t_init_at_update);
    t_prep(1);
    sim_nor[0x60] ^= 1;
    sim_nor[0xb9040+8+500] ^= 0x20;
    sim.limit = 3500000;
    CHECK(t_go() == END_TIMEOUT && !t_update_calls);
    CHECK(t_row_is(3, "APP NOT FOUND"));
    /* An updater can repair metadata and change the header; both are re-read. */
    t_prep(1);
    t_ram_request();
    sim_nor[0x60] ^= 1;
    t_update_returns = true;
    t_update_hook = t_updater_repair;
    CHECK(t_go() == END_APP);
    CHECK(t_update_calls == 1 && t_app_calls == 1);
    CHECK(t_init_at_app > t_init_at_update);
    uint8_t fresh_header[32];
    memcpy(fresh_header, sim_nor, 32);
    core_enc(0xffff, fresh_header, 32);
    CHECK(!memcmp(t_app_header, fresh_header, 32));
    CHECK(!core_le16(RET(8)));
    /* The same cycle returns through the app and menu: the request stays used. */
    t_prep(1);
    t_ram_request();
    t_update_returns = t_app_returns = true;
    t_key(KEY_SELECT, 3500, 3600);
    CHECK(t_go() == END_RESET);
    CHECK(t_update_calls == 1 && t_app_calls == 1 && sim.xip_count == 1);
    CHECK(!core_le16(t_reset_retained+8));
    /* First matching magic+CRC, type zero: do not skip to older valid data. */
    t_prep(1);
    sim_nor[0x60] ^= 1;
    memcpy(sim_nor+0xfef00, sim_nor+0xbef00, 80);
    core_put_le16(sim_nor+0xfef02, 0);
    core_put_le16(sim_nor+0xfef00, core_crc16(0, sim_nor+0xfef02, 78));
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT && !t_update_calls);
    CHECK(t_row_is(5, "UPDATE 2 FLASH"));
}

static void t_uart(void)
{
    static const char *const commands[] = {
        "UARTUPDATE", "UART_UPDATE_CUSTOM"
    };
    for (unsigned i = 0; i < 2; ++i) {
        t_prep(t_own_fixture());
        memcpy(RET(0), commands[i], strlen(commands[i])+1);
        CHECK(t_go() == END_RESET && !t_usb_reset());
        CHECK(!core_le32(t_reset_retained));
        CHECK(!memcmp(t_reset_retained+4, commands[i]+4, strlen(commands[i])-3));
        uint8_t keep[24];
        memcpy(keep, t_reset_retained, 24);
        t_prep(t_own_fixture());
        memcpy(RET(0), keep, 24);
        CHECK(t_go() == END_APP);
    }
    t_prep(t_own_fixture());
    memcpy(RET(0), "UARTUPDATEX", 12);
    CHECK(t_go() == END_APP);
    t_prep(t_own_fixture());
    memcpy(RET(0), "UARTUPDATE", 11);
    sim_nor[0x60] ^= 1;
    t_no_scan_request();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && !t_resets);
    CHECK(!memcmp(RET(0), "UARTUPDATE", 11));
}

static void t_change_region(const char *name, uint32_t address, uint32_t length)
{
    for (unsigned off = 32; off < 9*32; off += 32) {
        uint8_t record[32];
        memcpy(record, sim_nor+0x4000+off, 32);
        core_enc((uint16_t)(0x980f ^ (off >> 2)), record, 32);
        if (memcmp(record+16, name, strlen(name)+1))
            continue;
        core_put_le32(record+4, address);
        core_put_le32(record+8, length);
        core_put_le16(record, core_crc16(0, record+2, 30));
        core_enc((uint16_t)(0x980f ^ (off >> 2)), record, 32);
        memcpy(sim_nor+0x4000+off, record, 32);
        return;
    }
    assert(!"region declaration not found");
}

static void t_wipe_confirmation(void)
{
    t_open_menu();
    t_key(KEY_PREV, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 6300);
}

static void t_confirmation_cosmetics(void)
{
    const unsigned deadlines[] = {2400000, 3500000, 6000000};
    for (unsigned wipe = 0; wipe < 2; ++wipe) {
        for (unsigned phase = 0; phase < 3; ++phase) {
            t_prep(t_own_fixture());
            if (wipe)
                t_wipe_confirmation();
            else {
                t_damage_app();
                t_pick_bypass();
            }
            sim.limit = deadlines[phase];
            CHECK(t_go() == END_TIMEOUT);
            CHECK(!t_app_calls && !t_update_calls && !t_resets);
            CHECK(t_row_is(8, wipe ? "WIPE USER DATA":"BOOT ANYWAY"));
            const char *prompt = phase == 2 ?
                (SMK ? "RELEASE SEQ PLAY":"RELEASE PLAY") :
                (SMK ? "HOLD SEQ PLAY 3 S":"HOLD PLAY 3 S");
            CHECK(t_row_is(14, prompt));
            unsigned stale = 0, invalid = 0;
            bool green = false, grey = false;
            for (unsigned y = 208; y < 224; ++y) {
                for (unsigned x = 0; x < 240; ++x) {
                    uint16_t pixel = sim.fb[y*240+x];
                    if (y < 210 || y >= 222)
                        stale += pixel != 0;
                    else {
                        invalid += pixel != GREEN && pixel != GREY;
                        green |= pixel == GREEN;
                        grey |= pixel == GREY;
                    }
                }
            }
            CHECK(!stale);
            CHECK(!invalid);
            CHECK(green == (phase != 0));
            CHECK(grey == (phase != 2));
            for (unsigned i = 0; i < nor_nlog; ++i)
                CHECK(nor_log[i].kind != 'E');
        }
    }
}

static void t_guard_and_wipe(void)
{
    if (SMK) {
        t_prep(1);
        CHECK(t_go() == END_APP);
        CHECK(!memcmp(sim_nor+0xff000, "TRIMCOPY", 8));
        uint8_t copied[0x22c];
        memcpy(copied, sim_nor+0xc1000, sizeof(copied));
        memset(sim_nor+0xc1000, 0xff, 0x1000);
        sim_nor[0x60] ^= 1;
        t_no_scan_request();
        t_reset_boot_state();
        memset(RET(0), 0, 256);
        memset(&sim.fb, 0, sizeof(sim.fb));
        sim.now = 0;
        sim.limit = 3000000;
        sim.err = 0;
        t_app_calls = t_update_calls = 0;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(!memcmp(sim_nor+0xc1000, copied, sizeof(copied)));
        CHECK(!t_app_calls && !t_update_calls);
    }
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_PREV, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 6300);
    CHECK(t_go() == END_RESET && !t_usb_reset());
    CHECK(t_row_is(13, "DONE"));
    unsigned changed = 0;
    for (unsigned i = 0; i < 0x100000; ++i) {
        bool erase = SMK ? (i >= 0x9d000 && i < 0xc1000):(i >= 0x93000 && i < 0xea000);
        if (SMK && i >= 0xff000 && i < 0xff23c)
            continue;
        if (sim_nor[i] != (erase ? 0xff:t_fixture_cache[t_own_fixture()][i]))
            ++changed;
    }
    CHECK(!changed);
    CHECK(!nor_touched(0xc1000, 0x1000) || !SMK);
    CHECK(!nor_touched(0xfff00, 0x100));
    if (SMK) {
        t_prep(1);
        t_change_region("VM", 0xc0000, 0x3000);
        t_change_region("BTIF", 0, 0);
        t_wipe_confirmation();
        sim.limit = 3000000;
        CHECK(t_go() == END_TIMEOUT && !t_resets);
        CHECK(t_row_is(8, "WIPE USER DATA"));
        CHECK(t_row_is(9, "ERASE C0000-C0FFF"));
        CHECK(t_row_is(10, "ERASE C2000-C2FFF"));
        CHECK(t_row_is(11, ""));
        CHECK(!nor_touched(0xc0000, 0x3000));
        t_prep(1);
        t_change_region("VM", 0xc0000, 0x3000);
        t_change_region("BTIF", 0, 0);
        t_wipe_confirmation();
        CHECK(t_go() == END_RESET && !t_usb_reset());
        CHECK(nor_touched(0xc0000, 0x1000));
        CHECK(nor_touched(0xc2000, 0x1000));
        CHECK(!nor_touched(0xc1000, 0x1000));
        for (unsigned i = 0; i < nor_nlog; ++i)
            if (nor_log[i].kind == 'E')
                CHECK(nor_log[i].len == 0x1000 && (nor_log[i].addr == 0xc0000 || nor_log[i].addr == 0xc2000));
        t_prep(1);
        t_change_region("VM", 1, 0x1000);
        t_change_region("BTIF", 0xc0000, 0x1000);
        t_wipe_confirmation();
        CHECK(t_go() == END_RESET && !t_usb_reset());
        CHECK(nor_touched(0xc0000, 0x1000));
        CHECK(!nor_touched(0xc1000, 0x1000));
        for (unsigned i = 0; i < nor_nlog; ++i)
            if (nor_log[i].kind == 'E')
                CHECK(nor_log[i].addr == 0xc0000 && nor_log[i].len == 0x1000);
    }
    t_prep(t_own_fixture());
    t_open_menu();
    t_key(KEY_PREV, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    t_key(KEY_SELECT, 2600, 4300);
    t_key(KEY_NEXT, 3500, 3600);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT && !t_resets);
    for (unsigned i = 0; i < nor_nlog; ++i)
        CHECK(nor_log[i].kind != 'E');
}

static void t_put_config(const uint8_t *blob, unsigned size)
{
    uint8_t descriptor[32];
    memcpy(descriptor, sim_nor+0x40, 32);
    core_enc(0xffff, descriptor, 32);
    core_put_le16(descriptor+2, core_crc16(0, blob, size));
    core_put_le32(descriptor+4, 0x38d0);
    core_put_le32(descriptor+8, size);
    core_put_le16(descriptor, core_crc16(0, descriptor+2, 30));
    core_enc(0xffff, descriptor, 32);
    memcpy(sim_nor+0x40, descriptor, 32);
    memcpy(sim_nor+0x38d0, blob, size);
}

static void t_config_and_flash(void)
{
    static const char *const dropped[] = {
        "PORT_INPUT", "PORT_OUTPUT", "ERR_PORT_OUTPUT", "BOOT_DELAY",
        "UTRX", "UTTX", "DVDD", "VDDIO", "DCDC14"
    };
    t_prep(t_own_fixture());
    uint8_t blob[1024];
    unsigned pos = 699;
    memcpy(blob, sim_nor+0x38d0, pos);
    for (unsigned i = 0; i < sizeof(dropped)/sizeof(dropped[0]); ++i) {
        blob[pos++] = 4;
        unsigned size = (unsigned)strlen(dropped[i])+1;
        memcpy(blob+pos, dropped[i], size);
        pos += size;
        memset(blob+pos, 0xff, 4);
        pos += 4;
    }
    t_put_config(blob, pos);
    CHECK(t_go() == END_APP);
    CHECK(sim_p33_get(9) == 0xab && sim_p33_get(6) == 0xfb && sim_p33_get(5) == 4);
    CHECK(sim.now < 700000);
    t_prep(t_own_fixture());
    sim_nor[0x38d0+100] ^= 1;
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && !t_app_calls);
    CHECK(t_row_is(2, "BAD CONFIG"));
    t_prep(t_own_fixture());
    uint8_t record[32];
    memcpy(record, sim_nor+0x40, 32);
    core_enc(0xffff, record, 32);
    core_put_le32(record+8, 0x2000);
    core_put_le16(record, core_crc16(0, record+2, 30));
    core_enc(0xffff, record, 32);
    memcpy(sim_nor+0x40, record, 32);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && t_row_is(2, "BAD CONFIG"));
    t_prep(t_own_fixture());
    sim.bad_quad = 1;
    CHECK(t_go() == END_APP);
    CHECK(t_sfc[0] == 0x00980281 && effective_width == 2);
    CHECK(sim.dma_dummy_dual > 0);
    t_prep(t_own_fixture());
    sim.bad_quad = sim.bad_dual = 1;
    CHECK(t_go() == END_APP);
    CHECK(t_sfc[0] == 0x00900089 && effective_width == 1);
    t_prep(t_own_fixture());
    t_app_returns = true;
    t_bad_quad_on_return = true;
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_app_calls == 1 && sim.xip_count == 1);
    CHECK(t_sfc[0] == 0x00980381);
    CHECK(effective_width == 2);
    CHECK(t_row_is(13, "SPI WIDTH FALLBACK"));
    t_prep(t_own_fixture());
    sim.bad_quad = 1;
    t_open_menu();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_row_is(13, "SPI WIDTH FALLBACK"));
    t_prep(t_own_fixture());
    sim.sr2 = 0;
    sim.ignore_wrsr = 1;
    CHECK(t_go() == END_APP && effective_width == 2);
    t_prep(t_own_fixture());
    sim.bad_quad = 1;
    t_damage_app();
    memcpy(RET(0), "BOOTANY", 8);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT && !t_app_calls);
    CHECK(t_row_is(3, "APP BAD CRC"));
}

static void t_pin_restoration(void)
{
    for (unsigned menu_path = 0; menu_path < 2; ++menu_path) {
        t_prep(t_own_fixture());
        uint32_t saved[2][8];
        for (unsigned p = 0; p < 2; ++p) {
            for (unsigned r = 0; r < 8; ++r) {
                uint32_t value = t_offsets[r] == 8 ? 0xffff:((0x1234*(r+1)+p*0x111)&0xffff);
                if (!p && !r)
                    value &= ~(BIT(1) | BIT(3));
                saved[p][r] = value;
                sim_wr(0x50000+(p ? 7:0)*0x40+t_offsets[r], value);
            }
        }
        sim_wr(0x16a0c, 0x00ab0000);
        sim.phy_writes = 0;
        if (menu_path) {
            t_open_menu();
            t_key(KEY_SELECT, 2000, 2100);
        } else
            t_key(KEY_SELECT, 0, 500);
        CHECK(t_go() == END_APP);
        for (unsigned p = 0; p < 2; ++p) {
            for (unsigned r = 0; r < 8; ++r) {
                uint32_t mask = (!SMK && menu_path && !p && (r == 0 || r == 1)) ? ~BIT(2):UINT32_MAX;
                CHECK((t_pin_snapshot[p][r]&mask) == (saved[p][r]&mask));
            }
        }
        CHECK(t_phy_snapshot == 0x00ab0000);
        if (SMK && !menu_path)
            CHECK(!sim.phy_writes);
    }
}

static void t_fm1_matrix_enable(void)
{
    if (SMK)
        return;
    for (unsigned state = 0; state < 4; ++state) {
        t_prep(4);
        hal_early();
        CHECK(apply_config());
        uint32_t original_out = BIT(5) | (state&1 ? BIT(2):0);
        uint32_t original_dir = 0xffff & ~(state&2 ? BIT(2):0);
        sim_wr(0x50000, original_out);
        sim_wr(0x50008, original_dir);
        unsigned all_keys = KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY;
        t_key(all_keys, 0, 1000);
        board_init(false);
        CHECK(!(sim_rd(0x50000)&BIT(2)));
        CHECK(!(sim_rd(0x50008)&BIT(2)));
        CHECK(board_keys() == all_keys);
        board_wait(3, true);
        CHECK(board_keys() == all_keys);
        /* Every key is open with PA2 high, an input, or both. */
        for (unsigned disabled = 1; disabled < 4; ++disabled) {
            sim_wr(0x50000, (sim_reg_get(0x50000)&~BIT(2)) | (disabled&1 ? BIT(2):0));
            sim_wr(0x50008, (sim_reg_get(0x50008)&~BIT(2)) | (disabled&2 ? BIT(2):0));
            CHECK(!board_keys());
        }
        gpio_output(0, 2, false);
        CHECK(board_keys() == all_keys);
        board_restore();
        sim_flush();
        CHECK(sim_reg_get(0x50000) == original_out);
        CHECK(sim_reg_get(0x50008) == original_dir);
        CHECK(!sim.err && !sim.ncmd && !nor_nlog);
    }
}

static void t_public_recovery(void)
{
    t_prep(4);
    t_key(KEY_RECOVERY, 0, 1400);
    CHECK(t_go() == END_RESET && t_usb_reset());
    CHECK(!sim.ncmd && !t_app_calls && !t_update_calls && !nor_nlog);
    CHECK(!sim.init_count && !sim.dma_reads);
    t_prep(4);
    t_key(KEY_RECOVERY | KEY_SELECT, 0, 1400);
    CHECK(t_go() == END_RESET && t_usb_reset());
    CHECK(!sim.ncmd && !t_app_calls && !t_update_calls);
    t_prep(4);
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && !t_update_calls && !t_resets && !nor_nlog);
    CHECK(t_row_is(0, SMK ? "SMK-37 M-BOOTLOADER":"FM-1 M-BOOTLOADER"));
    CHECK(t_row_is(1, "1.0-ABCDEF0-DIRTY"));
    CHECK(t_row_is(2, "BAD CONFIG"));
    CHECK(t_row_is(3, "APP NOT FOUND"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE"));
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    void *ram = mmap((void *)(uintptr_t)0x01c00000, 0x80000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(ram == (void *)(uintptr_t)0x01c00000);
    void *xip = mmap((void *)(uintptr_t)0x02000000, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(xip == (void *)(uintptr_t)0x02000000);
    for (unsigned f = 0; f < 4; ++f) {
        if ((SMK && f == 0) || (!SMK && f > 1))
            continue;
        char path[80];
        snprintf(path, sizeof(path), "testdata/%s.bin", t_fixture_names[f]);
        FILE *input = fopen(path, "rb");
        if (!input && errno == ENOENT) {
            printf("SKIP: %s.bin not present (boot fixture groups)\n", t_fixture_names[f]);
            continue;
        }
        assert(input && fread(t_fixture_cache[f], 1, 0x100000, input) == 0x100000);
        assert(fgetc(input) == EOF);
        assert(!fclose(input));
        t_fixture_present[f] = true;
    }
    puts(SMK ? "boot simulation: SMK-37":"boot simulation: FM-1");
    t_fm1_matrix_enable();
    t_public_recovery();
    t_normal();
    if (t_fixture_present[t_own_fixture()]) {
        t_keys_and_menu();
        t_bad_apps_and_bypass();
        t_uart();
        t_confirmation_cosmetics();
        t_guard_and_wipe();
        t_config_and_flash();
        t_pin_restoration();
    }
    if (t_fixture_present[1])
        t_updates();
    printf("boot simulation: %u scenarios, %u checks, %u failures\n", t_cases, t_checks, t_failures);
    return t_failures ? 1:0;
}
