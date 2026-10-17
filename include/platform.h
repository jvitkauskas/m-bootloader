/* SPDX-License-Identifier: MIT */
#ifndef MB_PLATFORM_H
#define MB_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef PLATFORM_SIM
volatile uint32_t *platform_register(uintptr_t address);
#define REG(a) (*platform_register(a))
#else
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#endif
#ifdef BOOT_SIM
uint32_t platform_dma_address(const void *pointer);
#define DMA_ADDRESS(p) platform_dma_address(p)
#else
#define DMA_ADDRESS(p) ((uint32_t)(uintptr_t)(p))
#endif
#define GPIO(p,o) REG(0x50000u + 0x40u*(p)+(o))
#define BIT(n) (1u << (n))
#ifdef PLATFORM_SIM
static inline void sync_cpu(void) {
}

static inline void irq_off(void) {
}

#else
static inline void sync_cpu(void) {
    __asm__ volatile("csync" ::: "memory");
}

static inline void irq_off(void) {
    __asm__ volatile("cli" ::: "memory");
}

#endif
void gpio_output(unsigned port, unsigned pin, bool high);
void gpio_level(unsigned port, unsigned pin, bool high);
void gpio_input(unsigned port, unsigned pin, bool up, bool down);
bool gpio_read(unsigned port, unsigned pin);
void hal_early(void);
bool hal_clock_a(void);
unsigned hal_clock_b(void);
bool hal_delay_us(uint32_t usec);
uint8_t p33_read(unsigned address);
void p33_write(unsigned address, uint8_t value);
void p33_or(unsigned address, uint8_t value);
void p33_and(unsigned address, uint8_t value);
bool hal_fault(void);
void hal_fault_clear(void);
uint32_t hal_efuse(unsigned bank, unsigned row);
bool flash_init(unsigned width, unsigned baud);
bool flash_quad(void);
unsigned flash_read_width(void);
uint32_t flash_jedec(void);
void flash_uid(uint8_t out[8]);
uint32_t flash_capacity(void);
bool flash_read(void *ctx, uint32_t address, void *out, uint32_t size);
bool flash_dma_start(uint32_t address, void *out, uint32_t size);
bool flash_dma_finish(void);
bool flash_program(void *ctx, uint32_t address, const void *data, uint32_t size);
bool flash_erase(void *ctx, uint32_t address, uint32_t size);
void flash_release(void);
bool hal_xip(uint32_t base, uint16_t key, unsigned width, unsigned baud);
void hal_reset(void) __attribute__((noreturn));
#endif
