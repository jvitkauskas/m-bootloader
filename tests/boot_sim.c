/* SPDX-License-Identifier: MIT */
#include "boot_sim.h"
#include "core.h"
#include <assert.h>
/* Power, clocks, timer */
#define PWR_CON 0x10000u
#define CLK_DIV 0x10008u
#define CLK_CON0 0x1000Cu
#define CLK_CON2 0x10014u
#define CHIP_ID 0x10200u
#define TMR0 0x10400u /* +0 control, +4 count, +8 period, +0xC pwm */
#define PLL1_A 0x119A0u
#define PLL1_B 0x119A4u
#define PLL2_A 0x119A8u
#define PLL2_B 0x119ACu
/* SPI blocks: +0 control, +4 baud, +8 byte buffer, +0xC DMA address, +0x10 count */
#define SPI0 0x11C00u
#define SPI1 0x11D00u
#define SPI_CON 0x00u
#define SPI_BAUD 0x04u
#define SPI_BUF 0x08u
#define SPI_ADR 0x0Cu
#define SPI_CNT 0x10u
#define SPI_EN (1u << 0)
#define SPI_FULL (1u << 3) /* separate TX/RX lines */
#define SPI_RX (1u << 12) /* direction: 1 = receive */
#define SPI_CLR (1u << 14) /* write 1: clear completion */
#define SPI_DONE (1u << 15) /* completion */
#define SPI_W(w) ((u32)(w) << 10) /* DMA / byte width: 0 single, 1 dual, 2 quad */
/* Peripheral ENC (on the SPI0 DMA path) */
#define ENC_CON 0x13000u
#define ENC_KEY 0x13004u
#define ENC_ADR 0x13008u
/* P33 serial bridge */
#define P33_PMU 0x13E00u
#define P33_RTC 0x13E04u
#define P33_CON 0x13E08u
#define P33_DAT 0x13E0Cu
/* High-speed USB PHY control, shared with the SMK-37 key LED commons */
#define PHY_CON 0x16A0Cu
/* SFC (flash controller for XIP) and its ENC */
#define SFC_CON 0x40200u
#define SFC_BAUD 0x40204u
#define SFC_CODE 0x40208u
#define SFC_BASE 0x4020Cu
#define XENC_CON 0x40300u
#define XENC_KEY 0x40304u
/* GPIO ports A..H = 0..7 and pin mux */
#define SIM_GPIO(p) (0x50000u + 0x40u * (p))
#define GP_OUT 0x00u
#define GP_IN 0x04u
#define GP_DIR 0x08u /* 1 = input */
#define GP_DIE 0x0Cu /* digital input enable */
#define GP_PU 0x10u
#define GP_PD 0x14u
#define GP_HD0 0x18u
#define GP_HD 0x1Cu
#define GP_DIEH 0x20u
#define PA 0
#define PB 1
#define PC 2
#define PD 3
#define PE 4
#define PF 5
#define PG 6
#define PH 7
#define MUX0 0x5101Cu
#define MUX1 0x51020u
/* Cache */
#define CACHE_CON 0x01EEE008u
/* P33 register addresses (not CPU addresses) */
#define P33_WDOG 0x80u
#define P33_VDDIO 0x05u
#define P33_DCDC14 0x06u
#define P33_DVDD 0x09u
struct sim sim;
/* ---------------------------------------------------------- generic regs */
#define NREG (0x60000 / 4)
static u32 reg[NREG];
static u32 cache_mem[0xB200 / 4];
static uintptr_t pending_addr;
static u32 pending_before;
static int pending_kind;
static int spi_reply, p33_reply;
static int p33_data_pending;
static struct {
    u32 a, v;
} extra[256];
static u32 nextra;
static u32 *slot(u32 a)
{
    u32 i;
    if (a < 0x60000)
        return &reg[a >> 2];
    if (a >= 0x01F00000 && a < 0x01F0B200)
        return &cache_mem[(a - 0x01F00000) >> 2];
    for (i = 0; i < nextra; i++)
        if (extra[i].a == a)
            return &extra[i].v;
    if (nextra < 256) {
        extra[nextra].a = a;
        extra[nextra].v = 0;
        return &extra[nextra++].v;
    }
    return &extra[255].v;
}

