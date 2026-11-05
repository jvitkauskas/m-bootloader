/* SPDX-License-Identifier: MIT */
#ifndef MB_USB_MIDI_H
#define MB_USB_MIDI_H
#include <stdbool.h>
#include <stdint.h>
/* A USB-MIDI device on USB0, polled, for the bootloader's MIDI mode. */
void usb_midi_start(void);
void usb_midi_poll(void);
bool usb_midi_configured(void);
/* Sends one whole F0..F7 message, polling until it has gone or the host has gone. */
void usb_midi_send(const uint8_t *data, uint32_t size);
void usb_midi_stop(void);
#endif
