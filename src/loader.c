/* SPDX-License-Identifier: MIT */
#include "loader.h"
#include <string.h>
static void send_bytes(struct loader_state *s, const void *data, uint32_t n) {
    const uint8_t *p = data;
    while (n) {
        unsigned k = n > 512 ? 512:n;
        (void)s->io.send(p, k);
        p += k;
        n -= k;
    }
}

static void receive_bytes(struct loader_state *s, void *data, uint32_t n) {
    uint8_t *p = data;
    while (n) {
        unsigned k = n > 512 ? 512:n;
        (void)s->io.receive(p, k);
        p += k;
        n -= k;
    }
}

static void drain(struct loader_state *s, uint32_t n) {
    while (n) {
        unsigned k = n > 512 ? 512:n;
        receive_bytes(s, s->buffer[1], k);
        n -= k;
    }
}

void loader_init(struct loader_state *s, const struct loader_services *io) {
    memset(s, 0, sizeof(*s));
    s->io = *io;
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (unsigned j = 0; j < 8; ++j)
            c = (c >> 1) ^ ((c&1) ? 0xedb88320u:0);
        s->crc_table[i] = c;
    }
}

static uint32_t crc32_fast(struct loader_state *s, uint32_t c, const uint8_t *p, unsigned n) {
    while (n--)
        c = (c >> 8) ^ s->crc_table[(c ^ *p++)&255];
    return c;
}

/* A second DMA runs while callbacks send/checksum the completed first buffer. */
static bool stream(struct loader_state *s, uint32_t a, uint32_t n, unsigned mode, uint8_t *answer) {
    unsigned index = 0;
    uint32_t crc = mode == 2 ? UINT32_MAX:0;
    bool async = s->io.read_start && s->io.read_finish;
    unsigned k = n > 4096 ? 4096:n;
    if (k && async && !s->io.read_start(a, s->buffer[index], k))
        return false;
    while (k) {
        uint8_t *p = s->buffer[index];
        bool ok = async ? s->io.read_finish():core_read(&s->io.nor.flash, a, p, k);
        if (!ok)
            return false;
        a += k;
        n -= k;
        unsigned next = n > 4096 ? 4096:n;
        if (next && async && !s->io.read_start(a, s->buffer[index ^ 1], next))
            return false;
        if (mode == 0)
            send_bytes(s, p, k);
        else if (mode == 1)
            crc = core_crc16((uint16_t)crc, p, k);
        else
            crc = crc32_fast(s, crc, p, k);
        index ^= 1;
        k = next;
    }
    if (mode == 1)
        core_put_be16(answer+2, (uint16_t)crc);
    if (mode == 2)
        core_put_be32(answer+2, crc ^ UINT32_MAX);
    return true;
}

static bool known(unsigned op) {
    switch (op) {
    case 0xfb00:
    case 0xfb01:
    case 0xfb02:
    case 0xfb04:
    case 0xfd05:
    case 0xfc09:
    case 0xfc0a:
    case 0xfc0b:
    case 0xfc0e:
    case 0xfc12:
    case 0xfc14:
    case 0xfc40:
    case 0xfc41:
    case 0xfc42:
    case 0xfc43:
    case 0xfc44:
    case 0xfc45:
    case 0xfc46:
        return true;
    default:
        return false;
    }
}