void sim_fail(const char *msg)
{
    if (!sim.err)
        snprintf(sim.errmsg, sizeof(sim.errmsg), "%s", msg);
    sim.err++;
}

/* ------------------------------------------------------ DMA address map */
#define NDMA 4096
static struct {
    const void *p;
} dma_tab[NDMA];
static u32 dma_next;
u32 platform_dma_address(const void *p)
{
    u32 i;
    for (i = 0; i < NDMA; i++)
        if (dma_tab[i].p == p)
            return 0x10000000u + i * 0x10000u;
    dma_tab[dma_next % NDMA].p = p;
    i = dma_next++ % NDMA;
    return 0x10000000u + i * 0x10000u;
}

static u8 *dma_ptr(u32 a)
{
    u32 i = (a - 0x10000000u) / 0x10000u;
    if (a < 0x10000000u || i >= NDMA || !dma_tab[i].p) {
        sim_fail("DMA to an unknown address");
        return NULL;
    }
    return (u8 *)dma_tab[i].p + (a & 0xFFFF);
}

/* --------------------------------------------------------------- P33 */
static u8 p33[1024];
static int p33_asserted, p33_n;
static u32 p33_op, p33_addr;
u8 sim_p33_get(u32 r) {
    return p33[r & 1023];
}

void sim_p33_set(u32 r, u8 v) {
    p33[r & 1023] = v;
}

static u32 p33_exec(u32 op, u32 a, u32 d)
{
    u32 v = p33[a];
    sim.p33_writes += op != 0x80;
    switch (op) {
    case 0x00:
        p33[a] = (u8)d;
        break;
    case 0x20:
        p33[a] |= (u8)d;
        break;
    case 0x40:
        p33[a] &= (u8)d;
        break;
    case 0x80:
        if (d != 0x5E)
            sim_fail("P33 read without dummy 0x5E");
        return p33[a];
    default:
        sim_fail("P33 unknown operation");
    }
    if (a == 0xB1 && (d & 2)) {
        /* efuse byte strobe */
        u32 bank = p33[0xB0] >> 6, row = d >> 6, j = (d >> 2) & 15;
        u32 w = sim.chip_id & 0xFF ? 4 : 3;
        if (j >= w)
        p33[0xB2] = 0;
        else
        p33[0xB2] = (u8)(sim.fuse[bank & 3][row & 7] >> (8 * j));
    }
    return v;
}

static void p33_bridge_write(u32 val)
{
    u32 *c = slot(P33_CON);
    u32 old = *c;
    if (!(old & 1) && (val & 1)) {
        p33_n = 0;
        p33_asserted = 1;
    }
    if ((old & 1) && !(val & 1)) {
        if (p33_n != 3)
            sim_fail("P33 transaction without exactly three bytes");
        p33_asserted = 0;
    }
    if ((val & 0x10) && !(old & 0x10)) {
        u32 d = *slot(P33_DAT) & 0xFF;
        if (!p33_asserted)
            sim_fail("P33 byte outside an asserted transaction");
        if ((val & 0xC) != 0xC)
            sim_fail("P33 byte without bits 3:2 = 11");
        if (p33_n == 0) {
            p33_op = d & 0xE0;
            p33_addr = (d & 3) << 8;
        } else if (p33_n == 1) {
            p33_addr |= d;
        } else if (p33_n == 2) {
            *slot(P33_DAT) = p33_exec(p33_op, p33_addr, d);
        }
        p33_n++;
        val &= ~0x12u;
        /* start is self-clearing, busy bit 1 stays clear */
    }
    *c = val;
}

/* ------------------------------------------------------------ SPI0 + NOR */
struct chip {
    int cs;
    /* selected */
    u32 n;
    /* bytes since select */
    u8 cmd;
    u32 addr;
    u32 dummy;
    /* dummy clock cycles done */
    u32 rd;
    /* running read address */
    u8 pbuf[512];
    u32 pn;
    u32 pbase;
    int wel;
    u32 did;
    /* data bytes returned in a read */
} ch;
static int protected_(void) {
    return (sim.sr1 & 0x7C) || (sim.sr2 & 0x40);
}

