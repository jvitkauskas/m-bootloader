/* SPDX-License-Identifier: MIT */
/* The menu's MIDI mode by default (src/boot.c midi_auto): boot.c built with MIDI_MODE on the boot simulator, the
 * USB MIDI side stubbed (no installer connects: the MIDI loop only polls its cancel). Built for the FM-1 and, with
 * BOARD_SMK, for the SMK-37 (its own binary and output line).
 * Without any fixture, on a blank flash (APP NOT FOUND, the menu comes up by itself) and on a synthetic app:
 *   - the menu that comes up by itself opens in MIDI mode, the app's state and the reason in view, the menu's keys
 *     named, the app not run;
 *   - each of the four keys goes back to the whole menu: its actions as before, the key not taken as a menu step;
 *   - a key that stays down can't hang the unit: pressed in MIDI mode and held, the menu still comes and doesn't take
 *     it as a press; down before MIDI mode opens, PLAY included, it neither keeps MIDI mode from opening nor leaves
 *     it, until it has read as up for 50 scans in a row;
 *   - an install that fails with nothing written doesn't take MIDI mode opened by itself away: it answers again,
 *     the failure in view, and only a key leaves; chosen from the menu, it goes back to the menu as before; one that
 *     fails after writing restarts the unit, never answering again over a half-written app;
 *   - a flash error (and on the SMK-37 a calibration guard error) opens MIDI mode with its reason in view, also after
 *     a RAM app has run and left the screen black;
 *   - an app that returns to the bootloader, the panel left off: MIDI mode, APP EXITED, and the menu after it;
 *   - PLAY held at power-on opens the menu itself, no MIDI mode.
 * With the board's flash dump (argv[1], else testdata/<board>-stock.bin, 1 MiB) as an extra, its app damaged (one
 * bit in app.bin: APP BAD CRC): the same, and BOOT ANYWAY still reached. A missing dump is a SKIP of the extra only;
 * with REQUIRE_FIXTURES set in the environment it fails.
 * Not here: USB and the clocks. The simulator has no USB controller, and src/usb_midi.c doesn't build for the host,
 * so what an exiting app leaves in them is for hardware (docs/SPEC.md §12.5). */
#define MIDI_MODE 1
/* The RAM app mu_start_app() calls is t_ram_app (below), under the name MU_APP_RAM has here. */
#define MU_TEST_APP_RAM 1
#include "midi_update.h"
#include "usb_midi.h"
#define main boot_test_main
#include "boot_test.c"
#undef main

static unsigned t_midi_runs, t_usb_starts, t_usb_stops, t_ram_app_calls, t_ram_app_mhz;
static unsigned long long t_usb_started_at;
/* The stub's first run: a failed install (written if t_mu_written) or a RAM app run (cmd 0x25); then it waits. */
static enum mu_result t_mu_first;
static bool t_mu_ram_app, t_mu_written;
void usb_midi_start(void) { ++t_usb_starts; t_usb_started_at = sim.now; }
void usb_midi_stop(void) { ++t_usb_stops; }
void usb_midi_poll(void) {}
void usb_midi_send(const uint8_t *data, uint32_t size) { (void)data; (void)size; }
enum mu_result mu_run(uint16_t key, uint8_t *sector, bool *written)
{
    (void)key, (void)sector;
    *written = false;
    if (++t_midi_runs == 1 && t_mu_first != MU_CANCELLED)
        return *written = t_mu_written, t_mu_first;
    if (t_midi_runs == 1 && t_mu_ram_app)
        mu_start_app();
    for (; ; ) {
        mu_idle();
        if (mu_cancel())
            return MU_CANCELLED;
    }
}

/* A RAM app that takes the panel down and leaves the screen black. */
void t_ram_app(unsigned mhz);
void t_ram_app(unsigned mhz)
{
    ++t_ram_app_calls;
    t_ram_app_mhz = mhz;
    REG(0x11d00) = 0;
    sim.panel_on = 0;
    memset(sim.fb, 0, sizeof(sim.fb));
}
__asm__(".globl mu_test_app_ram\n.set mu_test_app_ram, t_ram_app");

static const unsigned t_menu_keys[] = { KEY_PREV, KEY_NEXT, KEY_SELECT, KEY_RECOVERY };
#define T_FOREVER 100000000u

