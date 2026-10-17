/* SPDX-License-Identifier: MIT */
/* Simulated NOR storage: enforces 1->0 programming, 256-byte pages, 4 KiB /
 * 64 KiB erase units, logs every modifying operation, can lose "power".
 * Shared by the core tests (through fl_*) and the register simulator. */
#include "boot_sim.h"
u8 sim_nor[NOR_MAX];
u32 nor_size;
struct nor_op nor_log[1 << 16];
u32 nor_nlog;
long nor_budget = -1;
u32 nor_reads;
void nor_load(const u8 *img, u32 n)
{
    memcpy(sim_nor, img, n);
    nor_size = n;
    nor_budget = -1;
    nor_clear_log();
}

void nor_clear_log(void)
{
    nor_nlog = 0;
    nor_reads = 0;
}

int nor_touched(u32 a, u32 n)
{
    u32 i;
    for (i = 0; i < nor_nlog; i++)
        if (nor_log[i].addr < a + n && a < nor_log[i].addr + nor_log[i].len)
            return 1;
    return 0;
}

static int spend(void)
{
    if (nor_budget == 0)
        return 0;
    if (nor_budget > 0)
        nor_budget--;
    return 1;
}

static void log_op(char k, u32 a, u32 n)
{
    if (nor_nlog < sizeof(nor_log) / sizeof(nor_log[0])) {
        nor_log[nor_nlog].kind = k;
        nor_log[nor_nlog].addr = a;
        nor_log[nor_nlog].len = n;
        nor_nlog++;
    }
}

/* One page program; the caller guarantees it stays inside a page. */
int nor_page(u32 addr, const u8 *s, u32 m)
{
    u32 i;
    if (!spend())
        return -1;
    log_op('P', addr, m);
    for (i = 0; i < m; i++)
        sim_nor[addr + i] &= s[i];
    return 0;
}

int nor_erase(u32 addr, u32 n)
{
    if (!spend())
        return -1;
    log_op('E', addr, n);
    memset(sim_nor + addr, 0xFF, n);
    return 0;
}
