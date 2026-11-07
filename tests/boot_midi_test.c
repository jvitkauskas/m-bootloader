/* SPDX-License-Identifier: MIT */
/* The menu's MIDI mode by default (src/boot.c midi_auto): boot.c built with MIDI_MODE on the boot simulator, the
 * USB MIDI side stubbed (no installer connects: the MIDI loop only polls its cancel). Built for the FM-1 and, with
 * BOARD_SMK, for the SMK-37 (its own binary and output line).
 * Without any fixture, on a blank flash (APP NOT FOUND, the menu comes up by itself): MIDI mode first, every key
 * leaves it, PLAY held at power-on no MIDI mode. With the board's flash dump (argv[1], else testdata/<board>-stock.bin,
 * 1 MiB) as an extra, its app damaged (one bit in app.bin: APP BAD CRC): the same, and BOOT ANYWAY still reached.
 * A missing dump is a SKIP of the extra only; with REQUIRE_FIXTURES set in the environment it fails.
 *   - the menu that comes up by itself opens in MIDI mode, "ANY KEY: MENU", the app not run;
 *   - any key (NEXT) goes back to the whole menu: its actions as before, the key not taken as a menu step;
 *   - from there BOOT ANYWAY is reached and confirmed as before (the other modes stay reachable);
 *   - PLAY held at power-on opens the menu itself, no MIDI mode. */
#define MIDI_MODE 1
#include "midi_update.h"
#include "usb_midi.h"
#define main boot_test_main
#include "boot_test.c"
#undef main

static unsigned t_midi_runs;
void usb_midi_start(void) {}
void usb_midi_stop(void) {}
void usb_midi_poll(void) {}
void usb_midi_send(const uint8_t *data, uint32_t size) { (void)data; (void)size; }
enum mu_result mu_run(uint16_t key, uint8_t *sector, bool *written)
{
    (void)key, (void)sector;
    *written = false;
    ++t_midi_runs;
    for (; ; ) {
        mu_idle();
        if (mu_cancel())
            return MU_CANCELLED;
    }
}

/* No fixture: a blank flash (the public malformed-image case, t_prep(4)) has no app, so the menu comes up by itself
 * with APP NOT FOUND. Runs on every checkout, for the board the binary is built for. */
static void t_midi_blank(void)
{
    /* nobody at the unit: MIDI mode */
    t_prep(4);
    t_midi_runs = 0;
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "MIDI MODE") && t_row_is(8, "ANY KEY: MENU"));
    /* NEXT leaves it: the whole menu, the selection where it starts, the key not taken as a step */
    t_prep(4);
    t_midi_runs = 0;
    t_key(KEY_NEXT, 3000, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "APP NOT FOUND"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE"));
    /* every key leaves it (the release wait and the key mapping differ per board) */
    static const unsigned keys[] = { KEY_PREV, KEY_NEXT, KEY_SELECT, KEY_RECOVERY };
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        t_prep(4);
        t_midi_runs = 0;
        t_key(keys[i], 3000, 3300);
        sim.limit = 5000000;
        CHECK(t_go() == END_TIMEOUT);
        CHECK(t_midi_runs == 1 && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
    }
    /* PLAY held at power-on: the menu itself, no MIDI mode */
    t_prep(4);
    t_midi_runs = 0;
    t_open_menu();
    sim.limit = 2500000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_midi_runs && !t_app_calls && t_row_is(3, "APP NOT FOUND") && t_row_is(8, "> DOWNLOAD MODE"));
}

/* With the board's flash dump (a real image, its app damaged by one bit: APP BAD CRC). */
static void t_midi_by_default(void)
{
    unsigned f = t_own_fixture();
    /* the app damaged, nobody at the unit: MIDI mode */
    t_prep(f);
    t_damage_app();
    t_midi_runs = 0;
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "MIDI MODE") && t_row_is(8, "ANY KEY: MENU"));
    /* NEXT leaves it: the whole menu, the selection where it starts */
    t_prep(f);
    t_damage_app();
    t_midi_runs = 0;
    t_key(KEY_NEXT, 3000, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE") && t_row_is(10, "  BOOT ANYWAY"));
    /* then BOOT ANYWAY, as before: two steps down, PLAY, PLAY held for the confirmation */
    t_prep(f);
    t_damage_app();
    t_midi_runs = 0;
    t_key(KEY_NEXT, 3000, 3300);
    t_key(KEY_NEXT, 4000, 4100);
    t_key(KEY_NEXT, 4400, 4500);
    t_key(KEY_SELECT, 5000, 5100);
    t_key(KEY_SELECT, 5600, 9100);
    CHECK(t_go() == END_RESET);
    CHECK(t_midi_runs == 1 && !memcmp(t_reset_retained, "BOOTANY", 8));
    /* PLAY held at power-on: the menu itself, no MIDI mode */
    t_prep(f);
    t_midi_runs = 0;
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