static void chip_select(int sel)
{
    if (sel && !ch.cs) {
        ch.n = 0;
        ch.pn = 0;
        ch.addr = 0;
        ch.dummy = 0;
        ch.did = 0;
    } else if (!sel && ch.cs) {
        /* rising edge: execute */
        switch (ch.cmd) {
        case 0x06:
            ch.wel = 1;
            break;
        case 0x01:
            sim.wrsr_cnt++;
            if (ch.wel && ch.pn >= 1 && !sim.ignore_wrsr) {
                sim.sr1 = (u8)((sim.sr1 & 0x03) | (ch.pbuf[0] & 0xFC));
                if (ch.pn >= 2) {
                    if (sim.combined_wrsr)
                    sim.sr2 = ch.pbuf[1];
                }
                sim.busy_left = sim.busy_polls;
            }
            ch.wel = 0;
            break;
        case 0x31:
            if (ch.wel && ch.pn >= 1 && !sim.ignore_wrsr) {
                sim.sr2 = ch.pbuf[0];
                sim.busy_left = sim.busy_polls;
            }
            ch.wel = 0;
            break;
        case 0x02:
            if (ch.wel && !protected_() && ch.n >= 4) {
                u32 i, base = ch.addr & ~255u;
                if (ch.pn > 256)
                sim_fail("page program with more than 256 bytes");
                for (i = 0; i < ch.pn; i++) {
                    u32 a = base + ((ch.addr + i) & 255);
                    if (a < nor_size && nor_page(a, &ch.pbuf[i], 1))
                    break;
                }
                sim.busy_left = sim.busy_polls;
            }
            ch.wel = 0;
            break;
        case 0x20:
        case 0xD8:
            if (ch.wel && !protected_() && ch.n >= 4) {
                u32 sz = ch.cmd == 0x20 ? 0x1000 : 0x10000;
                u32 a = ch.addr & ~(sz - 1);
                if (a + sz <= nor_size)
                nor_erase(a, sz);
                sim.busy_left = sim.busy_polls;
            }
            ch.wel = 0;
            break;
        default:
            break;
        }
    }
    ch.cs = sel;
}

/* One byte on the SPI pins. `rx`: the chip drives the byte; `w`: 0/1/2 width. */
static u8 chip_byte(u8 tx, int rx, int w)
{
    u32 cyc = 8 >> w;
    u8 out = 0xFF;
    if (!ch.cs) {
        sim_fail("SPI byte while chip select is high");
        return 0xFF;
    }
    if (ch.n == 0) {
        if (rx || w)
        sim_fail("opcode must be sent single-bit");
        ch.cmd = tx;
        ch.n = 1;
        return 0xFF;
    }
    switch (ch.cmd) {
    case 0x9F:
        out = (u8)(sim.jedec >> (16 - 8 * (ch.n - 1)));
        if (!rx)
            sim_fail("JEDEC read driven by the host");
        break;
    case 0x05:
        out = sim.sr1 & ~1u;
        if (sim.busy_left) {
            out |= 1;
            sim.status_polls++;
            sim.busy_left--;
        }
        if (!rx)
            sim_fail("status read driven by the host");
        break;
    case 0x35:
        out = sim.sr2;
        break;
    case 0x4B:
        if (ch.n <= 4) {
            if (rx)
            sim_fail("UID dummy must be sent");
        } else {
            out = sim.uid[(ch.n - 5) & 7];
        }
        break;
    case 0x01:
    case 0x31:
    case 0x02:
        if (ch.cmd == 0x02 && ch.n <= 3) {
            ch.addr = (ch.addr << 8) | tx;
        } else if (ch.pn < sizeof(ch.pbuf)) {
            ch.pbuf[ch.pn++] = tx;
        }
        if (rx)
            sim_fail("write command with a receive byte");
        break;
    case 0x20:
    case 0xD8:
        if (ch.n <= 3)
            ch.addr = (ch.addr << 8) | tx;
        break;
    case 0x03:
    case 0x0B:
    case 0x3B:
    case 0x6B:
        if (ch.n <= 3) {
            if (rx || w)
            sim_fail("read address must be sent single-bit");
            ch.addr = (ch.addr << 8) | tx;
            if (ch.n == 3)
            ch.rd = ch.addr;
        } else {
            u32 need = ch.cmd == 0x03 ? 0 : 8;
            if (!rx)
                sim_fail("read data phase driven by the host");
            if (ch.cmd == 0x6B && w != 2 && ch.dummy < need)
                sim_fail("6B dummy/data not in quad width");
            if (ch.cmd == 0x3B && w != 1 && ch.dummy < need)
                sim_fail("3B dummy/data not in dual width");
            if (ch.cmd == 0x03 && w != 0)
                sim_fail("03 data must be single-bit");
            if (ch.dummy < need) {
                ch.dummy += cyc;
                if (ch.dummy > need)
                    sim_fail("dummy clocks overshoot");
                ch.n++;
                return 0xFF;
            }
            out = ch.rd < nor_size ? sim_nor[ch.rd] : 0xFF;
            if ((ch.cmd == 0x6B && sim.bad_quad) || (ch.cmd == 0x3B && sim.bad_dual))
                out = (u8)~out;
            ch.rd++;
            ch.did++;
        }
        break;
    default:
        break;
    }
    ch.n++;
    return out;
}

/* SPI0 registers */
static void spi0_wr(u32 off, u32 val)
{
    u32 *con = slot(SPI0 + SPI_CON);
    u32 w = (*con >> 10) & 3;
    int rx = (*con >> 12) & 1;
    if (off == SPI_CON) {
        *con = (val & ~0x8000u) | (*con & 0x8000u);
        if (val & SPI_CLR)
        *con &= ~0x8000u;
        return;
    }
    if (off == SPI_BUF) {
        if (!(*con & 1))
        sim_fail("SPI0 byte with the block disabled");
        if (rx)
        *slot(SPI0 + SPI_BUF) = chip_byte(0xFF, 1, (int)w), sim.byte_rx++;
        else
        (void)chip_byte((u8)val, 0, (int)w), sim.byte_tx++;
        if (w)
        sim_fail("multi-bit phase sent through byte register instead of DMA");
        *con |= 0x8000u;
        return;
    }
    if (off == SPI_CNT) {
        u32 n = val & 0xFFFF, i;
        u8 *p = dma_ptr(*slot(SPI0 + SPI_ADR));
        if (!n) {
            sim.dma_zero++;
            sim_fail("zero-length DMA");
            return;
        }
        if (!(*con & 1))
        sim_fail("SPI0 DMA with the block disabled");
        if (*slot(ENC_CON) != 2)
        sim_fail("SPI0 DMA without ENC pass-through (control 2)");
        if (!rx)
        sim_fail("SPI0 DMA in the transmit direction is not modelled");
        if (w && (*con & SPI_FULL))
        sim_fail("multi-bit DMA with bit 3 set");
        if (ch.n == 4 && !ch.dummy && (ch.cmd == 0x6B || ch.cmd == 0x3B)) {
            u32 expected = ch.cmd == 0x6B ? 4 : 2;
            if (n != expected)
                sim_fail("wide dummy must be one exact-length DMA");
        }
        if (p)
        for (i = 0; i < n; i++)
        p[i] = chip_byte(0xFF, 1, (int)w);
        if (ch.cmd == 0x6B && ch.did == 0)
            sim.dma_dummy_quad++;
        if (ch.cmd == 0x3B && ch.did == 0)
            sim.dma_dummy_dual++;
        sim.dma_reads++;
        sim.dma_bytes += n;
        *con |= 0x8000u;
        return;
    }
    *slot(SPI0 + off) = val;
}

