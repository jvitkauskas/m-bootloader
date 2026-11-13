/* SPDX-License-Identifier: MIT */
#ifndef MB_USB_DISK_H
#define MB_USB_DISK_H
#include <stdbool.h>
#include <stdint.h>
/* USB DISK: the SD card as a USB mass storage device (bulk-only, SCSI) on USB0, polled, one sector at a time. A write
 * has reached the card before the host is told it succeeded. Ported from m-apps' usbdisk. */
/* In RAM that's free while the menu runs: it's where a staged updater or stage 2's images go (docs/SPEC.md §2.1, §11.5). */
#define USB_DISK_RAM 0x01c0a800u
struct usb_disk_buffers {
    /* Receive buffers need 4 bytes more than they hold, as the controller's DMA writes past a full packet. */
    uint8_t sector[512+4], command[64+4], reply[32], status[16];
};
enum usb_disk_end { USB_DISK_EJECTED, USB_DISK_LEFT, USB_DISK_NO_HOST };
/* Serves an open card (sd_disk_open) until the host ejects it, usb_disk_leave() says to, or no host comes for about
 * two minutes. */
enum usb_disk_end usb_disk_run(uint32_t sectors);
/* The caller's: asked every few milliseconds whether to leave, even in the middle of a transfer. */
bool usb_disk_leave(void);
#endif