static void t_midi_prep(unsigned fixture)
{
    t_prep(fixture);
    midi_alone = false;
    midi_stuck = 0;
    t_midi_runs = t_usb_starts = t_usb_stops = t_ram_app_calls = t_ram_app_mhz = 0;
    t_usb_started_at = 0;
    t_mu_first = MU_CANCELLED;
    t_mu_written = false;
    t_mu_ram_app = false;
}

static bool t_midi_shown(const char *state)
{
    return t_row_is(3, "MIDI MODE") && t_row_is(4, state) && t_row_is(8, "TO OPEN THE MENU:") && t_row_is(9, MENU_KEYS);
}

/* No fixture: a blank flash (the public malformed-image case, t_prep(4)) has no app, so the menu comes up by itself
 * with APP NOT FOUND. Runs on every checkout, for the board the binary is built for. */
static void t_midi_blank(void)
{
    /* nobody at the unit: MIDI mode, why the app didn't start in view */
    t_midi_prep(4);
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1 && t_usb_starts == 1);
    CHECK(t_midi_shown("APP NOT FOUND") && t_row_is(2, ""));
    /* NEXT leaves it: the whole menu, the selection where it starts, the key not taken as a step */
    t_midi_prep(4);
    t_key(KEY_NEXT, 3000, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1 && t_usb_stops == 1);
    CHECK(t_row_is(3, "APP NOT FOUND"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE"));
    /* every key leaves it (the release wait and the key mapping differ per board) */
    for (unsigned i = 0; i < sizeof(t_menu_keys)/sizeof(t_menu_keys[0]); ++i) {
        t_midi_prep(4);
        t_key(t_menu_keys[i], 3000, 3300);
        sim.limit = 5000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* PLAY held at power-on: the menu itself, no MIDI mode */
    t_midi_prep(4);
    t_open_menu();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_midi_runs && !t_app_calls && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
}

/* A key that stays down: the menu all the same, and no press taken from it. */
static void t_midi_stuck(void)
{
    /* each key pressed in MIDI mode and never let go: the menu, at its first item (PLAY would have picked it) */
    for (unsigned i = 0; i < sizeof(t_menu_keys)/sizeof(t_menu_keys[0]); ++i) {
        t_midi_prep(4);
        t_key(t_menu_keys[i], 3000, T_FOREVER);
        sim.limit = 8000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && !t_resets && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* let go at last, the next press is a step again */
    t_midi_prep(4);
    t_key(KEY_NEXT, 3000, 6000);
    t_key(KEY_NEXT, 6500, 6600);
    sim.limit = 8000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_row_is(8, "  DOWNLOAD MODE") && t_row_is(9, "> MIDI MODE"));
    /* down before MIDI mode opens: it stays in MIDI mode, and another key still leaves it */
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, T_FOREVER);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, T_FOREVER);
    t_key(KEY_PREV, 4000, 4100);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    /* PLAY stuck once the power-on check has read it up: nothing waits for it, MIDI mode opens */
    static const unsigned from[] = { 50, 200 };
    for (unsigned i = 0; i < sizeof(from)/sizeof(from[0]); ++i) {
        t_midi_prep(4);
        t_key(KEY_SELECT, from[i], T_FOREVER);
        sim.limit = 6000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && t_usb_starts == 1 && t_midi_shown("APP NOT FOUND"));
        /* and another key leaves it, PLAY not taken as a press in the menu */
        t_midi_prep(4);
        t_key(KEY_SELECT, from[i], T_FOREVER);
        t_key(KEY_NEXT, 4000, 4100);
        sim.limit = 6000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && !t_resets && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* down from 5 ms, the power-on check sees PLAY held: that asks for the menu itself, no MIDI mode, as before */
    t_midi_prep(4);
    t_key(KEY_SELECT, 5, T_FOREVER);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_midi_runs && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
}

