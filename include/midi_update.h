/* SPDX-License-Identifier: MIT */
#ifndef MB_MIDI_UPDATE_H
#define MB_MIDI_UPDATE_H
#include <stdbool.h>
#include <stdint.h>
#define MU_SECTOR 0x1000u
/* The first and last address written: never the boot area, nor the key area at 0xFF000. */
#define MU_FIRST 0x4000u
#define MU_LIMIT 0xff000u
enum mu_result {
    MU_CANCELLED, /* nothing was written */
    MU_DONE,
    MU_DOWNLOAD,  /* the host asked for download mode */
    MU_BAD_PACKAGE,
    MU_WRONG_KEY,
    MU_LOST,      /* the host stopped answering */
    MU_FLASH_ERROR
};
/* A byte from the USB-MIDI stream. */
void mu_rx(uint8_t byte);
/* Answers the updater's protocol until a package is installed or mu_cancel() says stop.
 * sector is MU_SECTOR bytes of scratch. Whether flash was changed is in *written. */
enum mu_result mu_run(uint16_t key, uint8_t *sector, bool *written);
/* The hooks. */
void mu_send(const uint8_t *data, uint32_t size);    /* one whole F0..F7 message */
void mu_idle(void);                                  /* runs USB, and takes about a millisecond */
bool mu_cancel(void);
bool mu_read(uint32_t address, void *out, uint32_t size);
bool mu_erase(uint32_t address);
bool mu_program(uint32_t address, const void *data, uint32_t size);
/* Each pass counts 0 to 100: reading the package to check it, then writing it. */
void mu_progress(bool writing, unsigned percent);
#endif
