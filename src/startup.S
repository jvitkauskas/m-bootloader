/* SPDX-License-Identifier: MIT */
#include "boot_version.h"
.section .entry,"ax",@progbits
.global boot_entry
.type boot_entry,@function
boot_entry:
    /* §4.1: leave the ROM's stacks untouched until exception return. */
    r1 = 0
    rete = r1
    r1 = boot_normal
    reti = r1
    rti
    .org 0x40
    .ascii "AC79BLID"
#ifdef BOARD_SMK
    .ascii "smk37\0\0\0"
#else
    .ascii "fm1\0\0\0\0\0"
#endif
.Lversion:
    .asciz BOOT_VERSION
    .zero 32 - (. - .Lversion)
boot_normal:
    r1 = __exception_stack_top
    ssp = r1
    r1 = __main_stack_top
    sp = r1
    goto boot_main
.size boot_entry,.-boot_entry
