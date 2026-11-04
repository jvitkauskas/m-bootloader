/* SPDX-License-Identifier: MIT */
/* Stage 1: straight after stage 0, whose functions it calls through --just-symbols. */
ENTRY(boot_main)
MEMORY { ram (rwx) : ORIGIN = STAGE1_ORIGIN, LENGTH = 0x1c0a800 - STAGE1_ORIGIN }
SECTIONS {
    .text ORIGIN(ram) : {
        KEEP(*(.entry))
        *(.text .text.*) *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    } > ram
    __payload_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8); __bss_end = .;
    } > ram
    __ram_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
    ASSERT(__ram_end <= __stage0_limit, "stage 1 runs into stage 0's data")
}
