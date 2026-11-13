/* SPDX-License-Identifier: MIT */
#ifndef MB_USB_H
#define MB_USB_H
#include <stdbool.h>
#include <stdint.h>
/* A polled USB device on USB0, for MIDI mode and USB DISK: endpoint 0 here, and a class's bulk endpoint 1, in and out.
 * The controller is a MUSB, whose registers are reached one at a time through USB_CON1. */
#define USB_PACKET 64u
#define USB_CON0 0x11800u
#define USB_EP_COUNT(n) (0x11808u+4u*(n))
#define USB_EP1_TX_ADDRESS 0x1181cu
#define USB_EP1_RX_ADDRESS 0x11820u
/* The controller's registers, by number. */
enum { USB_INT_RX1 = 4, USB_INDEX = 14, USB_TX_CSR1 = 17, USB_RX_CSR1 = 20, USB_RX_CSR2 = 21, USB_RX_COUNT1 = 22,
    USB_RX_COUNT2 = 23 };
#define USB_EP_IN 0x81u
#define USB_EP_OUT 0x01u
/* What a class does with a request on endpoint 0. */
enum usb_answer { USB_STANDARD, USB_ACK, USB_STALL, USB_SENT };
struct usb_class {
    const uint8_t *device, *config;     /* the configuration's total length is its bytes 2 and 3 */
    const char *product, *serial;       /* strings 1 and 2; no serial if 0 */
    /* The host chose configuration 1: set up endpoint 1, with usb_ep1_reset(). */
    void (*configured)(void);
    /* A bus reset, or the configuration changed. May be 0. */
    void (*reset)(void);
    /* Sees every request first, as the 8-byte setup packet, and answers with usb_ep0_send() for USB_SENT. May be 0. */
    enum usb_answer (*request)(const uint8_t *setup);
};
void usb_start(const struct usb_class *c);
/* Bus resets and endpoint 0. */
void usb_poll(void);
bool usb_configured(void);
void usb_stop(void);
void usb_write(uint32_t r, uint32_t v);
uint32_t usb_read(uint32_t r);
/* Selects an endpoint's registers, only if they aren't already. */
void usb_select(uint32_t endpoint);
void usb_ep0_send(const uint8_t *data, uint32_t size, uint32_t asked);
/* Endpoint 1's buffers, with both directions ready and neither halted. A receive buffer needs 4 bytes more than a
 * packet, as the controller's DMA writes past a full one. */
void usb_ep1_reset(const uint8_t *in, uint8_t *out);
/* Endpoint 1's halts, which the host sees with GET_STATUS and clears. */
extern bool usb_halted_in, usb_halted_out;
void usb_halt(uint32_t endpoint, bool on);
#endif
