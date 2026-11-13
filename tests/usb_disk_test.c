/* SPDX-License-Identifier: MIT */
/* src/usb_disk.c against a host speaking bulk-only transport and SCSI to it, through a stand-in for src/usb.c's
 * endpoint 1 (packets in and out by DMA, and halts) and for the card. The host side is written from the bulk-only
 * specification: command and status blocks, residues, stalls and their clearing, and reset recovery. */
#define _DEFAULT_SOURCE
#include "../src/usb_disk.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define CARD_SECTORS 64u
#define MAX_PACKETS 512u

static uint8_t card[CARD_SECTORS*512];
static bool card_fails, leave_now;
static unsigned card_fails_after;
static unsigned card_reads, card_writes, leave_asks, leave_after;

bool sd_disk(uint32_t lba, uint8_t *p, bool write) {
    assert(lba < CARD_SECTORS);
    if (card_fails || (card_fails_after && !--card_fails_after))
        return false;
    if (write) {
        memcpy(card+lba*512, p, 512);
        ++card_writes;
    } else {
        memcpy(p, card+lba*512, 512);
        ++card_reads;
    }
    return true;
}

bool usb_disk_leave(void) {
    ++leave_asks;
    return leave_now || (leave_after && leave_asks >= leave_after);
}

bool hal_delay_us(uint32_t usec) {
    (void)usec;
    return true;
}

/* Endpoint 1, as usb.c and the controller present it. */
static uint32_t tx_address, rx_address, tx_count, selected_endpoint;
static bool configured, armed, rx_ready, tx_stuck, host_resets, host_present = true;
static unsigned rx_count;
static const struct usb_class *device;
bool usb_halted_in, usb_halted_out;
static unsigned stalls_in, stalls_out;
/* What the host sends, and what it has received. */
static uint8_t out_packets[MAX_PACKETS][64], in_data[64*MAX_PACKETS];
static unsigned out_sizes[MAX_PACKETS], out_next, out_queued, in_len, in_packets;
static uint8_t ep0_reply[8];
static unsigned ep0_reply_len;

volatile uint32_t *platform_register(uintptr_t address) {
    if (address == USB_EP1_TX_ADDRESS)
        return &tx_address;
    if (address == USB_EP1_RX_ADDRESS)
        return &rx_address;
    assert(address == USB_EP_COUNT(1));
    return &tx_count;
}

/* The controller takes the host's next packet when ready for one, writing 4 bytes past a full one. */
static void deliver(void) {
    if (!armed || rx_ready || usb_halted_out || out_next == out_queued)
        return;
    unsigned n = out_sizes[out_next];
    uint8_t *to = (uint8_t *)(uintptr_t)rx_address;
    memcpy(to, out_packets[out_next++], n);
    if (n == 64)
        memset(to+64, 0xee, 4);
    rx_count = n;
    rx_ready = true;
    armed = false;
}

void usb_select(uint32_t endpoint) {
    selected_endpoint = endpoint;
}

uint32_t usb_read(uint32_t r) {
    assert(selected_endpoint == 1);
    deliver();
    switch (r) {
    case USB_TX_CSR1:
        return tx_stuck ? 1:0;
    case USB_RX_CSR1:
        return rx_ready ? 1:0;
    case USB_RX_CSR2:
        return 0;
    case USB_RX_COUNT1:
        return rx_count&0xff;
    case USB_RX_COUNT2:
        return rx_count >> 8;
    default:
        assert(!"register read");
        return 0;
    }
}

void usb_write(uint32_t r, uint32_t v) {
    assert(selected_endpoint == 1);
    if (r == USB_TX_CSR1) {
        assert(v&1 && !usb_halted_in && tx_count <= 64);
        memcpy(in_data+in_len, (const uint8_t *)(uintptr_t)tx_address, tx_count);
        in_len += tx_count;
        ++in_packets;
        return;
    }
    if (r == USB_RX_CSR1) {
        /* Ready for the next packet: what was received has been taken. Bit 4 does it, as usb_midi.c and m-apps'
         * usbdisk write it on hardware; bit 0 is written back as read. */
        assert(v&0x10);
        rx_ready = false;
        armed = true;
        return;
    }
    assert(r == USB_RX_CSR2);
}

void usb_ep1_reset(const uint8_t *in, uint8_t *out) {
    tx_address = (uint32_t)(uintptr_t)in;
    rx_address = (uint32_t)(uintptr_t)out;
    usb_halted_in = usb_halted_out = false;
    rx_ready = false;
    armed = true;
}

void usb_halt(uint32_t endpoint, bool on) {
    if (endpoint == USB_EP_IN) {
        stalls_in += on;
        usb_halted_in = on;
    } else if (endpoint == USB_EP_OUT) {
        stalls_out += on;
        usb_halted_out = on;
        /* A host that finds the data stage stalled sends no more of it. */
        if (on)
            out_next = out_queued;
    }
}