/* A key down when MIDI mode opens counts again once it has read as up for 50 scans in a row. */
static void t_midi_released(void)
{
    /* MIDI mode opens after keys that go down at 100 ms, past the power-on checks */
    t_midi_prep(4);
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT && t_usb_started_at > 150000);
    /* NEXT down at power-on, let go, pressed again: it leaves */
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, 1500);
    t_key(KEY_NEXT, 4000, 4300);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    /* all four down when it opens, let go, then each one leaves */
    for (unsigned i = 0; i < sizeof(t_menu_keys)/sizeof(t_menu_keys[0]); ++i) {
        t_midi_prep(4);
        t_key(KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY, 100, 3500);
        t_key(t_menu_keys[i], 4500, 4800);
        sim.limit = 7000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && !t_resets && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* still down: none of them leaves */
    t_midi_prep(4);
    t_key(KEY_PREV | KEY_NEXT | KEY_SELECT | KEY_RECOVERY, 100, T_FOREVER);
    sim.limit = 7000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
    /* a stuck key that reads up for 10 ms now and then doesn't make MIDI mode leave by itself */
    t_midi_prep(4);
    t_key(KEY_NEXT, 0, 3000);
    t_key(KEY_NEXT, 3010, 4000);
    t_key(KEY_NEXT, 4010, T_FOREVER);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_midi_shown("APP NOT FOUND"));
}

/* An install that fails with nothing written: MIDI mode opened by itself stays, the failure in view; only a key
 * leaves. Chosen from the menu, it goes back to the menu as before. */
static void t_midi_failures(void)
{
    static const enum mu_result fails[] = { MU_LOST, MU_BAD_PACKAGE, MU_WRONG_KEY };
    static const char *const texts[] = { "INSTALLER STOPPED", "BAD PACKAGE", "WRONG CHIP KEY" };
    for (unsigned i = 0; i < sizeof(fails)/sizeof(fails[0]); ++i) {
        t_midi_prep(4);
        t_mu_first = fails[i];
        sim.limit = 30000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 2 && t_usb_starts == 1 && !t_usb_stops && !t_resets);
        CHECK(t_midi_shown("APP NOT FOUND") && t_row_is(13, texts[i]));
        t_midi_prep(4);
        t_mu_first = fails[i];
        t_key(KEY_NEXT, 6000, 6100);
        sim.limit = 8000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 2 && t_usb_stops == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* chosen from the menu: PLAY at power-on, NEXT, PLAY; it fails, and the menu is back */
    t_midi_prep(4);
    t_mu_first = MU_LOST;
    t_open_menu();
    t_key(KEY_NEXT, 1600, 1700);
    t_key(KEY_SELECT, 2000, 2100);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_usb_stops == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(9, "> MIDI MODE"));
    /* opened by itself, it fails after writing: never again over a half-written app, it restarts */
    for (unsigned i = 0; i < sizeof(fails)/sizeof(fails[0]); ++i) {
        t_midi_prep(4);
        t_mu_first = fails[i];
        t_mu_written = true;
        sim.limit = 8000000;
        CHECK(t_go() == END_RESET);
        CHECK(t_resets == 1 && t_midi_runs == 1 && t_usb_stops == 1 && !t_usb_reset());
        CHECK(t_row_is(13, texts[i]) && t_row_is(14, "RESTARTS: TRY AGAIN"));
    }
}

/* The other ways the menu comes up by itself: their reason stays in view in MIDI mode. */
static void t_midi_reasons(void)
{
    /* the flash isn't 1 MiB: FLASH ERROR */
    t_midi_prep(4);
    sim.jedec = (sim.jedec&~0xffu) | 0x13;
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_row_is(2, "FLASH ERROR") && t_midi_shown("APP NOT FOUND"));
    if (SMK) {
        /* calibration to back up, and the flash refuses the write: TRIM ERROR */
        t_midi_prep(4);
        memcpy(sim_nor+0xc1000+0x224, "trimdat", 8);
        nor_budget = 0;
        sim.limit = 3000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && t_row_is(2, "TRIM ERROR") && t_midi_shown("APP NOT FOUND"));
    }
    /* a RAM app runs (cmd 0x25) and returns, the screen left black: the reason is drawn again with the rest */
    t_midi_prep(4);
    sim.jedec = (sim.jedec&~0xffu) | 0x13;
    t_mu_ram_app = true;
    sim.limit = 4000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_ram_app_calls == 1 && t_ram_app_mhz == 60 && t_usb_starts == 2 && t_usb_stops == 1);
    CHECK(t_row_is(2, "FLASH ERROR") && t_midi_shown("APP NOT FOUND"));
}

/* A synthetic app on a blank flash (the layout of tests/core_test.c's synthetic banks), for the chip key 980F the
 * simulator's efuse gives: the header, the application directory pointing at a bank at 0x10000, and the bank's area
 * head and app.bin, its 32 zero bytes the entry. Stored bytes, worked out apart from src/core.c from §5.1 and §5.2
 * (CRC16/XMODEM and ENC, checked against §5.1's vectors), so a wrong CRC or cipher in core.c can't pass here. */
