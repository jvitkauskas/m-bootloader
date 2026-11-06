/* SPDX-License-Identifier: MIT */
/* MIDI mode as a program in RAM, for units without m-bootloader: a host can back up the flash and
 * install firmware (docs/SPEC.md §11.7). M-UPGRADE's route stages it like an updater, and the
 * installed bootloader, stock or ours, runs it (STAGE_OTA). From download mode, M-Flasher's run-ram
 * starts it, which leaves the flash alone. */
#include "midi_update.h"
#include "usb_midi.h"
#include "core.h"
#include "platform.h"
#include "display.h"
#include <string.h>
#define RETAINED ((uint8_t *)(uintptr_t)0x01c7fd80)
#define YELLOW 0xffe0
#define WHITE 0xffff
#define GREEN 0x07e0
#define RED 0xf800
#ifdef BOARD_SMK
#define BOARD 2
#define TITLE "SMK-37 M-FLASHER"
#else
#define BOARD 1
#define TITLE "FM-1 M-FLASHER"
#endif
/* A host gets this long to appear, or the unit goes back to its firmware. */
#define NO_HOST_MS 120000u
extern uint8_t __bss_start[], __bss_end[];
static uint8_t sector[MU_SECTOR] __attribute__((aligned(4)));
static bool screen;
static uint32_t waited;

void mu_send(const uint8_t *data, uint32_t size) {
    usb_midi_send(data, size);
}

void mu_idle(void) {
    for (unsigned n = 0; n < 2; ++n) {
        usb_midi_poll();
        hal_delay_us(500);
    }
}

bool mu_cancel(void) {
    if (usb_midi_configured())
        waited = 0;
    else
        ++waited;
    return waited >= NO_HOST_MS;
}

bool mu_read(uint32_t address, void *out, uint32_t size) {
    return flash_read(0, address, out, size);
}

bool mu_erase(uint32_t address) {
    return flash_erase(0, address, MU_SECTOR);
}

bool mu_program(uint32_t address, const void *data, uint32_t size) {
    return flash_program(0, address, data, size);
}

void mu_unit_info(struct mu_unit *unit) {
    unit->jedec = flash_jedec();
    unit->capacity = flash_capacity();
    flash_uid(unit->uid);
    unit->kind = MU_STAGE;
    unit->version = STAGE_VERSION;
}

static void show(unsigned row, uint16_t color, const char *text) {
    if (screen)
        display_text(row, color, text);
}

void mu_progress(bool writing, unsigned percent) {
    static unsigned shown;
    if (!screen)
        return;
    if (!percent) {
        show(3, YELLOW, writing ? "WRITING" : "CHECKING");
        show(6, YELLOW, writing ? "DO NOT SWITCH OFF" : "");
        shown = 0;
    }
    if (percent != shown || percent == 100)
        display_progress(percent, GREEN);
    shown = percent;
}

#ifdef STAGE_OTA
static bool read(void *ctx, uint32_t address, void *out, uint32_t size) {
    return flash_read(ctx, address, out, size);
}

/* The next start must not run this again: the unit goes back to its firmware. */
static void retire_request(uint32_t stream_end) {
    struct core_nor nor = {{0, flash_capacity(), read}, flash_program, flash_erase};
    struct core_header header;
    hal_fault_clear();
    if (core_header_read(&nor.flash, &header))
        core_request_retire(&nor, header.alignment, stream_end);
    core_put_le16(RETAINED+8, 0);
    sync_cpu();
}
#endif

void stage_main(void *context, uint32_t stream_end) __attribute__((noreturn));
void stage_main(void *context, uint32_t stream_end) {
    (void)context;
    (void)stream_end;
    irq_off();
    memset(__bss_start, 0, (size_t)(__bss_end-__bss_start));
    hal_early();
#ifdef STAGE_OTA
    /* The bootloader that started us has set the 60 MHz LSB clock (§3.2). */
    hal_timer_clock(60);
#else
    hal_clock_b();
#endif
    flash_init(1, 1);
#ifdef STAGE_OTA
    if (flash_capacity())
        retire_request(stream_end);
#endif
    uint16_t key = (uint16_t)(hal_efuse(1, 3) >> 8);
    screen = display_init(BOARD);
    if (screen) {
        for (unsigned row = 0; row < 15; ++row)
            display_text(row, WHITE, "");
        display_text(1, GREEN, TITLE);
        display_text(3, WHITE, "CONNECT USB, AND");
        display_text(4, WHITE, "OPEN THE WEB PAGE.");
        display_text(8, WHITE, "SWITCH OFF: BACK TO");
        display_text(9, WHITE, "THE FIRMWARE");
    }
    usb_midi_start();
    for (; ; ) {
        bool written;
        enum mu_result r = mu_run(key, sector, &written);
        if (r == MU_DOWNLOAD || r == MU_DONE || r == MU_RESTART || r == MU_CANCELLED) {
            hal_delay_us(200000);
            usb_midi_stop();
            flash_release();
            if (r == MU_DOWNLOAD) {
                memcpy(RETAINED, "usb_update_mode", 16);
                sync_cpu();
            }
            hal_reset();
        }
        show(3, RED, r == MU_BAD_PACKAGE ? "BAD PACKAGE" : r == MU_WRONG_KEY ? "WRONG CHIP KEY"
            : r == MU_LOST ? "HOST STOPPED" : "FLASH ERROR");
        show(6, written ? RED : WHITE, written ? "INSTALL AGAIN" : "");
    }
}
