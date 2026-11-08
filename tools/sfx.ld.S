/* SPDX-License-Identifier: MIT */
/* The self-extracting installer (src/sfx.c): its entry at 0x01C0A800, the pack at SFX_PACK (tools/sfx_layout.c
 * place), the unpacker after it. objcopy fills the gap with zeros, which the stream's LZ4 takes to almost nothing. */
ENTRY(sfx_entry)
MEMORY { ram (rwx) : ORIGIN = 0x1c0a800, LENGTH = 0xc000 }
SECTIONS {
    .entry ORIGIN(ram) : { KEEP(*(.entry)) } > ram
    .pack SFX_PACK : { KEEP(*(.pack)) } > ram
    .text : {
        *(.text .text.*) *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    } > ram
    __payload_end = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > ram
    __ram_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
    ASSERT(__ram_end <= ORIGIN(ram) + LENGTH(ram), "private RAM overflow")
    ASSERT(__payload_end - ORIGIN(ram) <= 0x5b1c, "self-extracting installer exceeds the stock updater's size")
}
