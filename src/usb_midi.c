/* SPDX-License-Identifier: MIT */
/* A USB-MIDI device on USB0: only what MIDI mode needs, polled, with no interrupts. The register
 * sequences were written from the facts in JieLi's AC79 SDK (Apache-2.0) and felucca-bsp's
 * chip/fm1_usb.h (MPL-2.0); no code was taken from either (NOTICE.md).
 * The controller is a MUSB, whose registers are reached one at a time through USB_CON1. */
#include "usb_midi.h"
#include "midi_update.h"
#include "platform.h"
#define CON0 0x11800u
#define CON1 0x11804u
#define EP_COUNT(n) (0x11808u+4u*(n))
#define EP0_ADDRESS 0x11818u
#define EP1_TX_ADDRESS 0x1181cu
#define EP_RX_ADDRESS(n) (0x11820u+8u*((n)-1u))
#define EP4_RX_ADDRESS 0x1183cu
#define IO_CON0 0x51000u
#define CLOCK_CON1 0x10010u
enum { FADDR = 0, POWER = 1, INT_TX1 = 2, INT_RX1 = 4, INT_USB = 6, INT_TX1_ENABLE = 7, INT_TX2_ENABLE = 8,
    INT_RX1_ENABLE = 9, INT_RX2_ENABLE = 10, INT_USB_ENABLE = 11, INDEX = 14, TX_MAX = 16, CSR0 = 17, TX_CSR1 = 17,
    TX_CSR2 = 18, RX_MAX = 19, RX_CSR1 = 20, RX_CSR2 = 21, RX_COUNT1 = 22, RX_COUNT2 = 23 };
#define WAIT_LIMIT 20000u
void settle(unsigned n);
#define PACKET 64u

static const uint8_t device_descriptor[18] = {
    18, 1, 0x10, 0x01, 0, 0, 0, PACKET, 0x09, 0x12, 0x02, 0x00, 0x00, 0x02, 0, 1, 0, 1
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
    9, 5, 0x01, 2, PACKET, 0, 0, 0, 0,             /* endpoint 1, out */
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, PACKET, 0, 0, 0, 0,             /* endpoint 1, in */
    5, 0x25, 1, 1, 3
};
static const uint8_t language_string[4] = { 4, 3, 0x09, 0x04 };
#ifdef BOARD_SMK
static const char product[] = "SMK-37 Update";
#else
static const char product[] = "FM-1 Update";
#endif

/* The controller's DMA writes past a full packet when receiving, so these have room to spare. */
static uint8_t ep0[PACKET+4] __attribute__((aligned(4)));
static uint8_t ep1_in[PACKET] __attribute__((aligned(4)));
static uint8_t ep1_out[PACKET+4] __attribute__((aligned(4)));
static uint8_t string_descriptor[2+2*(sizeof(product)-1)];
static const uint8_t *ep0_data;
static uint32_t ep0_left;
static uint8_t pending_address, configuration;
static bool address_pending, ep0_sending, zero_end, running;

/* Accesses a few cycles apart make the controller babble on the bus, and the host then turns the
 * port off: seen on the FM-1, and warned of in felucca-bsp's chip/usb.c. So they're kept apart. */
static __attribute__((noinline)) void sie_start(uint32_t command) {
    settle(40);
    REG(CON1) = command;
    sync_cpu();
}

static void sie_write(uint32_t r, uint32_t v) {
    if (!(REG(CON0)&4))
        return;
    sie_start((r << 8 & 0xffff00u) | (v&0xffu));
    for (uint32_t n = WAIT_LIMIT; n && !(REG(CON1)&0x8000u); --n)
        ;
}

static uint32_t sie_read(uint32_t r) {
    if (!(REG(CON0)&4))
        return 0;
    sie_start((r << 8 & 0xffbf00u) | 0x4000u);
    for (uint32_t n = WAIT_LIMIT; n; --n)
        if (REG(CON1)&0x8000u)
            return REG(CON1)&0xffu;
    return 0;
}

static void ep0_chunk(void) {
    uint32_t n = ep0_left > PACKET ? PACKET:ep0_left;
    for (uint32_t i = 0; i < n; ++i)
        ep0[i] = ep0_data[i];
    sync_cpu();
    REG(EP0_ADDRESS) = DMA_ADDRESS(ep0);
    REG(EP_COUNT(0)) = n;
    ep0_data += n;
    ep0_left -= n;
    bool last = !ep0_left && !(n == PACKET && zero_end);
    if (!ep0_left && n == PACKET)
        zero_end = false;
    sie_write(INDEX, 0);
    sie_write(CSR0, last ? 0x0a:0x02);
    ep0_sending = !last;
}

static void ep0_send(const uint8_t *data, uint32_t size, uint32_t asked) {
    if (size > asked)
        size = asked;
    ep0_data = data;
    ep0_left = size;
    zero_end = size < asked && !(size%PACKET);
    sie_write(INDEX, 0);
    sie_write(CSR0, 0x40);
    ep0_chunk();
}

static void ep1_configure(void) {
    REG(EP1_TX_ADDRESS) = DMA_ADDRESS(ep1_in);
    REG(EP_RX_ADDRESS(1)) = DMA_ADDRESS(ep1_out);
    sie_write(INDEX, 1);
    sie_write(TX_MAX, 0xff);
    sie_write(TX_CSR1, 0x48);
    sie_write(TX_CSR2, 0);
    sie_write(RX_MAX, 0xff);
    sie_write(RX_CSR1, 0x90);
    sie_write(RX_CSR2, 0);
    sie_write(INT_RX1_ENABLE, 0x02);
    REG(CON0) &= ~(1u << 20);
}

static const uint8_t *descriptor(uint32_t value, uint32_t *size) {
    switch (value >> 8) {
    case 1:
        *size = sizeof(device_descriptor);
        return device_descriptor;
    case 2:
        *size = sizeof(config_descriptor);
        return config_descriptor;
    case 3:
        if (!(value&255)) {
            *size = sizeof(language_string);
            return language_string;
        }
        if ((value&255) == 1) {
            string_descriptor[0] = sizeof(string_descriptor);
            string_descriptor[1] = 3;
            for (uint32_t i = 0; i < sizeof(product)-1; ++i) {
                string_descriptor[2+2*i] = (uint8_t)product[i];
                string_descriptor[3+2*i] = 0;
            }
            *size = sizeof(string_descriptor);
            return string_descriptor;
        }
        return 0;
    default:
        return 0;
    }
}

static void ep0_service(void) {
    static const uint8_t zero[2];
    if (address_pending) {
        sie_write(FADDR, pending_address);
        address_pending = false;
    }
    sie_write(INDEX, 0);
    uint32_t csr = sie_read(CSR0);
    if (csr&0x04) {                             /* we sent a stall */
        sie_write(CSR0, 0);
        ep0_sending = false;
        return;
    }
    if (csr&0x10) {                             /* the host abandoned the transfer */
        sie_write(CSR0, 0x80);
        ep0_sending = false;
    }
    if (ep0_sending) {
        if (!(csr&0x02))
            ep0_chunk();
        return;
    }
    if (!(csr&0x01))
        return;
    __asm__ volatile("ssync" ::: "memory");
    uint32_t value = ep0[2] | (uint32_t)ep0[3] << 8, asked = ep0[6] | (uint32_t)ep0[7] << 8;
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
        ep0_send(d, size, asked);
        return;
    }
    case 0x0009:                                /* set configuration */
        if (ep0[2] > 1)
            goto stall;
        configuration = ep0[2];
        if (configuration)
            ep1_configure();
        break;
    case 0x8008:
        ep0_send(&configuration, 1, asked);
        return;
    case 0x8000:
    case 0x8100:
    case 0x8200:
        ep0_send(zero, 2, asked);
        return;
    case 0x0102:                                /* clear or set endpoint or interface features */
    case 0x0302:
    case 0x000b:
    case 0x010b:
        break;
    default:
        goto stall;
    }
    sie_write(INDEX, 0);
    sie_write(CSR0, 0x48);
    return;
stall:
    sie_write(INDEX, 0);
    sie_write(CSR0, 0x60);
}

static void ep1_receive(void) {
    sie_write(INDEX, 1);
    uint32_t csr = sie_read(RX_CSR1) | sie_read(RX_CSR2) << 8;
    if (!(csr&1))
        return;
    uint32_t n = sie_read(RX_COUNT1) | sie_read(RX_COUNT2) << 8;
    if (n > PACKET)
        n = PACKET;
    __asm__ volatile("ssync" ::: "memory");
    for (uint32_t i = 0; i+3 < n; i += 4) {
        /* Code index 4 to 7 are the SysEx ones, and say how many of the 3 bytes count. */
        uint32_t code = ep1_out[i]&15, count = code == 5 ? 1:code == 6 ? 2:3;
        if (code < 4 || code > 7)
            continue;
        for (uint32_t b = 0; b < count; ++b)
            mu_rx(ep1_out[i+1+b]);
    }
    csr = (csr&~0x164u) | 0x10u;
    sie_write(RX_CSR1, csr&0xff);
    sie_write(RX_CSR2, csr >> 8);
}

void usb_midi_poll(void) {
    if (!running)
        return;
    if (REG(CON0)&BIT(13))                      /* a frame's start is pending: acknowledge it */
        REG(CON0) |= BIT(12);
    uint32_t bus = sie_read(INT_USB), tx = sie_read(INT_TX1), rx = sie_read(INT_RX1);
    if (bus&0x04) {                             /* bus reset */
        sie_write(FADDR, 0);
        configuration = 0;
        ep0_sending = address_pending = false;
        REG(EP0_ADDRESS) = DMA_ADDRESS(ep0);
        sie_write(INT_USB_ENABLE, 0x07);
        sie_write(INT_TX1_ENABLE, 0x01);
        sie_write(INT_RX1_ENABLE, 0);
    }
    if (tx&0x01)
        ep0_service();
    if ((rx&0x02) && configuration)
        ep1_receive();
}

bool usb_midi_configured(void) {
    return configuration;
}

void usb_midi_start(void) {
    REG(CON0) = 0;
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
    REG(CON0) &= ~0x4035u;
    REG(CON1) = 0;
    REG(CON0) |= 1;
    REG(IO_CON0) &= ~0xf0u;
    REG(IO_CON0) |= 0x40;                       /* D+ pull-up: on the bus */
    REG(CON0) |= 0x3c;
    sie_write(POWER, 0x60);
    sie_write(INT_USB_ENABLE, 0x07);
    sie_write(INT_TX1_ENABLE, 0x01);
    sie_write(INT_TX2_ENABLE, 0);
    sie_write(INT_RX1_ENABLE, 0);
    sie_write(INT_RX2_ENABLE, 0);
    REG(CON0) &= ~(0x1fu << 19);
    running = true;
    configuration = 0;
}

static bool ep1_wait(void) {
    /* About 200 ms of polling: a host that isn't reading loses the message, and asks again. */
    for (unsigned n = 0; n < 400; ++n) {
        sie_write(INDEX, 1);
        uint32_t csr = sie_read(TX_CSR1);
        if (!(csr&0x01)) {
            if (csr&0x80)
                sie_write(TX_CSR1, csr&~0x80u);
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
        if (!configuration || !ep1_wait())
            return;
        while (at < size && n < PACKET) {
            uint32_t left = size-at;
            uint32_t count = left > 3 ? 3:left;
            ep1_in[n] = count == 3 ? (left == 3 ? 7:4):(uint8_t)(4+count);
            for (uint32_t b = 0; b < 3; ++b)
                ep1_in[n+1+b] = b < count ? data[at+b]:0;
            at += count;
            n += 4;
        }
        sync_cpu();
        REG(EP1_TX_ADDRESS) = DMA_ADDRESS(ep1_in);
        REG(EP_COUNT(1)) = n;
        sie_write(INDEX, 1);
        sie_write(TX_CSR1, sie_read(TX_CSR1) | 0x01);
    }
}

void usb_midi_stop(void) {
    running = false;
    REG(CON0) = 0;
    REG(IO_CON0) = 0x0c;
}
