/* SPDX-License-Identifier: MIT */
/* src/usb.c, with src/usb_midi.c's class, against a host enumerating it through a stand-in for USB0's controller: its
 * registers reached one at a time through USB_CON1, and endpoint 0's packets by DMA. The host side follows the USB 2.0
 * specification's control transfers; the controller's bits are the ones usb.c is written for (MUSB's CSR0). */
#define _DEFAULT_SOURCE
#include "../src/usb.c"
#include "../src/usb_midi.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CON1 0x11804u

void settle(unsigned n) {
    (void)n;
}

bool hal_delay_us(uint32_t usec) {
    (void)usec;
    return true;
}

static uint8_t midi_bytes[64];
static unsigned midi_count;

void mu_rx(uint8_t b) {
    midi_bytes[midi_count++] = b;
}

/* The chip's registers by address, and the controller's own, by number. */
static uint32_t con0, con1, con1_left, io_con0, clock_con1, ep0_address, ep1_tx, ep1_rx, ep_rx[4], ep_count[5], other;
static uint8_t sie[32];
static unsigned sie_writes[32];
/* Endpoint 0: what the host has sent, and what's pending for the device to see. */
static uint8_t int_usb, int_tx1, int_rx1;
static uint8_t csr0;
static uint8_t ep0_in[512];
static unsigned ep0_in_len, ep0_packets;
static bool ep0_stalled, ep0_acked;

static uint32_t sie_get(uint32_t r) {
    uint32_t v;
    switch (r) {
    case 6:
        v = int_usb;
        int_usb = 0;
        return v;
    case 2:
        v = int_tx1;
        int_tx1 = 0;
        return v;
    case 4:
        v = int_rx1;
        int_rx1 = 0;
        return v;
    case 17:
        return sie[14] == 0 ? csr0:sie[17];
    default:
        return sie[r];
    }
}

/* What the device does to endpoint 0's status, as the controller and the host see it. */
static void csr0_write(uint8_t v) {
    if (v&0x80)                                 /* setup end serviced */
        csr0 &= (uint8_t)~0x10;
    if (v&0x40)                                 /* the setup packet taken */
        csr0 &= (uint8_t)~0x01;
    if (v&0x20) {                               /* stall: sent, which interrupts */
        ep0_stalled = true;
        csr0 |= 0x04;
        int_tx1 |= 1;
        return;
    }
    if (!v)
        csr0 &= (uint8_t)~0x04;
    if (v&0x02) {                               /* a packet to send: the host takes it at once */
        unsigned n = ep_count[0];
        assert(n <= 64 && ep0_in_len+n <= sizeof(ep0_in));
        memcpy(ep0_in+ep0_in_len, (const uint8_t *)(uintptr_t)ep0_address, n);
        ep0_in_len += n;
        ++ep0_packets;
        if (!(v&0x08))
            int_tx1 |= 1;                       /* ready for the next */
    }
    if ((v&0x48) == 0x48)
        ep0_acked = true;
}

static void sie_set(uint32_t r, uint32_t v) {
    ++sie_writes[r];
    if (r == 17 && sie[14] == 0) {
        csr0_write((uint8_t)v);
        return;
    }
    sie[r] = (uint8_t)v;
}

volatile uint32_t *platform_register(uintptr_t a) {
    switch (a) {
    case USB_CON0:
        return &con0;
    case CON1:
        /* A command written and not yet done (0 is one: register 0 set to 0): do it, if the controller's on. */
        if (con1 != con1_left && (con0&4)) {
            uint32_t r = con1 >> 8&0x3f;
            if (con1&0x4000u)
                con1 = 0x8000u | sie_get(r);
            else {
                sie_set(r, con1&0xff);
                con1 = 0x8000u;
            }
        }
        con1_left = con1;
        return &con1;
    case IO_CON0:
        return &io_con0;
    case CLOCK_CON1:
        return &clock_con1;
    case EP0_ADDRESS:
        return &ep0_address;
    case USB_EP1_TX_ADDRESS:
        return &ep1_tx;
    case USB_EP1_RX_ADDRESS:
        return &ep1_rx;
    case 0x11828u:
        return &ep_rx[2];
    case 0x11830u:
        return &ep_rx[3];
    case EP4_RX_ADDRESS:
        return &ep_rx[0];
    default:
        if (a >= USB_EP_COUNT(0) && a <= USB_EP_COUNT(4))
            return &ep_count[(a-USB_EP_COUNT(0))/4];
        assert(!"register");
        return &other;
    }
}