int loader_dispatch(struct loader_state *s, const uint8_t cbw[31], uint8_t ans[16]) {
    const uint8_t *c = cbw+15;
    unsigned op = core_be16(c);
    if (!known(op))
        return 0;
    /* FB06, FD07 and FB08 remain ROM-owned. */
    uint32_t count = core_le32(cbw+8), a = core_be32(c+2), n = core_be16(c+6);
    bool in = (cbw[12]&128) != 0;
    memset(ans, 0, 16);
    ans[0] = c[0];
    ans[1] = c[1];
    if (op == 0xfb02 || op == 0xfc12) {
        if (!in)
            drain(s, count);
        s->error = 5;
        if (in)
            send_bytes(s, ans, count < 16 ? count:16);
        return 1;
    }
    bool out = op == 0xfb04 || op == 0xfc44;
    if (cbw[14] != 16 || (count && in == out)) {
        if (!in)
            drain(s, count);
        s->error = 7;
        if (in)
            send_bytes(s, ans, count < 16 ? count:16);
        return 1;
    }
    if (op == 0xfb04 || op == 0xfc44) {
        uint32_t declared = op == 0xfb04 ? n:core_be16(c+7);
        if (count != declared) {
            drain(s, count);
            s->error = 7;
            return 1;
        }
        if (op == 0xfb04 && n > 4096) {
            drain(s, count);
            s->error = 1;
            return 1;
        }
        if (op == 0xfc44) {
            /* Keep complete OUT framing even when text cannot be drawn. */
            if (count > 20 || c[2] > 14) {
                drain(s, count);
                s->error = 6;
                return 1;
            }
            receive_bytes(s, s->buffer[0], count);
            if (s->screen && s->io.screen_row)
            s->io.screen_row(c[2], core_be16(c+3), core_be16(c+5), (char *)s->buffer[0], count);
            return 1;
        }
        receive_bytes(s, s->buffer[0], n);
        if (core_crc16(0, s->buffer[0], n) != core_le16(c+9)) {
            s->error = 2;
            return 1;
        }
        if (!core_range(a, n, s->io.nor.flash.capacity)) {
            s->error = 3;
            return 1;
        }
        /* Verify whole payload first, then split pages independently of HAL. */
        uint32_t done = 0;
        while (done < n) {
            uint32_t k = 256-((a+done)&255);
            if (k > n-done)
                k = n-done;
            if (!s->io.nor.program(s->io.nor.flash.ctx, a+done, s->buffer[0]+done, k)) {
                s->error = 4;
                break;
            }
            done += k;
        }
        return 1;
    }
    if (op == 0xfd05 && count != n) {
        s->error = 7;
        send_bytes(s, ans, count < 16 ? count:16);
        return 1;
    }
    if (op != 0xfd05 && count > 16) {
        s->error = 7;
        send_bytes(s, ans, 16);
        return 1;
    }
    switch (op) {
    case 0xfb00:
    case 0xfb01:
        {
            uint32_t size = op == 0xfb00 ? 65536:4096;
            if ((a&(size-1)) || !core_range(a, size, s->io.nor.flash.capacity))
                s->error = 3;
            else if (!s->io.nor.erase(s->io.nor.flash.ctx, a, size))
                s->error = 4;
            break;
        }
    case 0xfd05:
        if (!core_range(a, n, s->io.nor.flash.capacity)) {
            s->error = 3;
            break;
        }
        if (!stream(s, a, n, 0, ans))
            s->error = 4;
        return 1;
    case 0xfc09:
        {
            uint8_t k[2];
            core_put_le16(k, s->io.key());
            uint32_t i = 0;
            core_usb_stream(core_usb_begin(UINT32_MAX), &i, k, 2);
            ans[7] = k[0];
            ans[6] = k[1];
            break;
        }
    case 0xfc0a:
        ans[2] = 3;
        core_put_le32(ans+4, s->io.jedec());
        break;
    case 0xfc0b:
        {
            uint32_t id = s->io.jedec();
            ans[2] = id >> 16;
            ans[3] = id >> 8;
            ans[4] = id;
            break;
        }
    case 0xfc0e:
    case 0xfc41:
        if (op == 0xfc41)
            n = core_be32(c+6);
        if (!core_range(a, n, s->io.nor.flash.capacity))
            s->error = 3;
        else if (!stream(s, a, n, op == 0xfc41 ? 2:1, ans))
            s->error = 4;
        break;
    case 0xfc14:
        core_put_be32(ans+2, 4096);
        break;
    case 0xfc40:
        memcpy(ans+2, "MFLR" LOADER_VERSION, 7);
        core_put_be32(ans+10, s->io.entry);
        ans[14] = 15;
        ans[15] = s->io.cpu_mhz;
        break;
    case 0xfc42:
        s->io.uid(ans+2);
        break;
    case 0xfc43:
        s->screen = 0;
        if ((c[2] == 1 || c[2] == 2) && s->io.screen_init)
            s->screen = s->io.screen_init(c[2]);
        ans[2] = s->screen;
        break;
    case 0xfc45:
        if (s->screen && s->io.screen_progress)
            s->io.screen_progress(c[2] > 100 ? 100:c[2], core_be16(c+3));
        break;
    case 0xfc46:
        ans[2] = s->error;
        s->error = 0;
        break;
    }
    send_bytes(s, ans, count < 16 ? count:16);
    return 1;
}