void usb_ep0_send(const uint8_t *data, uint32_t size, uint32_t asked) {
    ep0_reply_len = size < asked ? size:asked;
    memcpy(ep0_reply, data, ep0_reply_len);
}

/* A request on endpoint 0 as usb.c handles it: the class first, then the standard ones. */
static enum usb_answer host_request(uint8_t type, uint8_t request, uint16_t index, uint16_t length) {
    uint8_t setup[8] = {type, request, 0, 0, (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length,
                        (uint8_t)(length >> 8)};
    enum usb_answer a = device->request(setup);
    if (a == USB_STANDARD && type == 0x02 && request == 0x01)
        usb_halt(index, false);
    return a;
}

/* A host clears a halt it finds, as bulk-only says, unless it's doing reset recovery itself. */
void usb_poll(void) {
    if (host_resets)
        return;
    if (usb_halted_in)
        host_request(0x02, 0x01, USB_EP_IN, 0);
    if (usb_halted_out)
        host_request(0x02, 0x01, USB_EP_OUT, 0);
}

bool usb_configured(void) {
    return configured;
}

void usb_start(const struct usb_class *c) {
    device = c;
    configured = host_present;
    if (configured) {
        c->configured();
        c->reset();
    }
}

void usb_stop(void) {
    configured = false;
}

static void queue(const uint8_t *p, unsigned n) {
    assert(out_queued < MAX_PACKETS && n <= 64);
    memcpy(out_packets[out_queued], p, n);
    out_sizes[out_queued++] = n;
}

static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> 8*i);
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t tag = 0x1000;
static unsigned residue;

/* One command: its block, any data out in full packets, and what came back. Returns the status, or -1 for none. */
static int command(const uint8_t *cdb, unsigned cdb_len, uint32_t expected, bool to_host, const uint8_t *out,
                   unsigned out_len, uint8_t *in, unsigned *got) {
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    put32(cbw+4, ++tag);
    put32(cbw+8, expected);
    cbw[12] = to_host ? 0x80:0;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw+15, cdb, cdb_len);
    out_next = out_queued = in_len = in_packets = 0;
    queue(cbw, 31);
    for (unsigned at = 0; at < out_len; at += 64)
        queue(out+at, out_len-at > 64 ? 64:out_len-at);
    for (unsigned n = 0; n < 4 && in_len < 13; ++n)
        poll();
    if (in_len < 13 || memcmp(in_data+in_len-13, "USBS", 4) || get32(in_data+in_len-9) != tag)
        return -1;
    if (in && got) {
        *got = in_len-13;
        memcpy(in, in_data, *got);
    }
    residue = get32(in_data+in_len-5);
    return in_data[in_len-1];
}

static void sense(uint8_t *key, uint8_t *code) {
    static const uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t reply[64];
    unsigned got;
    assert(command(cdb, 6, 18, true, 0, 0, reply, &got) == 0 && got == 18 && reply[0] == 0x70 && reply[7] == 10);
    *key = reply[2]&15;
    *code = reply[12];
}

static void expect_sense(uint8_t key, uint8_t code) {
    uint8_t k, c;
    sense(&k, &c);
    assert(k == key && c == code);
    /* And it's cleared by being read. */
    sense(&k, &c);
    assert(!k && !c);
}

static void rw10(uint8_t *cdb, bool write, uint32_t lba, unsigned count) {
    memset(cdb, 0, 10);
    cdb[0] = write ? 0x2a:0x28;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
}

static void start(void) {
    total_sectors = CARD_SECTORS;
    sense_key = sense_code = 0;
    ejected = aborted = needs_reset = leaving = false;
    usb_start(&disk);
}

int main(void) {
    void *ram = mmap((void *)(uintptr_t)(USB_DISK_RAM&~0xfffu), 0x2000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(ram == (void *)(uintptr_t)(USB_DISK_RAM&~0xfffu));
    for (unsigned i = 0; i < sizeof(card); ++i)
        card[i] = (uint8_t)(i*13+i/512);
    uint8_t cdb[16], buffer[64*MAX_PACKETS];
    unsigned got;
    start();

    /* The descriptors a host reads first: a bulk-only SCSI interface with endpoint 1 both ways. */
    assert(disk.device[4] == 0 && disk.config[2] == 32 && disk.config[9+5] == 8 && disk.config[9+6] == 6
           && disk.config[9+7] == 0x50 && disk.config[18+2] == 0x81 && disk.config[25+2] == 0x01);
    /* One logical unit: the highest is 0. */
    assert(host_request(0xa1, 0xfe, 0, 1) == USB_SENT && ep0_reply_len == 1 && ep0_reply[0] == 0);

    /* Inquiry: a removable direct-access device, all of it, or what the host asked for with the rest as residue. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = 36;
    assert(command(cdb, 6, 36, true, 0, 0, buffer, &got) == 0 && got == 36 && !residue);
    assert(buffer[0] == 0 && buffer[1] == 0x80 && !memcmp(buffer+8, "M-BOOT  ", 8));
    /* A short packet ends the data, so no stall is needed. */
    assert(command(cdb, 6, 96, true, 0, 0, buffer, &got) == 0 && got == 36 && residue == 60 && !stalls_in);
    assert(command(cdb, 6, 5, true, 0, 0, buffer, &got) == 0 && got == 5 && !residue);
    /* Vital product data isn't offered. */
    cdb[1] = 1;
    assert(command(cdb, 6, 36, true, 0, 0, buffer, &got) == 1 && !got && residue == 36);
    expect_sense(5, 0x24);
    cdb[1] = 0;

    /* Ready, the size, and not write-protected. */
    memset(cdb, 0, sizeof(cdb));
    assert(command(cdb, 6, 0, false, 0, 0, buffer, &got) == 0);
    cdb[0] = 0x25;
    assert(command(cdb, 10, 8, true, 0, 0, buffer, &got) == 0 && got == 8);
    assert(buffer[0] == 0 && buffer[3] == CARD_SECTORS-1 && buffer[6] == 2 && buffer[7] == 0);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1a;
    cdb[4] = 4;
    assert(command(cdb, 6, 4, true, 0, 0, buffer, &got) == 0 && got == 4 && !(buffer[2]&0x80));

    /* Reading: sectors in order, in full packets. */
    rw10(cdb, false, 5, 3);
    unsigned reads = card_reads;
    assert(command(cdb, 10, 3*512, true, 0, 0, buffer, &got) == 0 && got == 3*512 && !residue);
    assert(!memcmp(buffer, card+5*512, 3*512) && card_reads == reads+3 && in_packets == 3*8+1);
    /* Writing: the card has it before the host hears so. */
    uint8_t data[4*512];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(255-i);
    rw10(cdb, true, 60, 4);
    assert(command(cdb, 10, 4*512, false, data, sizeof(data), 0, 0) == 0 && !residue);
    assert(!memcmp(card+60*512, data, sizeof(data)) && out_next == out_queued);
    /* No sectors is nothing to do. */
    rw10(cdb, false, 0, 0);
    assert(command(cdb, 10, 0, true, 0, 0, buffer, &got) == 0 && !got);

    /* Past the end, partly or wholly: refused, nothing read or written, and the data stage stalled. */
    unsigned stalled = stalls_in;
    rw10(cdb, false, CARD_SECTORS-1, 2);
    assert(command(cdb, 10, 2*512, true, 0, 0, buffer, &got) == 1 && !got && residue == 2*512 && stalls_in == stalled+1);
    expect_sense(5, 0x21);
    rw10(cdb, true, CARD_SECTORS, 1);
    stalled = stalls_out;
    unsigned writes = card_writes;
    assert(command(cdb, 10, 512, false, data, 512, 0, 0) == 1 && residue == 512 && stalls_out == stalled+1);
    assert(card_writes == writes);
    expect_sense(5, 0x21);
    /* The length or direction not as the command says. */
    rw10(cdb, false, 0, 1);
    assert(command(cdb, 10, 1024, true, 0, 0, buffer, &got) == 1 && !got);
    expect_sense(5, 0x24);
    assert(command(cdb, 10, 512, false, data, 512, 0, 0) == 1);
    expect_sense(5, 0x24);

    /* The card failing: a read stops where it failed, a write is reported, and neither is taken as done. */
    card_fails = true;
    rw10(cdb, false, 0, 2);
    assert(command(cdb, 10, 1024, true, 0, 0, buffer, &got) == 1 && !got && residue == 1024);
    expect_sense(3, 0x11);
    rw10(cdb, true, 0, 2);
    stalled = stalls_out;
    assert(command(cdb, 10, 1024, false, data, 1024, 0, 0) == 1 && residue == 512 && stalls_out == stalled+1);
    expect_sense(3, 0x0c);
    card_fails = false;
    /* Failing after a sector has gone out in full packets: the host expects more, so the data stage is stalled. */
    card_fails_after = 2;
    stalled = stalls_in;
    rw10(cdb, false, 0, 2);
    assert(command(cdb, 10, 1024, true, 0, 0, buffer, &got) == 1 && got == 512 && residue == 512);
    assert(!memcmp(buffer, card, 512) && stalls_in == stalled+1);
    card_fails_after = 0;
    /* A failure is only the failing command's: the next one succeeds, and then there's nothing to report. */
    card_fails = true;
    rw10(cdb, false, 0, 1);
    assert(command(cdb, 10, 512, true, 0, 0, buffer, &got) == 1);
    card_fails = false;
    memset(cdb, 0, sizeof(cdb));
    assert(command(cdb, 6, 0, false, 0, 0, 0, 0) == 0);
    expect_sense(0, 0);

    /* A short packet in the middle of a sector. */
    rw10(cdb, true, 10, 1);
    stalled = stalls_out;
    assert(command(cdb, 10, 512, false, data, 100, 0, 0) == 1 && stalls_out == stalled+1);
    expect_sense(5, 0x24);

    /* What isn't supported says so, both ways round, and the next command is unaffected. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x23;
    assert(command(cdb, 10, 12, true, 0, 0, buffer, &got) == 1 && !got);
    expect_sense(5, 0x20);
    cdb[0] = 0x55;
    assert(command(cdb, 10, 8, false, data, 8, 0, 0) == 1);
    expect_sense(5, 0x20);
    /* A command with no data that's sent some. */
    cdb[0] = 0x35;
    assert(command(cdb, 10, 8, true, 0, 0, buffer, &got) == 1);
    expect_sense(5, 0x20);
    cdb[0] = 0x35;
    assert(command(cdb, 10, 0, false, 0, 0, 0, 0) == 0);

    /* A bad command block: both halted until a bulk-only reset, which clearing the halts alone doesn't replace. */
    host_resets = true;
    uint8_t bad[31] = {'U', 'S', 'B', 'X'};
    out_next = out_queued = in_len = 0;
    queue(bad, 31);
    poll();
    assert(usb_halted_in && usb_halted_out && !in_len);
    host_request(0x02, 0x01, USB_EP_IN, 0);
    host_request(0x02, 0x01, USB_EP_OUT, 0);
    assert(usb_halted_in && usb_halted_out);
    assert(host_request(0x21, 0xff, 0, 0) == USB_ACK);
    host_request(0x02, 0x01, USB_EP_IN, 0);
    host_request(0x02, 0x01, USB_EP_OUT, 0);
    assert(!usb_halted_in && !usb_halted_out);
    host_resets = false;
    memset(cdb, 0, sizeof(cdb));
    assert(command(cdb, 6, 0, false, 0, 0, 0, 0) == 0);
    /* As is a block of the wrong size, or with a command length out of range. */
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    cbw[14] = 17;
    host_resets = true;
    out_next = out_queued = 0;
    queue(cbw, 31);
    poll();
    assert(usb_halted_in && usb_halted_out && needs_reset);
    host_request(0x21, 0xff, 0, 0);
    out_next = out_queued = 0;
    queue(cbw, 30);
    poll();
    assert(usb_halted_in && usb_halted_out && needs_reset);
    host_request(0x21, 0xff, 0, 0);
    host_resets = false;

    /* Ejecting: not ready afterwards, and the session ends. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1b;
    cdb[4] = 2;
    assert(command(cdb, 6, 0, false, 0, 0, 0, 0) == 0 && ejected);
    cdb[0] = 0;
    cdb[4] = 0;
    assert(command(cdb, 6, 0, false, 0, 0, 0, 0) == 1);
    expect_sense(2, 0x3a);

    /* The whole session: ended by ejecting, by the key between commands, by no host, and by the key in the middle of
     * a transfer that the host stopped reading, which sends no status. */
    out_next = out_queued = in_len = 0;
    memset(cbw, 0, sizeof(cbw));
    memcpy(cbw, "USBC", 4);
    cbw[14] = 6;
    cbw[15] = 0x1b;
    cbw[19] = 2;
    queue(cbw, 31);
    leave_now = false;
    leave_after = leave_asks = 0;
    assert(usb_disk_run(CARD_SECTORS) == USB_DISK_EJECTED && in_len == 13 && !configured);
    out_next = out_queued = 0;
    leave_now = true;
    assert(usb_disk_run(CARD_SECTORS) == USB_DISK_LEFT);
    leave_now = false;
    host_present = false;
    assert(usb_disk_run(CARD_SECTORS) == USB_DISK_NO_HOST && leave_asks >= NO_HOST_CHECKS);
    host_present = true;
    start();
    rw10(cdb, false, 0, 8);
    tx_stuck = true;
    leave_after = leave_asks+1;
    out_next = out_queued = in_len = 0;
    uint8_t read_cbw[31] = {'U', 'S', 'B', 'C'};
    put32(read_cbw+8, 8*512);
    read_cbw[12] = 0x80;
    read_cbw[14] = 10;
    memcpy(read_cbw+15, cdb, 10);
    queue(read_cbw, 31);
    poll();
    assert(leaving && aborted && !in_len);
    tx_stuck = false;

    puts("PASS: USB DISK's bulk-only transport and SCSI against a host: inquiry, size, reads, writes, refusals, card "
         "failures, bad command blocks and reset recovery, ejecting, and leaving mid-transfer");
    return 0;
}
