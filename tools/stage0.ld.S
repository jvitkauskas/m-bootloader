/* SPDX-License-Identifier: MIT */
/* Stage 0: at the bootloader's base, with its data and the stacks at the top of its RAM. */
ENTRY(boot_entry)
MEMORY { ram (rwx) : ORIGIN = 0x1c02000, LENGTH = 0x8800 }
SECTIONS {
    .text ORIGIN(ram) : {
        KEEP(*(.entry))
        *(.text .text.*) *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    } > ram
    __payload_end = .;
    stage1_entry = .;
    /* Stage 1 is linked here, up to __stage0_limit. */
    __stage0_limit = ORIGIN(ram) + LENGTH(ram) - 5120 - 256;
    .bss __stage0_limit (NOLOAD) : {
        __stage0_bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8); __stage0_bss_end = .;
    } > ram
    ASSERT(__stage0_bss_end <= __stage0_limit + 256, "stage 0 data overflow")
    .stacks ORIGIN(ram) + LENGTH(ram) - 5120 (NOLOAD) : {
        __exception_stack_bottom = .; . += 1024; __exception_stack_top = .;
        __main_stack_bottom = .; . += 4096; __main_stack_top = .;
    } > ram
    __ram_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
