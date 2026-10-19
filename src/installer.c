/* SPDX-License-Identifier: MIT */
/* Staged updater that installs the bootloader built with it: M-UPGRADE stages
 * it from a .fwsc package and the installed bootloader, stock or ours, runs it
 * (§6.3). It rewrites only the bootloader and its boot-area record, checks
 * them, retires the update request, in RAM and in flash, and resets into the
 * new bootloader. */
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
#define TITLE "SMK-37 M-BOOTLOADER"
#else
#define BOARD 1
#define TITLE "FM-1 M-BOOTLOADER"
#endif
#define SECTOR 4096u
extern uint8_t __bss_start[], __bss_end[];
extern const uint8_t installer_container[], installer_container_end[];
static uint8_t area[CORE_BOOT_AREA] __attribute__((aligned(4)));
static uint8_t check[SECTOR] __attribute__((aligned(4)));

static bool read(void *ctx, uint32_t address, void *out, uint32_t size) {
    return flash_read(ctx, address, out, size);
}

static bool write_sector(uint32_t address) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        hal_fault_clear();
        if (flash_erase(0, address, SECTOR) && flash_program(0, address, area + address, SECTOR) && flash_read(0, address, check, SECTOR) && !memcmp(check, area + address, SECTOR))
            return true;
    }
    return false;
}

static const char *install(bool screen) {
    uint32_t size = (uint32_t)(installer_container_end - installer_container);
    if (!flash_init(1, 1) || flash_capacity() != 0x100000)
        return "NO FLASH";
    if (!flash_read(0, 0, area, sizeof(area)))
        return "READ FAILED";
    if (!core_boot_install(area, installer_container, size, flash_capacity()))
        return "BOOT AREA UNKNOWN";
    bool changed = false;
    for (uint32_t address = 0; address < CORE_BOOT_AREA; address += SECTOR) {
        if (screen)
            display_progress(address * 100 / CORE_BOOT_AREA, GREEN);
        if (!flash_read(0, address, check, SECTOR))
            return "READ FAILED";
        if (!memcmp(check, area + address, SECTOR))
            continue;
        changed = true;
        if (!write_sector(address))
            return "WRITE FAILED";
    }
    if (screen)
        display_progress(100, GREEN);
    return changed ? 0 : "ALREADY INSTALLED";
}

/* The flash copy of the request is acted on whenever the app does not mount,
 * by this bootloader as by the stock one: retire it too, or a later broken
 * app would have every start run this again (§6.2). */
static void retire_flash_request(uint32_t stream_end) {
    struct core_nor nor = {{0, flash_capacity(), read}, flash_program, flash_erase};
    struct core_header header;
    hal_fault_clear();
    if (core_header_read(&nor.flash, &header))
        core_request_retire(&nor, header.alignment, stream_end);
}

void installer_main(void *context, uint32_t stream_end) __attribute__((noreturn));
void installer_main(void *context, uint32_t stream_end) {
    (void)context;
    irq_off();
    memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
    /* Watchdog off, as §6.3 says it already is: never reset mid-write. */
    hal_early();
    /* The bootloader that started us has set the 60 MHz LSB clock (§3.2). */
    hal_timer_clock(60);
    bool screen = display_init(BOARD);
    if (screen) {
        for (unsigned row = 0; row < 15; ++row)
            display_text(row, WHITE, "");
        display_text(1, GREEN, TITLE);
        display_text(3, YELLOW, "INSTALLING");
        display_text(4, WHITE, (const char *)installer_container + 0x60);
        display_text(6, WHITE, "DO NOT SWITCH OFF");
    }
    const char *failure = install(screen);
    /* Never run again for this request, by this or the next bootloader. */
    if (flash_capacity())
        retire_flash_request(stream_end);
    core_put_le16(RETAINED + 8, 0);
    sync_cpu();
    if (screen) {
        display_text(6, WHITE, "");
        display_text(3, failure ? RED : GREEN, failure ? failure : "INSTALLED");
        display_text(8, WHITE, "RESTARTING");
    }
    hal_delay_us(failure ? 5000000 : 2000000);
    flash_release();
    hal_reset();
}