/* ------------------------------------------------------------- SPI1 panel */
static struct {
    u32 cmd;
    u32 nargs;
    u8 args[16];
    u32 wx0, wx1, wy0, wy1, cx, cy;
    int mem;
} lcd;
static int port_pin(u32 port, u32 pin) {
    return (*slot(SIM_GPIO(port) + GP_OUT) >> pin) & 1;
}

static void lcd_byte(u8 b)
{
    /* which board? FM-1: CS PC7, DC PC8; SMK: CS PC8, DC PB5 */
    int fm1 = (*slot(MUX1) & 0x10) != 0;
    int cs = fm1 ? port_pin(PC, 7) : port_pin(PC, 8);
    int dc = fm1 ? port_pin(PC, 8) : port_pin(PB, 5);
    if (cs) {
        sim_fail("panel byte with chip select high");
        return;
    }
    if (!dc) {
        lcd.cmd = b;
        lcd.nargs = 0;
        lcd.mem = b == 0x2C;
        if (b == 0x2C) {
            lcd.cx = lcd.wx0;
            lcd.cy = lcd.wy0;
        }
        if (sim.ncmd < 512) {
            sim.cmds[sim.ncmd] = b;
            sim.cmd_nargs[sim.ncmd] = 0;
            sim.ncmd++;
        }
        if (b == 0x29)
            sim.panel_on = 1;
        return;
    }
    if (lcd.mem) {
        static int half;
        static u8 hi;
        if (!half) {
            hi = b;
            half = 1;
        } else {
            if (lcd.cy <= lcd.wy1 && lcd.cx <= lcd.wx1 && lcd.cx < 240 && lcd.cy < 240) {
                sim.fb[lcd.cy * 240 + lcd.cx] = (u16)((hi << 8) | b);
                sim.fb_pix++;
            } else {
                sim_fail("pixel outside the window");
            }
            if (++lcd.cx > lcd.wx1) {
                lcd.cx = lcd.wx0;
                lcd.cy++;
            }
            half = 0;
        }
        return;
    }
    if (lcd.nargs < 16)
        lcd.args[lcd.nargs] = b;
    lcd.nargs++;
    if (sim.ncmd && sim.ncmd <= 512 && lcd.nargs > 0 && lcd.nargs <= 16) {
        sim.cmd_args[sim.ncmd - 1][lcd.nargs - 1] = b;
        sim.cmd_nargs[sim.ncmd - 1] = (u8)lcd.nargs;
    }
    if (lcd.cmd == 0x2A && lcd.nargs == 4) {
        lcd.wx0 = (lcd.args[0] << 8) | lcd.args[1];
        lcd.wx1 = (lcd.args[2] << 8) | lcd.args[3];
    }
    if (lcd.cmd == 0x2B && lcd.nargs == 4) {
        lcd.wy0 = (lcd.args[0] << 8) | lcd.args[1];
        lcd.wy1 = (lcd.args[2] << 8) | lcd.args[3];
    }
}

static void spi1_wr(u32 off, u32 val)
{
    u32 *con = slot(SPI1 + SPI_CON);
    if (off == SPI_CON) {
        *con = (val & ~0x8000u) | (*con & 0x8000u);
        if (val & SPI_CLR)
        *con &= ~0x8000u;
        return;
    }
    if (off == SPI_BUF) {
        if (!(*con & 1))
        sim_fail("SPI1 byte with the block disabled");
        lcd_byte((u8)val);
        *con |= 0x8000u;
        return;
    }
    if (off == SPI_CNT) {
        u32 n = val & 0xFFFF, i;
        u8 *p = dma_ptr(*slot(SPI1 + SPI_ADR));
        if (!n) {
            sim.dma_zero++;
            sim_fail("zero-length panel DMA");
            return;
        }
        if (p)
        for (i = 0; i < n; i++)
        lcd_byte(p[i]);
        *con |= 0x8000u;
        return;
    }
    *slot(SPI1 + off) = val;
}

/* ------------------------------------------------- key matrix and LEDs */
static u32 sr_acc;
static u32 sr_bits[32];
static void sr_clock(int data)
{
    if (sim.sr_clocks < 32)
        sr_bits[sim.sr_clocks] = (u32)data;
    sim.sr_clocks++;
}

static void sr_latch(void)
{
    u32 n = sim.board == 1 ? 16 : 32, k;
    if (sim.sr_clocks != n) {
        char m[80];
        snprintf(m, sizeof(m), "latch after %u clocks (expected %u)", sim.sr_clocks, n);
        sim_fail(m);
    }
    sr_acc = 0;
    if (sim.board == 1) {
        for (k = 0; k < 16 && k < sim.sr_clocks; k++)
            /* first bit in = bit 15 */
        sr_acc |= sr_bits[k] << (15 - k);
    } else {
        /* stream: least significant byte first, MSB first within each byte */
        for (k = 0; k < 32 && k < sim.sr_clocks; k++)
            sr_acc |= sr_bits[k] << (8 * (k / 8) + 7 - (k % 8));
    }
    sim.word = sr_acc;
    sim.sr_clocks = 0;
}

static int key_down(u32 key)
{
    return sim.keyfn && (sim.keyfn(sim.now) & key);
}

/* Is a key electrically "closed" given the selected columns? */
static int key_closed(u32 key)
{
    u32 w = sim.word;
    if (!key_down(key))
        return 0;
    if (sim.board == 1) {
        switch (key) {
        case SK_REC:
            return !(w & 0x0080);
        case SK_SEL:
            return !(w & 0x0400);
        case SK_PREV:
            return !(w & 0x0001) && !(w & 0x0800);
        case SK_NEXT:
            return !(w & 0x0002) && !(w & 0x1000);
        }
    } else {
        switch (key) {
        case SK_PREV:
            return !(w & (1u << 16));
        case SK_NEXT:
            return !(w & (1u << 28));
        case SK_SEL:
            return !(w & (1u << 24));
        case SK_REC:
            return !(w & (1u << 27));
        }
    }
    return 0;
}

static void check_input_pin(u32 port, u32 pin)
{
    if (!(*slot(SIM_GPIO(port) + GP_DIR) & (1u << pin)) || !(*slot(SIM_GPIO(port) + GP_DIE) & (1u << pin)) || !(*slot(SIM_GPIO(port) + GP_PU) & (1u << pin)) || (*slot(SIM_GPIO(port) + GP_PD) & (1u << pin)))
        sim_fail("key pin read without input + digital enable + pull-up");
}

static u32 gpio_in(u32 port)
{
    u32 v = *slot(SIM_GPIO(port) + GP_OUT), dir = *slot(SIM_GPIO(port) + GP_DIR);
    v = (v & ~dir) | dir;
    /* inputs float high (pull-ups) */
    if (port == PA && sim.board == 1) {
        check_input_pin(PA, 6);
        check_input_pin(PA, 8);
        sim.key_reads++;
        if (key_closed(SK_REC) || key_closed(SK_SEL))
        v &= ~(1u << 6);
        if (key_closed(SK_PREV) || key_closed(SK_NEXT))
        v &= ~(1u << 8);
    }
    if (port == PH && sim.board == 2) {
        check_input_pin(PH, 8);
        sim.key_reads++;
        if (key_closed(SK_REC) || key_closed(SK_SEL) || key_closed(SK_PREV) || key_closed(SK_NEXT))
        v &= ~(1u << 8);
    }
    return v;
}

static void led_account(u32 us)
{
    u32 phy = *slot(PHY_CON), pa = *slot(SIM_GPIO(PA) + GP_OUT), ph = *slot(SIM_GPIO(PH) + GP_OUT);
    u32 w = sim.word;
    if (sim.board == 1) {
        int ph9 = (ph >> 9) & 1, pa10 = (pa >> 10) & 1;
        if (ph9 && !(w & 0x0001) && !(w & 0x0800))
        sim.lit_us[0] += us;
        if (ph9 && !(w & 0x0002) && !(w & 0x1000))
        sim.lit_us[1] += us;
        if (pa10 && !(w & 0x0400))
        sim.lit_us[2] += us;
    } else if ((phy & 0x1000F) == 0x1000F) {
        /* drive enabled */
        int g0 = !(phy & (1u << 17)), g1 = !(phy & (1u << 18));
        if (g0 && !(w & 1))
        sim.lit_us[0] += us;
        if (g1 && !(w & 1))
        sim.lit_us[1] += us;
        if (g1 && !(w & (1u << 12)))
        sim.lit_us[2] += us;
        if (g0 && !(w & (1u << 12)))
        sim.lit_us[3] += us;
        /* KNOB BANK */
    }
}

void boot_sim_settle(unsigned iterations)
{
    sim_flush();
    if (sim.board && !(sim_reg_get(CLK_CON0) & (1u << 8))) {
        /* §3.2: 672000 register-loop iterations took about 14 seconds.
         * Account only for A before the switch, not full-speed SPI CS waits. */
        u32 us = (u32)((unsigned long long)iterations * 1000000 / 48000);
        sim.startup_us += us;
        sim_delay(us);
    }
}

void sim_delay(u32 us)
{
    led_account(us);
    sim.now += us;
    if (sim.limit && sim.now > sim.limit && sim.on_timeout)
        sim.on_timeout();
}

/* --------------------------------------------------------------- dispatch */
u32 sim_rd(u32 a)
{
    if (a == P33_CON || a == P33_DAT || a == P33_PMU || a == P33_RTC)
        return *slot(a);
    if (a == CHIP_ID)
        return sim.chip_id;
    if (a == CACHE_CON)
        return *slot(a) | (1u << 14);
        /* idle */
    if (a == TMR0)
        /* timer: expires at once */
        return *slot(a) | 0x8000u;
    if (a == SIM_GPIO(PD) + GP_IN)
        return *slot(SIM_GPIO(PD) + GP_OUT);
    if (a == SIM_GPIO(PA) + GP_IN)
        return gpio_in(PA);
    if (a == SIM_GPIO(PH) + GP_IN)
        return gpio_in(PH);
    return *slot(a);
}

void sim_wr(u32 a, u32 v)
{
    if (a == PHY_CON)
        sim.phy_writes++;
    if (a == SPI0 && v == 0x28)
        sim.init_count++;
    if (a == XENC_CON && v == 1) {
        u32 base = *slot(SFC_BASE);
        u8 *dest = (u8 *)(uintptr_t)0x02000000;
        u16 key = (u16)*slot(XENC_KEY);
        assert(base < nor_size);
        memcpy(dest, sim_nor + base, nor_size - base);
        for (u32 q = 0; q + 32 <= nor_size - base; q += 32)
        core_enc(key ^ (u16)(q >> 2), dest + q, 32);
        sim.xip_count++;
    }
    if (a == TMR0 && v == 1) {
        if (sim.board && !(*slot(CLK_CON0) & (1u << 8)))
            sim_fail("Timer 0 used before the startup clock switch");
        u32 mhz = (*slot(CLK_DIV) & 0x30700) == 0x10200 ? 60 :
        ((*slot(CLK_CON2) & 255) == 0x40 && (*slot(CLK_DIV) & 0x30700) == 0x100 ? 48 : 24);
        sim_delay(*slot(TMR0 + 8) / mhz);
    }
    if (a == PLL1_A && !sim.pll_seen) {
        sim.pll_seen = 1;
        sim.p33_before_pll = sim.p33_writes;
    }
    if (a >= SPI0 && a < SPI0 + 0x20) {
        spi0_wr(a - SPI0, v);
        return;
    }
    if (a >= SPI1 && a < SPI1 + 0x20) {
        spi1_wr(a - SPI1, v);
        return;
    }
    if (a == P33_CON) {
        p33_bridge_write(v);
        return;
    }
    if (a == SIM_GPIO(PD) + GP_OUT) {
        u32 dir = *slot(SIM_GPIO(PD) + GP_DIR);
        *slot(a) = v;
        if (!(dir & 1))
            /* PD0 is an output: chip select */
        chip_select(!(v & 1));
        return;
    }
    if (a == SIM_GPIO(PD) + GP_DIR) {
        *slot(a) = v;
        return;
    }
    if (a == SIM_GPIO(PA) + GP_OUT && sim.board) {
        u32 old = *slot(a);
        u32 dir = *slot(SIM_GPIO(PA) + GP_DIR);
        /* only driven pins reach the registers */
        *slot(a) = v;
        if (!(dir & 8) && !(old & 8) && (v & 8))
            /* PA3 rising: clock in PA4 */
        sr_clock((v >> 4) & 1);
        if (!(dir & 2) && !(old & 2) && (v & 2))
            /* PA1 rising: latch */
        sr_latch();
        return;
    }
    *slot(a) = v;
}

