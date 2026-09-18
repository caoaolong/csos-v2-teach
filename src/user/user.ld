OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

SECTIONS
{
    . = 0x40000000;
    /* 入口 stub 固定在二进制起始处，其余代码随后 */
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    ASSERT(. <= 0x40000000 + 0x1000, "user binary exceeds one page")
}
