/* SPDX-License-Identifier: MIT */
/* The menu's MIDI mode by default (src/boot.c midi_auto): boot.c built with MIDI_MODE on the boot simulator, the
 * USB MIDI side stubbed (no installer connects: the MIDI loop only polls its cancel), over a real FM-1 flash dump
 * (argv[1], 1 MiB) whose app is damaged (one bit in app.bin: APP BAD CRC).
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

static void t_midi_by_default(void)
{
    /* the app damaged, nobody at the unit: MIDI mode */
    t_prep(0);
    t_damage_app();
    t_midi_runs = 0;
    sim.limit = 3000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "MIDI MODE") && t_row_is(8, "ANY KEY: MENU"));
    /* NEXT leaves it: the whole menu, the selection where it starts */
    t_prep(0);
    t_damage_app();
    t_midi_runs = 0;
    t_key(KEY_NEXT, 3000, 3300);
    sim.limit = 5000000;
    CHECK(t_go() == END_TIMEOUT);
    CHECK(!t_app_calls && t_midi_runs == 1);
    CHECK(t_row_is(3, "APP BAD CRC"));
    CHECK(t_row_is(8, "> DOWNLOAD MODE") && t_row_is(9, "  MIDI MODE") && t_row_is(10, "  BOOT ANYWAY"));
    /* then BOOT ANYWAY, as before: two steps down, PLAY, PLAY held for the confirmation */
    t_prep(0);
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
    t_prep(0);
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
    const char *path = argc > 1 ? argv[1] : "testdata/fm1-stock.bin";
    FILE *input = fopen(path, "rb");
    if (!input && errno == ENOENT) {
        printf("SKIP: %s not present (boot fixture groups)\n", path);
        return 0;
    }
    assert(input);
    assert(fread(t_fixture_cache[0], 1, 0x100000, input) == 0x100000);
    fclose(input);
    t_fixture_present[0] = true;
    puts("boot simulation: the menu's MIDI mode by default (FM-1)");
    t_fm1_matrix_enable();
    t_midi_by_default();
    printf("%u checks, %u failed\n", t_checks, t_failures);
    return t_failures != 0;
}