static const struct {
    uint32_t address;
    uint8_t bytes[32];
} t_app_lines[] = {
    /* 0x00000, flash header: size 0xFF000, alignment 16, ENC FFFF */
    { 0x00000, {
        0xfb, 0xa2, 0x9f, 0x1f, 0x1f, 0x3e, 0x7c, 0xf8, 0xf0, 0x31, 0xac, 0x67, 0xce, 0xad, 0x5b, 0x97,
        0x0f, 0x1e, 0x3c, 0x78, 0xd1, 0x83, 0x27, 0x4e, 0x9c, 0x19, 0x13, 0x26, 0x4c, 0x98, 0x30, 0x60,
    } },
    /* 0x00020, app_dir_head: bank 0x10000, no info record, ENC FFFF */
    { 0x00020, {
        0x71, 0x26, 0x9f, 0x1f, 0x1f, 0x3e, 0x7d, 0xf8, 0xf0, 0xc1, 0xa3, 0x67, 0x4f, 0xbd, 0x5b, 0x97,
        0x6e, 0x6e, 0x4c, 0x27, 0xb5, 0xea, 0x55, 0x11, 0xf4, 0x7c, 0x72, 0x42, 0x4c, 0x98, 0x30, 0x60,
    } },
    /* 0x10000, app_area_head: entry 0x02000120, extent 0x1000, ENC 980F */
    { 0x10000, {
        0x55, 0x04, 0x81, 0x03, 0xf9, 0xb3, 0x64, 0xca, 0xb1, 0x72, 0xc4, 0x88, 0x93, 0x01, 0x23, 0x46,
        0xcc, 0x2a, 0xe5, 0x54, 0x77, 0x5e, 0x1c, 0xb2, 0xf9, 0x05, 0x9e, 0x97, 0x88, 0xf9, 0xd3, 0xa6,
    } },
    /* 0x10020, app.bin: 32 bytes at 0x120, data CRC 0000, ENC 980F^8 */
    { 0x10020, {
        0x8e, 0x78, 0x5e, 0xbc, 0x79, 0xb3, 0x64, 0xc8, 0x91, 0x62, 0xc4, 0x88, 0x92, 0x20, 0x61, 0xc2,
        0xc4, 0x1b, 0x87, 0xe1, 0xfc, 0x74, 0x75, 0x17, 0x0f, 0x1e, 0x1d, 0x3a, 0x74, 0xc9, 0xb3, 0x66,
    } },
    /* 0x10120, app.bin's 32 zero bytes, ENC 980F^48 */
    { 0x10120, {
        0x47, 0xaf, 0x5e, 0xbc, 0x59, 0xb2, 0x64, 0xc8, 0xb1, 0x62, 0xe5, 0xca, 0x94, 0x28, 0x50, 0xa0,
        0x61, 0xe3, 0xc6, 0xad, 0x5a, 0xb4, 0x68, 0xf1, 0xc3, 0x86, 0x2d, 0x5a, 0xb4, 0x68, 0xd0, 0xa0,
    } },
};

static void t_synthetic_app(void)
{
    for (unsigned i = 0; i < sizeof(t_app_lines)/sizeof(t_app_lines[0]); ++i)
        memcpy(sim_nor+t_app_lines[i].address, t_app_lines[i].bytes, 32);
}

/* What an app may leave as it returns: the panel's bus off. */
static void t_app_leaves_panel_off(void)
{
    REG(0x11d00) = 0;
    sim.panel_on = 0;
}