u32 sim_reg_get(u32 a) {
    return *slot(a);
}

void sim_reset(void)
{
    u32 i;
    memset(reg, 0, sizeof(reg));
    memset(cache_mem, 0, sizeof(cache_mem));
    pending_addr = 0;
    pending_before = 0;
    pending_kind = spi_reply = p33_reply = p33_data_pending = 0;
    memset(extra, 0, sizeof(extra));
    nextra = 0;
    memset(p33, 0, sizeof(p33));
    memset(&ch, 0, sizeof(ch));
    memset(&lcd, 0, sizeof(lcd));
    memset(dma_tab, 0, sizeof(dma_tab));
    dma_next = 0;
    p33_asserted = p33_n = 0;
    memset(&sim, 0, sizeof(sim));
    sim.jedec = 0xEB6014;
    for (i = 0; i < 8; i++)
        sim.uid[i] = (u8)(0xA0 + i);
    sim.sr1 = 0;
    sim.sr2 = 0x02;
    /* QE already set */
    sim.combined_wrsr = 1;
    sim.busy_polls = 3;
    sim.chip_id = 0;
    /* GPIO dir after reset: inputs */
    for (i = 0; i < 8; i++)
        reg[(SIM_GPIO(i) + GP_DIR) >> 2] = 0xFFFF;
    reg[CLK_CON2 >> 2] = 0x80;
    /* ROM state: 192 MHz PLL / 4 */
    reg[CLK_DIV >> 2] = 0;
    sim.fuse[1][3] = 0x00980F00;
    /* key 980F in bits 23:8 */
    reg[SPI0 >> 2] = 0;
}

/* Commit the previous volatile MMIO lvalue before handing out the next one.
 * Byte registers are writes on the first access and replies on the second;
 * DMA count registers are write-only in these drivers. Ordinary registers
 * are committed only when their value changed. The model below this adapter
 * still checks the chip-side protocol independently of the driver. */
void sim_flush(void)
{
    if (!pending_addr)
        return;
    u32 a = (u32)pending_addr;
    u32 value = *slot(a);
    if (pending_kind || value != pending_before) {
        *slot(a) = pending_before;
        sim_wr(a, value);
        if (pending_kind == 1)
            spi_reply = 1;
        if (pending_kind == 2)
            p33_data_pending = 1;
    }
    pending_addr = 0;
    pending_kind = 0;
}

volatile uint32_t *platform_register(uintptr_t address)
{
    sim_flush();
    assert(!(address & 3));
    u32 a = (u32)address;
    if (a == P33_CON && p33_data_pending) {
        u32 v = *slot(a);
        if (v & 0x10) {
            *slot(a) = v & ~0x10u;
            p33_bridge_write(v);
            p33_data_pending = 0;
            p33_reply = 1;
        }
    }
    pending_addr = address;
    if (a == SPI0 + SPI_BUF) {
        if (spi_reply)
            spi_reply = 0;
        else
            pending_kind = 1;
    } else if (a == P33_DAT) {
        if (p33_reply)
            p33_reply = 0;
        else
            pending_kind = 2;
    } else if (a == SPI1 + SPI_BUF || a == SPI0 + SPI_CNT || a == SPI1 + SPI_CNT) {
        pending_kind = 3;
    }
    u32 value = sim_rd(a);
    *slot(a) = value;
    pending_before = value;
    return slot(a);
}
