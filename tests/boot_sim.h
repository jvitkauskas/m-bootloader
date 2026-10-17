/* SPDX-License-Identifier: MIT */
/* Register-level simulator of the chip blocks the drivers use, with a model
 * of the SPI NOR on SPI0 and of the panel on SPI1. It checks the protocol the
 * drivers speak (opcodes, dummy clocks, write enable, page wrap, protection),
 * not electrical behaviour. */
#ifndef SIM_H
#define SIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define NOR_MAX 0x100000u
extern u8 sim_nor[NOR_MAX];
extern u32 nor_size;
struct nor_op {
    char kind;
    u32 addr, len;
};
extern struct nor_op nor_log[];
extern u32 nor_nlog;
extern long nor_budget;
extern u32 nor_reads;
int nor_page(u32 addr, const u8 *s, u32 n);
int nor_erase(u32 addr, u32 n);
void nor_load(const u8 *img, u32 n);
void nor_clear_log(void);
int nor_touched(u32 addr, u32 n);
void sim_flush(void);
u32 platform_dma_address(const void *p);
u32 sim_rd(u32 a);
void sim_wr(u32 a, u32 v);
/* Adapted from the other clean-room implementation (a), under MIT.
 * See NOTICE.md. */
struct sim {
    /* flash chip model (storage is sim_nor[]) */
    u32 jedec;
    /* mfr<<16 | type<<8 | cap */
    u8 uid[8];
    u8 sr1, sr2;
    int combined_wrsr;
    /* part accepts 01 with two data bytes */
    int ignore_wrsr;
    /* status writes have no effect */
    int bad_quad, bad_dual;
    /* 6B / 3B reads return inverted data */
    int busy_polls;
    /* status reads that report WIP after a program/erase */
    u32 busy_left;
    /* efuse: [bank][row] */
    u32 fuse[4][8];
    u32 chip_id;
    /* counters / diagnostics */
    int err;
    /* protocol violations seen */
    char errmsg[160];
    u32 dma_reads, dma_bytes;
    u32 byte_rx, byte_tx;
    u32 status_polls;
    u32 wrsr_cnt;
    u32 p33_writes;
    /* display */
    int panel_on;
    u32 phy_writes;
    u32 init_count;
    u32 xip_count;
    u32 dma_dummy_quad, dma_dummy_dual;
    u16 fb[240 * 240];
    u32 fb_pix;
    /* pixels written */
    u32 cmds[512];
    u32 ncmd;
    /* log of command bytes in order */
    u8 cmd_args[512][16];
    u8 cmd_nargs[512];
    u32 dma_zero;
    /* zero-length DMA attempts */
    /* key matrix, shift registers, LEDs */
    int board;
    /* 1 FM-1, 2 SMK-37 */
    u32 (*keyfn)(unsigned long long now_us);
    /* SK_* keys held at that time */
    unsigned long long now;
    /* virtual microseconds */
    unsigned long long startup_us;
    /* §3.2's measured slow-clock time spent in pre-switch busy loops. */
    u32 word;
    /* latched shift-register word (logical bit order) */
    u32 sr_clocks;
    /* clocks since the last latch */
    unsigned long long lit_us[4];
    /* prev, next, sel, unwanted LED on-time */
    u32 key_reads;
    unsigned long long limit;
    /* virtual time after which on_timeout runs (0 = none) */
    void (*on_timeout)(void);
    u32 pll_seen, p33_before_pll;
};
#define SK_PREV 1u
#define SK_NEXT 2u
#define SK_SEL 4u
#define SK_REC 8u
void sim_delay(u32 us);
extern struct sim sim;
void sim_reset(void);
/* power-on state of every block; keeps sim_nor[] */
void sim_fail(const char *msg);
u32 sim_reg_get(u32 addr);
/* P33 register file (not CPU space) */
u8 sim_p33_get(u32 reg);
void sim_p33_set(u32 reg, u8 v);
/* last value written to an unmodelled CPU register */
#endif
