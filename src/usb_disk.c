/* SPDX-License-Identifier: MIT */
/* Bulk-only mass storage on usb.c, serving the card through sdboot.c one sector at a time. From m-apps' usbdisk,
 * without its streaming, which the bootloader has no room for. */
#include "usb_disk.h"
#include "platform.h"
#include "sdboot.h"
#include "usb.h"
#define SECTOR 512u
#define TICK_US 100u
#define LEAVE_EVERY_TICKS 1000u                 /* about 0.1 s when idle */
#define NO_HOST_CHECKS 1200u                    /* about 2 minutes */
#define AFTER_EJECT_US 300000u

static const uint8_t device_descriptor[18] = {
    18, 1, 0x10, 0x01, 0, 0, 0, USB_PACKET, 0x09, 0x12, 0x03, 0x00, 0x00, 0x01, 0, 1, 2, 1
};
static const uint8_t config_descriptor[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,                  /* mass storage, SCSI, bulk-only */
    7, 5, USB_EP_IN, 2, USB_PACKET, 0, 0,
    7, 5, USB_EP_OUT, 2, USB_PACKET, 0, 0
};
static const uint8_t inquiry[36] = {
    0, 0x80, 0x02, 0x02, 31, 0, 0, 0,
    'M', '-', 'B', 'O', 'O', 'T', ' ', ' ',
#ifdef BOARD_SMK
    'S', 'M', 'K', '-', '3', '7', ' ', 'U', 'S', 'B', ' ', 'D', 'I', 'S', 'K', ' ',
#else
    'F', 'M', '-', '1', ' ', 'U', 'S', 'B', ' ', 'D', 'I', 'S', 'K', ' ', ' ', ' ',
#endif
    '1', '.', '0', ' '
};

#define B ((struct usb_disk_buffers *)(uintptr_t)USB_DISK_RAM)
static uint32_t total_sectors, tx_status;
static uint8_t sense_key, sense_code;
static bool ejected, aborted, needs_reset, leaving;

static void endpoints_reset(void) {
    usb_ep1_reset(B->reply, B->command);
}

static void changed(void) {
    aborted = true;
    needs_reset = false;
}

static enum usb_answer request(const uint8_t *setup) {
    static const uint8_t zero;
    switch ((uint32_t)setup[0] << 8 | setup[1]) {
    case 0xa1fe:                                /* the highest logical unit */
        usb_ep0_send(&zero, 1, setup[6] | (uint32_t)setup[7] << 8);
        return USB_SENT;
    case 0x21ff:                                /* bulk-only reset */
        changed();
        endpoints_reset();
        return USB_ACK;
    case 0x0201:                                /* after a bad command block, the halts stay until that reset */
        return needs_reset ? USB_ACK:USB_STANDARD;
    default:
        return USB_STANDARD;
    }
}

static const struct usb_class disk = {
    device_descriptor, config_descriptor,
#ifdef BOARD_SMK
    "SMK-37 USB DISK",
#else
    "FM-1 USB DISK",
#endif
    "000000000001", endpoints_reset, changed, request
};

/* Between packets: the bus, and whether to give up the transfer. */
static bool still_on(uint32_t n) {
    if ((n&15) == 15)
        usb_poll();
    if ((n&0xfff) == 0xfff && usb_disk_leave())
        leaving = aborted = true;
    return !aborted && usb_configured();
}

/* Ready for the next packet, which the controller's DMA will put here. */
static void rx_arm(uint8_t *to) {
    REG(USB_EP1_RX_ADDRESS) = DMA_ADDRESS(to);
    usb_select(1);
    uint32_t csr = usb_read(USB_RX_CSR1) | usb_read(USB_RX_CSR2) << 8;
    csr = (csr&~0x164u) | 0x10u;
    usb_write(USB_RX_CSR1, csr&0xff);
    usb_write(USB_RX_CSR2, csr >> 8);
}

/* Waits for the IN endpoint to be free. False if the transfer was given up, by a reset or a lost host. */
static bool tx_wait(void) {
    /* Sending would clear a stall the host hasn't. */
    if (usb_halted_in)
        return false;
    for (uint32_t n = 0; still_on(n); ++n) {
        usb_select(1);
        uint32_t csr = usb_read(USB_TX_CSR1);
        if (!(csr&0x01)) {
            if (csr&0x80)
                usb_write(USB_TX_CSR1, csr&~0x80u);
            tx_status = csr&~0x80u;
            return true;
        }
    }
    return false;
}

static bool tx_send(const uint8_t *data, uint32_t n) {
    if (!tx_wait())
        return false;
    sync_cpu();
    REG(USB_EP1_TX_ADDRESS) = DMA_ADDRESS(data);
    REG(USB_EP_COUNT(1)) = n;
    usb_write(USB_TX_CSR1, tx_status | 0x01);
    return true;
}

/* Waits for a packet, and returns its size, or -1 if the transfer was given up. */
static int rx_wait(void) {
    for (uint32_t n = 0; still_on(n); ++n) {
        usb_select(1);
        if (usb_read(USB_RX_CSR1)&1) {
            uint32_t size = usb_read(USB_RX_COUNT1) | usb_read(USB_RX_COUNT2) << 8;
            sync_dma();
            return (int)(size > USB_PACKET ? USB_PACKET:size);
        }
    }
    return -1;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One command's state. */
static uint32_t expected, moved;
static bool to_host;

static void fail(uint8_t key, uint8_t code) {
    sense_key = key;
    sense_code = code;
}

/* The end of the data: if the host expected more than a short packet tells it, it gets a stall. */
static void finish_in(uint32_t last_packet) {
    if (moved < expected && (!moved || last_packet == USB_PACKET))
        usb_halt(USB_EP_IN, true);
}

/* No data, for a refused command: a stall in the direction the host expected it. */
static void refuse(void) {
    if (to_host) {
        moved = 0;
        finish_in(0);
    } else if (expected)
        usb_halt(USB_EP_OUT, true);
}

static void send_reply(const uint8_t *data, uint32_t size) {
    if (size > expected)
        size = expected;
    uint32_t sent = 0, last = 0;
    while (sent < size) {
        last = size-sent > USB_PACKET ? USB_PACKET:size-sent;
        if (!tx_send(data+sent, last))
            return;
        sent += last;
    }
    moved = sent;
    finish_in(last);
}

static void read_sectors(uint32_t first, uint32_t count) {
    for (uint32_t s = 0; s < count; ++s) {
        /* The last packet may still be going out of this buffer. */
        if (!tx_wait())
            return;
        if (!sd_disk(first+s, B->sector, false)) {
            fail(3, 0x11);
            break;
        }
        for (uint32_t at = 0; at < SECTOR; at += USB_PACKET)
            if (!tx_send(B->sector+at, USB_PACKET))
                return;
        moved += SECTOR;
    }
    finish_in(USB_PACKET);
}

/* A sector from the host. False, having dealt with it, if it won't come. */
static bool receive_sector(void) {
    for (uint32_t at = 0; at < SECTOR; ) {
        rx_arm(B->sector+at);
        int got = rx_wait();
        if (got < 0)
            return false;
        at += (uint32_t)got;
        moved += (uint32_t)got;
        if ((uint32_t)got < USB_PACKET) {
            fail(5, 0x24);                      /* a short packet inside a sector */
            usb_halt(USB_EP_OUT, true);
            return false;
        }
    }
    return true;
}

static void write_sectors(uint32_t first, uint32_t count) {
    for (uint32_t s = 0; s < count; ++s) {
        if (!receive_sector())
            return;
        if (!sd_disk(first+s, B->sector, true)) {
            fail(3, 0x0c);
            /* With all the data in, the host has nothing to send, so it wouldn't find a stall and clear it. After a
             * reset, the command is gone, and a stall would only get in the next one's way. */
            if (moved < expected && !aborted)
                usb_halt(USB_EP_OUT, true);
            return;
        }
    }
}

static void no_data_expected(void) {
    if (expected) {
        fail(5, 0x20);
        if (to_host)
            usb_halt(USB_EP_IN, true);
        else
            usb_halt(USB_EP_OUT, true);
    }
}

static void command_run(const uint8_t *c) {
    uint8_t asked_key = sense_key, asked_code = sense_code, *reply = B->reply;
    if (c[0] != 0x03)
        sense_key = sense_code = 0;
    for (unsigned i = 0; i < 18; ++i)
        reply[i] = 0;
    switch (c[0]) {
    case 0x00:                                  /* test unit ready */
        if (ejected)
            fail(2, 0x3a);
        no_data_expected();
        break;
    case 0x03:                                  /* request sense */
        reply[0] = 0x70;
        reply[2] = asked_key;
        reply[7] = 10;
        reply[12] = asked_code;
        sense_key = sense_code = 0;
        send_reply(reply, 18);
        break;
    case 0x12:                                  /* inquiry */
        if (c[1]&1) {
            fail(5, 0x24);
            refuse();
            break;
        }
        send_reply(inquiry, sizeof(inquiry));
        break;
    case 0x1a:                                  /* mode sense (6): not write-protected */
        reply[0] = 3;
        send_reply(reply, 4);
        break;
    case 0x1b:                                  /* start stop unit */
        if ((c[4]&3) == 2)
            ejected = true;
        no_data_expected();
        break;
    case 0x1e:                                  /* prevent or allow removal: we don't hold a disk in */
    case 0x2f:                                  /* verify */
    case 0x35:                                  /* synchronize cache: there's no cache here */
        no_data_expected();
        break;
    case 0x25:                                  /* read capacity (10) */
        if (ejected) {
            fail(2, 0x3a);
            refuse();
            break;
        }
        put_be32(reply, total_sectors-1);
        put_be32(reply+4, SECTOR);
        send_reply(reply, 8);
        break;
    case 0x28:                                  /* read (10) */
    case 0x2a: {                                /* write (10) */
        uint32_t first = be32(c+2), count = (uint32_t)c[7] << 8 | c[8];
        bool write = c[0] == 0x2a;
        if (ejected)
            fail(2, 0x3a);
        else if (first > total_sectors || count > total_sectors-first)
            fail(5, 0x21);
        else if (expected != count*SECTOR || to_host == write)
            fail(5, 0x24);
        if (sense_key) {
            refuse();
            break;
        }
        if (write)
            write_sectors(first, count);
        else
            read_sectors(first, count);
        break;
    }
    default:
        fail(5, 0x20);
        refuse();
        break;
    }
}

static void command_handle(void) {
    uint8_t c[16], *block = B->command;
    uint32_t tag = le32(block+4);
    expected = le32(block+8);
    to_host = (block[12]&0x80) != 0;
    moved = 0;
    for (unsigned i = 0; i < sizeof(c); ++i)
        c[i] = block[15+i];
    /* The next packet after this one is either the data, or this endpoint's next command. */
    if (to_host || !expected)
        rx_arm(block);
    aborted = false;
    command_run(c);
    /* Waits out a stall the host has to clear. */
    for (uint32_t n = 0; (usb_halted_in || usb_halted_out) && still_on(n); ++n)
        ;
    if (aborted || !usb_configured())
        return;
    if (!to_host && expected)
        rx_arm(block);
    put_le32(B->status, 0x53425355);
    put_le32(B->status+4, tag);
    put_le32(B->status+8, expected-moved);
    B->status[12] = sense_key ? 1:0;
    tx_send(B->status, 13);
}

static void poll(void) {
    usb_poll();
    if (!usb_configured() || needs_reset)
        return;
    usb_select(1);
    if (!(usb_read(USB_RX_CSR1)&1))
        return;
    uint32_t size = usb_read(USB_RX_COUNT1) | usb_read(USB_RX_COUNT2) << 8;
    sync_dma();
    uint8_t *block = B->command;
    if (size != 31 || le32(block) != 0x43425355 || (block[13]&15) || !block[14] || block[14] > 16) {
        /* A bad command block: bulk-only says stall both until the host resets the device. */
        needs_reset = true;
        rx_arm(block);
        usb_halt(USB_EP_IN, true);
        usb_halt(USB_EP_OUT, true);
        return;
    }
    command_handle();
}

enum usb_disk_end usb_disk_run(uint32_t sectors) {
    total_sectors = sectors;
    sense_key = sense_code = 0;
    ejected = aborted = needs_reset = leaving = false;
    usb_start(&disk);
    bool was_configured = false;
    for (unsigned ticks = 0, checks = 0; !ejected && !leaving; ) {
        poll();
        hal_delay_us(TICK_US);
        if (++ticks < LEAVE_EVERY_TICKS)
            continue;
        ticks = 0;
        was_configured |= usb_configured();
        if (usb_disk_leave())
            leaving = true;
        else if (!was_configured && ++checks >= NO_HOST_CHECKS)
            break;
    }
    /* Let the host hear the end of its last command. */
    for (unsigned n = 0; n < AFTER_EJECT_US/TICK_US; ++n) {
        usb_poll();
        hal_delay_us(TICK_US);
    }
    usb_stop();
    return ejected ? USB_DISK_EJECTED:leaving ? USB_DISK_LEFT:USB_DISK_NO_HOST;
}
