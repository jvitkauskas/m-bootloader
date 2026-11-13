/* SPDX-License-Identifier: MIT */
/* A USB-MIDI device on USB0 (usb.c): only what MIDI mode needs, polled, with no interrupts. */
#include "usb_midi.h"
#include "midi_update.h"
#include "platform.h"
#include "usb.h"

static const uint8_t device_descriptor[18] = {
    18, 1, 0x10, 0x01, 0, 0, 0, USB_PACKET, 0x09, 0x12, 0x02, 0x00, 0x00, 0x02, 0, 1, 0, 1
};
static const uint8_t config_descriptor[101] = {
    9, 2, 101, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,                     /* audio control */
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,                     /* MIDI streaming */
    7, 0x24, 1, 0x00, 0x01, 65, 0,
    6, 0x24, 2, 1, 1, 0,                           /* jacks */
    6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 0x01, 2, USB_PACKET, 0, 0, 0, 0,         /* endpoint 1, out */
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, USB_PACKET, 0, 0, 0, 0,         /* endpoint 1, in */
    5, 0x25, 1, 1, 3
};

/* The controller's DMA writes past a full packet when receiving, so this has room to spare. */
static uint8_t ep1_in[USB_PACKET] __attribute__((aligned(4)));
static uint8_t ep1_out[USB_PACKET+4] __attribute__((aligned(4)));

static void configured(void) {
    usb_ep1_reset(ep1_in, ep1_out);
}

static const struct usb_class midi = {
    device_descriptor, config_descriptor,
#ifdef BOARD_SMK
    "SMK-37 Update",
#else
    "FM-1 Update",
#endif
    0, configured, 0, 0
};

static void ep1_receive(void) {
    usb_select(1);
    uint32_t csr = usb_read(USB_RX_CSR1) | usb_read(USB_RX_CSR2) << 8;
    if (!(csr&1))
        return;
    uint32_t n = usb_read(USB_RX_COUNT1) | usb_read(USB_RX_COUNT2) << 8;
    if (n > USB_PACKET)
        n = USB_PACKET;
    sync_dma();
    for (uint32_t i = 0; i+3 < n; i += 4) {
        /* Code index 4 to 7 are the SysEx ones, and say how many of the 3 bytes count. */
        uint32_t code = ep1_out[i]&15, count = code == 5 ? 1:code == 6 ? 2:3;
        if (code < 4 || code > 7)
            continue;
        for (uint32_t b = 0; b < count; ++b)
            mu_rx(ep1_out[i+1+b]);
    }
    csr = (csr&~0x164u) | 0x10u;
    usb_write(USB_RX_CSR1, csr&0xff);
    usb_write(USB_RX_CSR2, csr >> 8);
}

void usb_midi_poll(void) {
    usb_poll();
    if (usb_configured() && (usb_read(USB_INT_RX1)&0x02))
        ep1_receive();
}

bool usb_midi_configured(void) {
    return usb_configured();
}

void usb_midi_start(void) {
    usb_start(&midi);
}

static bool ep1_wait(void) {
    /* About 200 ms of polling: a host that isn't reading loses the message, and asks again. */
    for (unsigned n = 0; n < 400; ++n) {
        usb_select(1);
        uint32_t csr = usb_read(USB_TX_CSR1);
        if (!(csr&0x01)) {
            if (csr&0x80)
                usb_write(USB_TX_CSR1, csr&~0x80u);
            return true;
        }
        usb_midi_poll();
        hal_delay_us(500);
    }
    return false;
}

void usb_midi_send(const uint8_t *data, uint32_t size) {
    uint32_t at = 0;
    while (at < size) {
        uint32_t n = 0;
        if (!usb_configured() || !ep1_wait())
            return;
        while (at < size && n < USB_PACKET) {
            uint32_t left = size-at;
            uint32_t count = left > 3 ? 3:left;
            ep1_in[n] = count == 3 ? (left == 3 ? 7:4):(uint8_t)(4+count);
            for (uint32_t b = 0; b < 3; ++b)
                ep1_in[n+1+b] = b < count ? data[at+b]:0;
            at += count;
            n += 4;
        }
        sync_cpu();
        REG(USB_EP1_TX_ADDRESS) = DMA_ADDRESS(ep1_in);
        REG(USB_EP_COUNT(1)) = n;
        usb_select(1);
        usb_write(USB_TX_CSR1, usb_read(USB_TX_CSR1) | 0x01);
    }
}

void usb_midi_stop(void) {
    usb_stop();
}
