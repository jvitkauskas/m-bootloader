/* SPDX-License-Identifier: MIT */
/* The self-extracting installer's first byte (src/sfx.c), and its pack. The jump keeps r0 and r1, and the caller's
 * stack and return address, for sfx_main. */
.section .entry,"ax",@progbits
.global sfx_entry
.type sfx_entry,@function
sfx_entry:
    goto sfx_main
.size sfx_entry,.-sfx_entry

.section .pack,"a",@progbits
.global sfx_pack
.global sfx_pack_end
sfx_pack:
    .incbin "installer.pack"
sfx_pack_end:
