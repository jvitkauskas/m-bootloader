/* SPDX-License-Identifier: MIT */
/* Entry of the RAM stage (src/stage.c). It never returns, so it leaves the caller's stack at once,
 * and passes on r0 and r1 untouched. */
.section .entry,"ax",@progbits
.global stage_entry
.type stage_entry,@function
stage_entry:
    r2 = __exception_stack_top
    ssp = r2
    r2 = __main_stack_top
    sp = r2
    goto stage_main
    /* Felucca-family firmware stores only its own loader, which writes the app area alone and checks
     * the chip key, as this stage does. It looks for this mark among the stream's first literals. */
    .ascii "FELUCCA-LOADER-1"
.size stage_entry,.-stage_entry
