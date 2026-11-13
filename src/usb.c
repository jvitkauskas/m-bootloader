/* SPDX-License-Identifier: MIT */
/* USB0's device side, for MIDI mode and USB DISK: polled, with no interrupts. The register sequences follow JieLi's
 * AC79 SDK (Apache-2.0) and felucca-bsp's chip/fm1_usb.h (MPL-2.0). */
#include "usb.h"
#include "platform.h"
#define CON1 0x11804u
#define EP0_ADDRESS 0x11818u
#define EP_RX_ADDRESS(n) (0x11820u+8u*((n)-1u))
#define EP4_RX_ADDRESS 0x1183cu
#define IO_CON0 0x51000u
#define CLOCK_CON1 0x10010u
enum { FADDR = 0, POWER = 1, INT_TX1 = 2, INT_USB = 6, INT_TX1_ENABLE = 7, INT_TX2_ENABLE = 8, INT_RX1_ENABLE = 9,
    INT_RX2_ENABLE = 10, INT_USB_ENABLE = 11, TX_MAX = 16, CSR0 = 17, TX_CSR2 = 18, RX_MAX = 19 };
#define WAIT_LIMIT 20000u
void settle(unsigned n);

/* The controller's DMA writes past a full packet when receiving, so this has room to spare. */
static uint8_t ep0[USB_PACKET+4] __attribute__((aligned(4)));
static uint8_t string_descriptor[2+2*16];
static const struct usb_class *class;
static const uint8_t *ep0_data;
static uint32_t ep0_left;
/* The selected endpoint, so that a class's data path doesn't reselect it for every packet. */
static uint32_t selected;
static uint8_t pending_address, configuration;
static bool address_pending, ep0_sending, zero_end, running;
bool usb_halted_in, usb_halted_out;

/* Accesses a few cycles apart make the controller babble on the bus, and the host then turns the
 * port off: seen on the FM-1, and warned of in felucca-bsp's chip/usb.c. So they're kept apart. */
static __attribute__((noinline)) void sie_start(uint32_t command) {
    settle(40);
    REG(CON1) = command;
    sync_cpu();
}

void usb_write(uint32_t r, uint32_t v) {
    if (!(REG(USB_CON0)&4))
        return;
    sie_start((r << 8 & 0xffff00u) | (v&0xffu));
    for (uint32_t n = WAIT_LIMIT; n && !(REG(CON1)&0x8000u); --n)
        ;
}

uint32_t usb_read(uint32_t r) {
    if (!(REG(USB_CON0)&4))
        return 0;
    sie_start((r << 8 & 0xffbf00u) | 0x4000u);
    for (uint32_t n = WAIT_LIMIT; n; --n)
        if (REG(CON1)&0x8000u)
            return REG(CON1)&0xffu;
    return 0;
}

void usb_select(uint32_t endpoint) {
    if (selected != endpoint) {
        selected = endpoint;
        usb_write(USB_INDEX, endpoint);
    }
}

static void ep0_chunk(void) {
    uint32_t n = ep0_left > USB_PACKET ? USB_PACKET:ep0_left;
    for (uint32_t i = 0; i < n; ++i)
        ep0[i] = ep0_data[i];
    sync_cpu();
    REG(EP0_ADDRESS) = DMA_ADDRESS(ep0);
    REG(USB_EP_COUNT(0)) = n;
    ep0_data += n;
    ep0_left -= n;
    bool last = !ep0_left && !(n == USB_PACKET && zero_end);
    if (!ep0_left && n == USB_PACKET)
        zero_end = false;
    usb_select(0);
    usb_write(CSR0, last ? 0x0a:0x02);
    ep0_sending = !last;
}

void usb_ep0_send(const uint8_t *data, uint32_t size, uint32_t asked) {
    if (size > asked)
        size = asked;
    ep0_data = data;
    ep0_left = size;
    zero_end = size < asked && !(size%USB_PACKET);
    usb_select(0);
    usb_write(CSR0, 0x40);
    ep0_chunk();
}

void usb_ep1_reset(const uint8_t *in, uint8_t *out) {
    REG(USB_EP1_TX_ADDRESS) = DMA_ADDRESS(in);
    REG(USB_EP1_RX_ADDRESS) = DMA_ADDRESS(out);
    usb_select(1);
    usb_write(TX_MAX, 0xff);
    usb_write(USB_TX_CSR1, 0x48);
    usb_write(TX_CSR2, 0);
    usb_write(RX_MAX, 0xff);
    usb_write(USB_RX_CSR1, 0x90);
    usb_write(USB_RX_CSR2, 0);
    usb_halted_in = usb_halted_out = false;
}

void usb_halt(uint32_t endpoint, bool on) {
    usb_select(1);
    if (endpoint == USB_EP_IN) {
        usb_write(USB_TX_CSR1, on ? 0x10:0x48);
        usb_halted_in = on;
    } else if (endpoint == USB_EP_OUT) {
        usb_write(USB_RX_CSR1, on ? 0x20:0x90);
        usb_halted_out = on;
    }
}

static const uint8_t *string(const char *text, uint32_t *size) {
    uint32_t n = 0;
    for (; text[n] && n < 16; ++n) {
        string_descriptor[2+2*n] = (uint8_t)text[n];
        string_descriptor[3+2*n] = 0;
    }
    string_descriptor[0] = (uint8_t)(2+2*n);
    string_descriptor[1] = 3;
    *size = 2+2*n;
    return string_descriptor;
}

static const uint8_t *descriptor(uint32_t value, uint32_t *size) {
    static const uint8_t language_string[4] = { 4, 3, 0x09, 0x04 };
    switch (value >> 8) {
    case 1:
        *size = 18;
        return class->device;
    case 2:
        *size = class->config[2] | (uint32_t)class->config[3] << 8;
        return class->config;
    case 3:
        if (!(value&255)) {
            *size = sizeof(language_string);
            return language_string;
        }
        if ((value&255) == 1)
            return string(class->product, size);
        if ((value&255) == 2 && class->serial)
            return string(class->serial, size);
        return 0;
    default:
        return 0;
    }
}

static void ep0_service(void) {
    static const uint8_t zero[2];
    static const uint8_t halted[2] = {1, 0};
    if (address_pending) {
        usb_write(FADDR, pending_address);
        address_pending = false;
    }
    usb_select(0);
    uint32_t csr = usb_read(CSR0);
    if (csr&0x04) {                             /* we sent a stall */
        usb_write(CSR0, 0);
        ep0_sending = false;
        return;
    }
    if (csr&0x10) {                             /* the host abandoned the transfer */
        usb_write(CSR0, 0x80);
        ep0_sending = false;
    }
    if (ep0_sending) {
        if (!(csr&0x02))
            ep0_chunk();
        return;
    }
    if (!(csr&0x01))
        return;
    sync_dma();
    switch (class->request ? class->request(ep0):USB_STANDARD) {
    case USB_SENT:
        return;
    case USB_ACK:
        goto ack;
    case USB_STALL:
        goto stall;
    case USB_STANDARD:
        break;
    }
    uint32_t value = ep0[2] | (uint32_t)ep0[3] << 8, index = ep0[4], asked = ep0[6] | (uint32_t)ep0[7] << 8;
    switch ((uint32_t)ep0[0] << 8 | ep0[1]) {
    case 0x0005:                                /* set address */
        pending_address = ep0[2]&0x7f;
        address_pending = true;
        break;
    case 0x8006: {                              /* get descriptor */
        uint32_t size;
        const uint8_t *d = descriptor(value, &size);
        if (!d)
            goto stall;
        if (!asked)
            break;
        usb_ep0_send(d, size, asked);
        return;
    }
    case 0x0009:                                /* set configuration */
        if (ep0[2] > 1)
            goto stall;
        configuration = ep0[2];
        if (configuration) {
            class->configured();
            usb_write(INT_RX1_ENABLE, 0x02);
            REG(USB_CON0) &= ~(1u << 20);
        }
        if (class->reset)
            class->reset();
        break;
    case 0x8008:
        usb_ep0_send(&configuration, 1, asked);
        return;
    case 0x8000:
    case 0x8100:
        usb_ep0_send(zero, 2, asked);
        return;
    case 0x8200:                                /* an endpoint's status */
        usb_ep0_send((index == USB_EP_IN && usb_halted_in) || (index == USB_EP_OUT && usb_halted_out) ? halted:zero, 2,
                     asked);
        return;
    case 0x0201:                                /* clear or set an endpoint's halt */
    case 0x0203:
        usb_halt(index, ep0[1] == 3);
        break;
    case 0x0102:                                /* clear or set endpoint or interface features */
    case 0x0302:
    case 0x000b:
    case 0x010b:
        break;
    default:
        goto stall;
    }
ack:
    usb_select(0);
    usb_write(CSR0, 0x48);
    return;
stall:
    usb_select(0);
    usb_write(CSR0, 0x60);
}

void usb_poll(void) {
    if (!running)
        return;
    if (REG(USB_CON0)&BIT(13))                  /* a frame's start is pending: acknowledge it */
        REG(USB_CON0) |= BIT(12);
    uint32_t bus = usb_read(INT_USB), tx = usb_read(INT_TX1);
    if (bus&0x04) {                             /* bus reset */
        selected = 0xff;
        usb_write(FADDR, 0);
        configuration = 0;
        ep0_sending = address_pending = false;
        REG(EP0_ADDRESS) = DMA_ADDRESS(ep0);
        usb_write(INT_USB_ENABLE, 0x07);
        usb_write(INT_TX1_ENABLE, 0x01);
        usb_write(INT_RX1_ENABLE, 0);
        if (class->reset)
            class->reset();
    }
    if (tx&0x01)
        ep0_service();
}

bool usb_configured(void) {
    return configuration;
}

void usb_start(const struct usb_class *c) {
    class = c;
    REG(USB_CON0) = 0;
    REG(CON1) = 0;
    REG(IO_CON0) = 0x0c;
    hal_delay_us(25000);
    REG(CLOCK_CON1) &= ~3u;                     /* USB clock: PLL 48 MHz */
    REG(EP0_ADDRESS) = DMA_ADDRESS(ep0);
    for (unsigned n = 1; n <= 3; ++n)
        REG(EP_RX_ADDRESS(n)) = DMA_ADDRESS(ep0);
    REG(EP4_RX_ADDRESS) = DMA_ADDRESS(ep0);
    REG(IO_CON0) |= BIT(10) | BIT(9);
    REG(IO_CON0) &= ~BIT(11);
    REG(IO_CON0) |= BIT(12);
    REG(USB_CON0) &= ~0x4035u;
    REG(CON1) = 0;
    REG(USB_CON0) |= 1;
    REG(IO_CON0) &= ~0xf0u;
    REG(IO_CON0) |= 0x40;                       /* D+ pull-up: on the bus */
    REG(USB_CON0) |= 0x3c;
    selected = 0xff;
    usb_write(POWER, 0x60);
    usb_write(INT_USB_ENABLE, 0x07);
    usb_write(INT_TX1_ENABLE, 0x01);
    usb_write(INT_TX2_ENABLE, 0);
    usb_write(INT_RX1_ENABLE, 0);
    usb_write(INT_RX2_ENABLE, 0);
    REG(USB_CON0) &= ~(0x1fu << 19);
    running = true;
    configuration = 0;
    ep0_sending = address_pending = false;
    usb_halted_in = usb_halted_out = false;
}

void usb_stop(void) {
    running = false;
    configuration = 0;
    REG(USB_CON0) = 0;
    REG(IO_CON0) = 0x0c;
}