/* A control transfer: the setup packet, then polling until the device has answered. Returns the bytes of an IN data
 * stage, or -1 for a stall. */
static int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, uint8_t *in) {
    uint8_t setup[8] = {type, request, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
                        (uint8_t)length, (uint8_t)(length >> 8)};
    memcpy((uint8_t *)(uintptr_t)ep0_address, setup, 8);
    ep0_in_len = ep0_packets = 0;
    ep0_stalled = ep0_acked = false;
    csr0 |= 0x01;
    int_tx1 |= 1;
    for (unsigned n = 0; n < 32 && !ep0_stalled && !ep0_acked; ++n) {
        usb_poll();
        /* An IN data stage ends at its last packet, which the device marks; the host then sends the status. */
        if (ep0_in_len && !int_tx1 && !(csr0&0x01))
            break;
    }
    if (ep0_stalled) {
        usb_poll();                             /* the device sees its stall sent */
        return -1;
    }
    if (in)
        memcpy(in, ep0_in, ep0_in_len);
    return (int)ep0_in_len;
}

static void bus_reset(void) {
    int_usb |= 0x04;
    usb_poll();
}

/* A class of the test's own, for the hooks and the zero-length end of a full last packet. */
static const uint8_t own_device[18] = {18, 1, 0x10, 1, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 1, 2, 1};
static uint8_t own_config[64] = {9, 2, 64, 0, 1, 1, 0, 0x80, 50};
static uint8_t own_in[64], own_out[68];
static unsigned own_configured, own_resets, own_requests;

static void own_configure(void) {
    ++own_configured;
    usb_ep1_reset(own_in, own_out);
}

static void own_reset(void) {
    ++own_resets;
}

static enum usb_answer own_request(const uint8_t *setup) {
    static const uint8_t answer[3] = {7, 8, 9};
    ++own_requests;
    switch (setup[0] << 8 | setup[1]) {
    case 0xc101:
        usb_ep0_send(answer, 3, setup[6]);
        return USB_SENT;
    case 0x4102:
        return USB_ACK;
    case 0x4103:
        return USB_STALL;
    default:
        return USB_STANDARD;
    }
}

static const struct usb_class own = {own_device, own_config, "THE TEST", "42", own_configure, own_reset, own_request};

static bool utf16(const uint8_t *d, int n, const char *text) {
    size_t len = strlen(text);
    if (n != (int)(2+2*len) || d[0] != n || d[1] != 3)
        return false;
    for (size_t i = 0; i < len; ++i)
        if (d[2+2*i] != (uint8_t)text[i] || d[3+2*i])
            return false;
    return true;
}

