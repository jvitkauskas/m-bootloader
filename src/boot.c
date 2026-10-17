/* SPDX-License-Identifier: MIT */
#include "core.h"
#include "contract.h"
#include "platform.h"
#include "board.h"
#include "display.h"
#include <string.h>
#define RETAINED ((uint8_t *)(uintptr_t)0x01c7fd80)
#define YELLOW 0xffe0
#define WHITE 0xffff
#define GREEN 0x07e0
#define RED 0xf800
#define GREY 0x8410
#ifdef BOOT_SIM
void boot_sim_call_app(uint32_t entry, void *object);
void boot_sim_call_updater(uint32_t entry, void *context, uint32_t end);
#endif
#ifdef BOARD_SMK
#define SMK true
#define BOARD 2
#define TITLE "SMK-37 M-BOOTLOADER"
#define CONTROLS "< > MOVE SEQ PLAY OK"
#define HOLD "HOLD SEQ PLAY 3 S"
#define RELEASE "RELEASE SEQ PLAY"
#else
#define SMK false
#define BOARD 1
#define TITLE "FM-1 M-BOOTLOADER"
#define CONTROLS "OCT-/+ MOVE PLAY OK"
#define HOLD "HOLD PLAY 3 S"
#define RELEASE "RELEASE PLAY"
#endif
extern uint8_t __bss_start[], __bss_end[];
static uint8_t work[CORE_WORK_SIZE] __attribute__((aligned(4)));
static uint8_t request[80], clock_meta[52], memory_meta[60], update_meta[24];
static struct core_config config;
static struct core_app app;
static struct core_header header;
static struct core_nor nor;
static uint32_t jedec;
static uint16_t unit_key, trim;
static uint8_t mac[8];
static unsigned effective_width;
static bool mounted, xip_attempted, screen_ready;
static const char *reason;
static uint32_t pointer(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

static void rom_download(void) __attribute__((noreturn));
static void rom_download(void) {
    memcpy(RETAINED, "usb_update_mode", 16);
    sync_cpu();
    hal_reset();
}

static bool screen(void) {
    if (!screen_ready)
        screen_ready = display_init(BOARD);
    return screen_ready;
}

static void clear_rows(unsigned first, unsigned last) {
    for (unsigned r = first; r <= last; ++r)
        display_text(r, WHITE, "");
}

static unsigned append(char *s, unsigned pos, const char *v) {
    while (*v && pos < 20)
        s[pos++] = *v++;
    s[pos] = 0;
    return pos;
}

static unsigned number(char *s, unsigned pos, uint32_t v, unsigned base, unsigned digits) {
    char temp[10];
    unsigned n = 0;
    do {
        temp[n++] = "0123456789ABCDEF"[v%base];
        v /= base;
    } while (v && n < sizeof(temp));
    while (n < digits && n < sizeof(temp))
        temp[n++] = '0';
    while (n && pos < 20)
        s[pos++] = temp[--n];
    s[pos] = 0;
    return pos;
}

__attribute__((noinline)) static bool apply_config(void) {
    p33_write(9, (p33_read(9)&0xf0) | 11);
    p33_write(6, (p33_read(6)&0xf8) | 3);
    for (unsigned n = 0; n < 2; ++n)
        sync_cpu();
    for (unsigned n = 0; n < 16; ++n)
        p33_write(0xd0+n, 0);
    for (unsigned n = 0; n < 3; ++n)
        p33_write(0xad+n, 0);
    for (unsigned n = 0; n < 2; ++n)
        sync_cpu();
    if (hal_fault())
        return false;
    return hal_clock_a();
}

__attribute__((noinline)) static bool read_config(const uint32_t *device) {
    core_config_defaults(&config);
    if (device[1] || !device[0] || !device[3])
        return false;
    uint8_t raw[32];
    struct core_record record;
    void *params = (void *)(uintptr_t)device[0];
    int (*read_rom)(void *, void *, uint32_t, uint32_t) = (void *)(uintptr_t)device[3];
    REG(0x13004) = 0xffff;
    REG(0x40304) = 0xffff;
    memset(raw, 0, sizeof(raw));
    ((uint8_t *)params)[1] = 1;
    (void)read_rom(params, raw, 64, 32);
    if (!core_record_parse(&record, raw) || memcmp(record.name, "isd_config.ini", 15))
        return false;
    if (!record.length || record.length > sizeof(work) || !core_range(record.address, record.length, 0x100000))
        return false;
    memset(work, 0, record.length);
    ((uint8_t *)params)[1] = 0;
    (void)read_rom(params, work, record.address, record.length);
    if (record.data_crc != 0xffff && core_crc16(0, work, record.length) != record.data_crc)
        return false;
    return core_config_parse(&config, work, record.length);
}

__attribute__((noinline)) static void identify(void) {
    jedec = flash_jedec();
    nor.flash.ctx = 0;
    nor.flash.capacity = flash_capacity();
    nor.flash.read = flash_read;
    nor.program = flash_program;
    nor.erase = flash_erase;
    memset(mac, 255, sizeof(mac));
    unit_key = (uint16_t)(hal_efuse(1, 3) >> 8);
    uint32_t r2 = hal_efuse(0, 2), r3 = hal_efuse(0, 3);
    trim = (uint16_t)(r2 | (r3&0xfff238fc));
    if (nor.flash.capacity >= 256 && core_read(&nor.flash, nor.flash.capacity-256, work, 256)) {
        memcpy(mac, work+64, 8);
        bool nonzero = false;
        for (unsigned i = 0; i < 8; ++i)
            if (work[i])
                nonzero = true;
        if (unit_key == 0xffff && nonzero && core_crc16(0, work, 8) == core_le16(work+8))
            unit_key = core_le16(work+5);
    }
}

static void download_screen(void) __attribute__((noreturn));
static void download_screen(void) {
    clear_rows(3, 14);
    display_text(3, GREEN, "DOWNLOAD MODE");
    display_text(5, WHITE, "CONNECT USB TO PC");
    display_text(6, WHITE, "TO WRITE FLASH.");
    display_text(8, WHITE, "POWER OFF AND ON");
    display_text(9, WHITE, "TO EXIT.");
    rom_download();
}

static void release_select(void) {
    while (board_keys()&KEY_SELECT)
        board_wait(20, true);
}

static bool confirm(bool wipe, const struct core_wipe *plan) {
    clear_rows(8, 14);
    display_text(8, RED, wipe ? "WIPE USER DATA":"BOOT ANYWAY");
    if (wipe) {
        uint32_t cursor = 0, address, size;
        unsigned row = 9;
        bool have = core_wipe_next(plan, nor.flash.capacity, SMK, &cursor, &address, &size);
        while (have) {
            uint32_t start = address, end = address+size;
            while ((have = core_wipe_next(plan, nor.flash.capacity, SMK, &cursor, &address, &size)) && address == end)
                end = address+size;
            char line[21];
            unsigned n = append(line, 0, "ERASE ");
            n = number(line, n, start, 16, 5);
            n = append(line, n, "-");
            number(line, n, end-1, 16, 5);
            display_text(row++, WHITE, line);
        }
        display_text(12, GREY, "KEEP PROTECTED AREAS");
    } else {
        display_text(9, WHITE, "APP MAY CRASH");
        display_text(10, WHITE, "OR MISBEHAVE");
        display_text(11, WHITE, "CRC AT NEXT BOOT");
    }
    release_select();
    display_text(13, GREY, "OTHER KEYS: CANCEL");
    display_text(14, YELLOW, HOLD);
    /* Clear both margins of the text row before its 12-pixel progress bar. */
    display_rect(0, 208, 240, 16, 0);
    unsigned held = 0;
    for (; ; ) {
        unsigned keys = board_keys();
        if (keys&~KEY_SELECT)
            return false;
        if (keys == KEY_SELECT)
            held += 20;
        else
            held = 0;
        display_rect(0, 210, 240, 12, GREY);
        display_rect(0, 210, 240*(held > 3000 ? 3000:held)/3000, 12, GREEN);
        if (held >= 3000) {
            display_text(14, GREEN, RELEASE);
            release_select();
            return true;
        }
        board_wait(20, true);
    }
}

__attribute__((noinline)) static bool wipe_user(const struct core_wipe *plan) {
    uint32_t cur = 0, a, n, total = 0, done = 0;
    while (core_wipe_next(plan, nor.flash.capacity, SMK, &cur, &a, &n))
        total += n;
    if (!total)
        return false;
    display_text(13, YELLOW, "ERASING");
    cur = 0;
    while (core_wipe_next(plan, nor.flash.capacity, SMK, &cur, &a, &n)) {
        struct core_wipe one;
        one.count = 1;
        one.start[0] = a;
        one.end[0] = a+n;
        if (!core_wipe_execute(&nor, &one, SMK, work))
            return false;
        done += n;
        display_rect(0, 210, 240, 12, GREY);
        display_rect(0, 210, 240*done/total, 12, GREEN);
    }
    display_text(13, GREEN, "DONE");
    board_wait(1000, true);
    hal_reset();
}

__attribute__((noinline)) static bool menu(void) {
    board_init(SMK);
    if (!screen())
        rom_download();
    display_text(13, YELLOW, "CHECKING");
    if (mounted)
        core_app_check(&nor.flash, &app, work);
    uint32_t updates = 0;
    core_request_scan(&nor.flash, header.alignment, work, &updates);
    struct core_wipe plan;
    bool can_wipe = core_wipe_plan(&nor.flash, &app, SMK, &plan);
    unsigned selection = 0, last = 0, previous = 0, stable = 0;
    bool first_draw = true;
    for (; ; ) {
        const char *actions[4];
        unsigned types[4], count = 0;
        if (app.state == CORE_APP_OK) {
            actions[count] = "BOOT APP";
            types[count++] = 0;
        }
        actions[count] = "DOWNLOAD MODE";
        types[count++] = 1;
        if (effective_width == config.width && app.base && (app.state == CORE_APP_BAD_CRC || app.state == CORE_APP_INCOMPLETE)) {
            actions[count] = "BOOT ANYWAY";
            types[count++] = 2;
        }
        if (can_wipe) {
            actions[count] = "WIPE USER DATA";
            types[count++] = 3;
        }
        if (selection >= count)
            selection = 0;
        display_text(0, YELLOW, TITLE);
        display_text(1, GREY, (const char *)(uintptr_t)0x01c02050);
        display_text(2, RED, reason ? reason:"");
        const char *states[] = {
            "APP NOT FOUND", "APP OK", "APP BAD CRC", "APP INCOMPLETE"
        };
        display_text(3, app.state == CORE_APP_OK ? GREEN:RED, states[app.state]);
        char line[21];
        unsigned pos = 0;
        line[0] = 0;
        if (app.app_length) {
            pos = number(line, 0, app.app_length, 10, 0);
            append(line, pos, " B");
        }
        display_text(4, WHITE, line);
        pos = append(line, 0, core_request_valid(RETAINED+8) ? "UPDATE RAM ":"UPDATE ");
        if (updates) {
            pos = number(line, pos, updates, 10, 0);
            append(line, pos, " FLASH");
        } else if (pos == 7)
            append(line, pos, "NONE");
        display_text(5, updates || core_request_valid(RETAINED+8) ? YELLOW:WHITE, line);
        pos = append(line, 0, "FLASH ");
        pos = number(line, pos, jedec, 16, 6);
        pos = append(line, pos, " ");
        pos = number(line, pos, nor.flash.capacity/1024, 10, 0);
        append(line, pos, "K");
        display_text(6, WHITE, line);
        display_text(7, GREY, "--------------------");
        for (unsigned i = 0; i < 5; ++i) {
            pos = append(line, 0, i == selection ? "> ":"  ");
            if (i < count)
                append(line, pos, actions[i]);
            else
                line[0] = 0;
            display_text(8+i, i == selection ? YELLOW:WHITE, line);
        }
        display_text(13, YELLOW, effective_width < config.width ? "SPI WIDTH FALLBACK":"");
        display_text(14, GREY, CONTROLS);
        if (first_draw) {
            /* Show status before waiting; entry-held PLAY is not a selection. */
            release_select();
            first_draw = false;
        }
        /* Redraw only on an action; polling keeps the multiplexed LEDs alive. */
        unsigned edge = 0;
        while (!edge) {
            board_wait(20, true);
            unsigned keys = board_keys();
            if (keys == previous)
                ++stable;
            else {
                previous = keys;
                stable = 1;
            }
            if (stable >= 2) {
                edge = keys&~last;
                last = keys;
            }
        }
        if (edge&KEY_PREV) {
            selection = (selection+count-1)%count;
            continue;
        }
        if (edge&KEY_NEXT) {
            selection = (selection+1)%count;
            continue;
        }
        if (!(edge&KEY_SELECT))
            continue;
        unsigned action = types[selection];
        if (action == 1)
            download_screen();
        if (action == 2 || action == 3) {
            if (!confirm(action == 3, &plan)) {
                release_select();
                last = previous = stable = 0;
                continue;
            }
        }
        if (action == 3) {
            if (!wipe_user(&plan)) {
                reason = "WIPE FAILED";
                can_wipe = false;
            }
            continue;
        }
        if (xip_attempted) {
            if (action == 2)
                memcpy(RETAINED, "BOOTANY", 8);
            hal_reset();
        }
        board_restore();
        return action == 2;
    }
}

__attribute__((noinline)) static void update(bool ram) {
    struct core_updater updater;
    contract_config(clock_meta, memory_meta, &config);
    contract_updater(update_meta, &app, unit_key, pointer(clock_meta), ram ? 0x01c7fdb0:pointer(memory_meta));
    *(volatile uint16_t *)(uintptr_t)0x01c7fd82 = 0;
    bool ok = core_updater_decode(&nor.flash, request, (void *)(uintptr_t)CORE_UPDATE_LOAD, CORE_UPDATE_MAX, work, &updater);
    if (ok) {
#ifdef BOOT_SIM
        boot_sim_call_updater(updater.load, update_meta, updater.stream_end);
#else
        void (*run)(void *, uint32_t) = (void *)(uintptr_t)updater.load;
        run(update_meta, updater.stream_end);
#endif
        reason = "UPDATE RETURNED";
    } else
        reason = "UPDATER REFUSED";
    if (ram)
        core_put_le16(RETAINED+8, 0);
    sync_cpu();
    /* §6.2: the updater may have changed both the SPI path and the contents. */
    hal_fault_clear();
    bool flash_ok = flash_init(config.width, config.spi_divider);
    effective_width = flash_read_width();
    nor.flash.capacity = flash_capacity();
    memset(&app, 0, sizeof(app));
    memset(&header, 0, sizeof(header));
    mounted = flash_ok && nor.flash.capacity == 0x100000 && core_app_mount_header(&nor.flash, unit_key, &app, &header);
    if (mounted)
        core_app_check(&nor.flash, &app, work);
    if (screen()) {
        clear_rows(2, 14);
        display_text(3, RED, reason);
        hal_delay_us(1000000);
    }
}

static bool xip_crc(void) {
    for (unsigned i = 0; i < app.file_count; ++i) {
        const struct core_record *r = app.files+i;
        if (core_crc16(0, (const void *)(uintptr_t)(0x02000000+r->address), r->length) != r->data_crc)
            return false;
    }
    return true;
}

void boot_main(const uint32_t *device) {
    bool bypass = contract_consume_bypass(RETAINED);
#ifndef BOOT_SIM
    memset(__bss_start, 0, (size_t)(__bss_end-__bss_start));
#endif
    hal_early();
    bool config_ok = read_config(device);
    if (!apply_config())
        rom_download();
    board_init(SMK);
    if (board_hold(KEY_RECOVERY, 1000))
        rom_download();
    bool want_menu = board_hold(KEY_SELECT, 1000);
    board_restore();
    bool flash_ok = flash_init(1, 1);
    identify();
    flash_ok = flash_ok && nor.flash.capacity == 0x100000 && flash_init(config.width, config.spi_divider);
    effective_width = flash_read_width();
    mounted = flash_ok && core_app_mount_header(&nor.flash, unit_key, &app, &header);
    enum core_guard_result guarded = core_guard(&nor, SMK, work);
    if (!config_ok)
        reason = "BAD CONFIG";
    else if (!flash_ok)
        reason = "FLASH ERROR";
    else if (guarded == CORE_GUARD_ERROR)
        reason = "TRIM ERROR";
    bool need_menu = want_menu || !config_ok || !flash_ok || guarded == CORE_GUARD_ERROR;
    bool updates_done = false;
    for (; ; ) {
        if (need_menu) {
            bypass = menu() || bypass;
            need_menu = false;
        }
        if (!updates_done) {
            updates_done = true;
            unsigned choice = contract_update_choice(RETAINED+8, mounted);
            bool found = choice == 1;
            if (found)
                memcpy(request, RETAINED+8, 80);
            else if (choice == 2)
                found = core_request_scan(&nor.flash, header.alignment, request, 0);
            if (found) {
                update(choice == 1);
                if (app.state != CORE_APP_OK) {
                    need_menu = true;
                    continue;
                }
            }
            else if (mounted && core_uart_request(RETAINED)) {
                memset(RETAINED, 0, 4);
                hal_reset();
            }
        }
        if (!mounted) {
            need_menu = true;
            continue;
        }
        xip_attempted = true;
        bool ok = hal_xip(app.base, unit_key, effective_width, config.spi_divider);
        if (ok && ((bypass && effective_width == config.width) || xip_crc())) {
            memcpy((void *)(uintptr_t)0x01c7fe20, header.bytes, 32);
            contract_app((void *)(uintptr_t)0x01c7fe08, app.base, unit_key, trim, mac);
            memset((void *)(uintptr_t)0x01c7fe58, 0, 8);
            REG(0x01c7fe60) = 0;
            board_restore();
            display_release();
            screen_ready = false;
            irq_off();
            sync_cpu();
#ifdef BOOT_SIM
            boot_sim_call_app(app.entry, (void *)(uintptr_t)0x01c7fe08);
#else
            void (*run)(void *) = (void *)(uintptr_t)app.entry;
            run((void *)(uintptr_t)0x01c7fe08);
#endif
            reason = "APP EXITED";
        } else if (!ok)
            reason = "XIP ERROR";
        bypass = false;
        flash_release();
        hal_fault_clear();
        flash_init(effective_width, config.spi_divider);
        effective_width = flash_read_width();
        if (mounted)
            core_app_check(&nor.flash, &app, work);
        need_menu = true;
    }
}
