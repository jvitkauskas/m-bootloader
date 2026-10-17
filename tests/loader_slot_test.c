/* SPDX-License-Identifier: MIT */
/* Compile twice to give each real loader_target.c entry independent private
 * state. Test linker aliases bound the actual entry's BSS memset to that state;
 * the release ELF checks separately enforce the complete target BSS layout. */
#ifdef TEST_SLOT_A
#define loader_entry loader_test_entry_a
#define __bss_start loader_test_bss_start_a
#define __bss_end loader_test_bss_end_a
#define TEST_INSTALL loader_test_install_a
#define TEST_STATE loader_test_state_a
#else
#define loader_entry loader_test_entry_b
#define __bss_start loader_test_bss_start_b
#define __bss_end loader_test_bss_end_b
#define TEST_INSTALL loader_test_install_b
#define TEST_STATE loader_test_state_b
#endif
#define TEST_STRING_INNER(value) #value
#define TEST_STRING(value) TEST_STRING_INNER(value)
#include "../src/loader_target.c"
struct loader_state *TEST_STATE(void)
{
    return &loader;
}

int TEST_INSTALL(const uint32_t params[3])
{
    __asm__ volatile( ".globl " TEST_STRING(__bss_start) "\n" ".set " TEST_STRING(__bss_start) ", %c0\n" ".globl " TEST_STRING(__bss_end) "\n" ".set " TEST_STRING(__bss_end) ", %c0 + %c1\n" : : "i" (&loader), "i" (sizeof(loader)));
    return loader_entry(params);
}
