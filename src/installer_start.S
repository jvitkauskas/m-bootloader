/* SPDX-License-Identifier: MIT */
/* Staged updater (§6.3) that installs the bootloader built with it. It never
 * returns, so it leaves the caller's stack, wherever that is, at once, and
 * passes on r0 (context) and r1 (stream end) untouched. */
.section .entry,"ax",@progbits
.global installer_entry
.type installer_entry,@function
installer_entry:
    r2 = __exception_stack_top
    ssp = r2
    r2 = __main_stack_top
    sp = r2
    goto installer_main
.size installer_entry,.-installer_entry

.section .rodata.container,"a",@progbits
.global installer_container
.global installer_container_end
.p2align 2
installer_container:
    .incbin "uboot.boot"
installer_container_end:
