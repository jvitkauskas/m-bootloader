/* SPDX-License-Identifier: MIT */
ENTRY(ENTRY_SYMBOL)
MEMORY { ram (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH }
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
#ifndef LOADER_BUILD
    .stacks (NOLOAD) : {
        __exception_stack_bottom = .; . += 1024; __exception_stack_top = .;
        __main_stack_bottom = .; . += 4096; __main_stack_top = .;
    } > ram
#else
    /* Calls use the existing ROM stack, which entry must preserve. */
#endif
    __ram_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
    ASSERT(__ram_end <= ORIGIN(ram) + LENGTH(ram), "private RAM overflow")
#ifndef LOADER_BUILD
    ASSERT(__payload_end - ORIGIN(ram) <= 0x3820, "boot payload exceeds stock flash slot")
#endif
}