/* The path after an app exits: MIDI mode starts by itself there too, from whatever the app left. */
static void t_midi_app_exit(void)
{
    /* the synthetic app boots as any app does, and no MIDI mode */
    t_midi_prep(4);
    t_synthetic_app();
    CHECK(t_go() == END_APP);
    CHECK(t_app_calls == 1 && !t_midi_runs && !t_usb_starts);
    /* it returns, the panel left off: MIDI mode, with APP EXITED above the app's state */
    t_midi_prep(4);
    t_synthetic_app();
    t_app_returns = true;
    t_app_hook = t_app_leaves_panel_off;
    sim.limit = 4000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_app_calls == 1 && t_midi_runs == 1 && t_usb_starts == 1);
    CHECK(t_row_is(2, "APP EXITED") && t_midi_shown("APP OK"));
    /* a key: the menu, and BOOT APP from it resets to try again (§7) */
    t_midi_prep(4);
    t_synthetic_app();
    t_app_returns = true;
    t_app_hook = t_app_leaves_panel_off;
    t_key(KEY_NEXT, 4000, 4100);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && t_usb_stops == 1 && t_row_is(2, "APP EXITED") && t_row_is(3, "APP OK"));
    CHECK(t_row_is(8, "> BOOT APP") && t_row_is(9, "  DOWNLOAD MODE"));
    t_midi_prep(4);
    t_synthetic_app();
    t_app_returns = true;
    t_key(KEY_NEXT, 4000, 4100);
    t_key(KEY_SELECT, 4600, 4700);
    CHECK(t_go() == END_RESET);
    CHECK(t_app_calls == 1 && t_midi_runs == 1 && !t_usb_reset());
    /* stuck in MIDI mode after it: the menu still comes */
    t_midi_prep(4);
    t_synthetic_app();
    t_app_returns = true;
    t_key(KEY_SELECT, 4000, T_FOREVER);
    sim.limit = 9000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_midi_runs == 1 && !t_resets && t_row_is(8, "> BOOT APP"));
    /* PLAY stuck from 200 ms, through the app and its return: MIDI mode all the same */
    t_midi_prep(4);
    t_synthetic_app();
    t_app_returns = true;
    t_key(KEY_SELECT, 200, T_FOREVER);
    sim.limit = 6000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(t_app_calls == 1 && t_midi_runs == 1 && t_row_is(2, "APP EXITED") && t_midi_shown("APP OK"));
}

/* With the board's flash dump (a real image, its app damaged by one bit: APP BAD CRC). */
static void t_midi_by_default(void)
{
    unsigned f = t_own_fixture();
    /* the app damaged, nobody at the unit: MIDI mode */
    t_midi_prep(f);
    t_damage_app();
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_midi_shown("APP BAD CRC"));
    /* NEXT leaves it: the whole menu, the selection where it starts */
    t_midi_prep(f);
    t_damage_app();
    t_key(KEY_NEXT, 3000, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE") && t_row_is(10, "  BOOT ANYWAY") && t_row_is(11, ""));
    /* then BOOT ANYWAY, as before: two steps down, PLAY, PLAY held for the confirmation */
    t_midi_prep(f);
    t_damage_app();
    t_key(KEY_NEXT, 3000, 3300);
    t_key(KEY_NEXT, 4000, 4100);
    t_key(KEY_NEXT, 4400, 4500);
    t_key(KEY_SELECT, 5000, 5100);
    t_key(KEY_SELECT, 5600, 9100);
    CHECK(t_go() == END_RESET);
    CHECK(t_midi_runs == 1 && !memcmp(t_reset_retained, "BOOTANY", 8));
    /* PLAY held at power-on: the menu itself, no MIDI mode */
    t_midi_prep(f);
    t_open_menu();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_midi_runs && !t_app_calls && t_row_is(8, "> BOOT APP"));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    void *ram = mmap((void *)(uintptr_t)0x01c00000, 0x80000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(ram == (void *)(uintptr_t)0x01c00000);
    void *xip = mmap((void *)(uintptr_t)0x02000000, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(xip == (void *)(uintptr_t)0x02000000);
    t_fm1_matrix_enable();
    printf("boot simulation: the menu's MIDI mode by default (%s)\n", SMK ? "SMK-37":"FM-1");
    t_midi_blank();
    t_midi_stuck();
    t_midi_released();
    t_midi_failures();
    t_midi_reasons();
    t_midi_app_exit();
    /* the flash dump of the board is an extra: argv[1], else testdata/<board>-stock.bin */
    const char *path = argc > 1 ? argv[1] : (SMK ? "testdata/smk37-stock.bin":"testdata/fm1-stock.bin");
    unsigned f = t_own_fixture();
    FILE *input = fopen(path, "rb");
    if (input) {
        assert(fread(t_fixture_cache[f], 1, 0x100000, input) == 0x100000);
        fclose(input);
        t_fixture_present[f] = true;
        t_midi_by_default();
    } else if (errno == ENOENT && !getenv("REQUIRE_FIXTURES")) {
        printf("SKIP: %s not present (the checks on a real dump)\n", path);
    } else {
        printf("FAIL: cannot read %s (errno %d)\n", path, errno);
        ++t_failures;
    }
    printf("%u checks, %u failed\n", t_checks, t_failures);
    return t_failures != 0;
}
