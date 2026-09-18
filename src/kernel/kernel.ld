ENTRY(_start)

KERNEL_PHYS_BASE = 0x100000;
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Bootstrap：VMA = LMA = 0x100000，UEFI 跳入 _start */
    . = KERNEL_PHYS_BASE;

    .bootstrap : {
        __bootstrap_start = .;
        *(.text.bootstrap)
        *(.text.boot_paging)
        . = ALIGN(4096);
        __boot_paging_data = .;
        *(.bss.boot_paging)
        . = ALIGN(16);
        __boot_stack_bottom = .;
        . += 8192;
        __boot_stack_top = .;
        __bootstrap_end = .;
    }

    /*
     * 主内核：单一 PT_LOAD，LMA 连续且页对齐，VMA 连续位于高半核。
     * 避免 .text/.rodata/.data/.bss 各自独立段导致 p_paddr 未对齐。
     */
    . = KERNEL_VIRT_BASE;

    .kernel : AT(ALIGN(__bootstrap_end, 4096)) {
        __kernel_virt_start = .;
        __kernel_start = .;
        __kernel_phys_start = ALIGN(__bootstrap_end, 4096);

        *(.text .text.*)

        . = ALIGN(4096);
        *(.rodata .rodata.*)

        . = ALIGN(4096);
        *(.data .data.*)

        . = ALIGN(16);
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;

        . = ALIGN(4096);
        __kernel_end = .;
    }

    __kernel_phys_end = LOADADDR(.kernel) + SIZEOF(.kernel);
}