int main(void) {
    uint8_t d[512];
    int n;

    /* MIDI mode's device. */
    usb_midi_start();
    assert(con0&4 && io_con0&0x40 && !usb_midi_configured());
    bus_reset();
    n = control(0x80, 6, 0x0100, 0, 64, d);
    assert(n == 18 && d[0] == 18 && d[1] == 1 && d[8] == 0x09 && d[9] == 0x12 && d[10] == 0x02 && d[16] == 0);
    /* The address takes effect after the status stage, at the next interrupt. */
    assert(control(0x00, 5, 9, 0, 0, 0) == 0 && ep0_acked && sie[0] == 0);
    n = control(0x80, 6, 0x0100, 0, 18, d);
    assert(n == 18 && sie[0] == 9);
    /* The configuration in two packets, or as much as asked. */
    n = control(0x80, 6, 0x0200, 0, 255, d);
    assert(n == 101 && ep0_packets == 2 && d[1] == 2 && d[2] == 101 && d[4] == 2);
    n = control(0x80, 6, 0x0200, 0, 9, d);
    assert(n == 9 && ep0_packets == 1);
    n = control(0x80, 6, 0x0300, 0, 255, d);
    assert(n == 4 && d[0] == 4 && d[1] == 3 && d[2] == 0x09 && d[3] == 0x04);
    n = control(0x80, 6, 0x0301, 0x409, 255, d);
    assert(utf16(d, n, "FM-1 Update"));
    /* No serial number, and nothing of other types. */
    assert(control(0x80, 6, 0x0302, 0x409, 255, d) == -1);
    assert(control(0x80, 6, 0x0600, 0, 10, d) == -1);
    /* A descriptor asked for with no room is just acknowledged. */
    assert(control(0x80, 6, 0x0100, 0, 0, d) == 0 && ep0_acked);
    /* Configured: endpoint 1 set up both ways. */
    assert(control(0x80, 8, 0, 0, 1, d) == 1 && d[0] == 0);
    assert(control(0x00, 9, 1, 0, 0, 0) == 0 && usb_midi_configured());
    assert(ep1_tx == (uint32_t)(uintptr_t)ep1_in && ep1_rx == (uint32_t)(uintptr_t)ep1_out && sie[9] == 0x02);
    assert(control(0x80, 8, 0, 0, 1, d) == 1 && d[0] == 1);
    assert(control(0x00, 9, 2, 0, 0, 0) == -1 && usb_midi_configured());
    /* Status: none of its own, nor of the interface or an endpoint. */
    assert(control(0x80, 0, 0, 0, 2, d) == 2 && !d[0] && !d[1]);
    assert(control(0x81, 0, 0, 0, 2, d) == 2 && !d[0] && !d[1]);
    assert(control(0x82, 0, 0, 0x81, 2, d) == 2 && !d[0] && !d[1]);
    /* An unknown request is stalled, and the next one works. */
    assert(control(0xc0, 0x33, 0, 0, 4, d) == -1);
    assert(control(0x80, 6, 0x0100, 0, 18, d) == 18);
    /* A packet of SysEx on endpoint 1: only its message bytes, by each packet's code. */
    static const uint8_t packet[12] = {0x04, 0xf0, 0x7d, 0x01, 0x06, 0x02, 0xf7, 0x00, 0x0f, 0xf8, 0, 0};
    memcpy(ep1_out, packet, sizeof(packet));
    sie[14] = 1;
    sie[20] = 1;
    sie[22] = sizeof(packet);
    int_rx1 = 0x02;
    midi_count = 0;
    usb_midi_poll();
    assert(midi_count == 5 && !memcmp(midi_bytes, "\xf0\x7d\x01\x02\xf7", 5) && sie[20]&0x10);
    /* A bus reset: unconfigured, at address 0. */
    bus_reset();
    assert(!usb_midi_configured() && sie[0] == 0);
    usb_midi_stop();
    assert(!usb_midi_configured() && !con0);

    /* A class of its own: the hooks, a serial number, halts, and a full last packet ended by an empty one. */
    usb_start(&own);
    bus_reset();
    assert(own_resets == 1);
    n = control(0x80, 6, 0x0200, 0, 255, d);
    assert(n == 64 && ep0_packets == 2 && ep0_in_len == 64);
    n = control(0x80, 6, 0x0200, 0, 64, d);
    assert(n == 64 && ep0_packets == 1);
    n = control(0x80, 6, 0x0302, 0x409, 255, d);
    assert(utf16(d, n, "42"));
    unsigned requests = own_requests;
    assert(control(0xc1, 1, 0, 0, 8, d) == 3 && d[0] == 7 && d[2] == 9 && own_requests == requests+1);
    assert(control(0x41, 2, 0, 0, 0, 0) == 0 && ep0_acked);
    assert(control(0x41, 3, 0, 0, 0, 0) == -1);
    assert(control(0x00, 9, 1, 0, 0, 0) == 0 && own_configured == 1 && own_resets == 2 && usb_configured());
    /* Halting an endpoint, as the host sees it and clears it; endpoint 0 and others aren't endpoint 1's. */
    assert(control(0x02, 3, 0, 0x81, 0, 0) == 0 && usb_halted_in && !usb_halted_out);
    assert(control(0x82, 0, 0, 0x81, 2, d) == 2 && d[0] == 1);
    assert(control(0x82, 0, 0, 0x01, 2, d) == 2 && d[0] == 0);
    usb_halt(USB_EP_OUT, true);
    assert(control(0x82, 0, 0, 0x01, 2, d) == 2 && d[0] == 1);
    assert(control(0x02, 1, 0, 0x81, 0, 0) == 0 && !usb_halted_in && usb_halted_out);
    assert(control(0x02, 1, 0, 0x01, 0, 0) == 0 && !usb_halted_out);
    assert(control(0x02, 3, 0, 0x82, 0, 0) == 0 && !usb_halted_in && !usb_halted_out);
    /* Unconfiguring is a change the class hears of. */
    assert(control(0x00, 9, 0, 0, 0, 0) == 0 && !usb_configured() && own_resets == 3 && own_configured == 1);
    usb_stop();

    puts("PASS: USB0's device side: MIDI mode's descriptors, address, configuration, SysEx in, bus reset; a class's "
         "hooks, serial number, endpoint halts and a full last packet ended empty");
    return 0;
}
