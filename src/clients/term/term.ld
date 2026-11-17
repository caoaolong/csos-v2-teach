OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

SECTIONS
{
    /* 基址 0x41200000：init 占 0x40002000 段，compositor 占 0x41000000 段，
     * 显存窗 0x42000000 起，堆跟在映像末端之后，均不重叠 */
    . = 0x41200000;
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    /* 上限 8 页：与 init/compositor 同口径 */
    ASSERT(. <= 0x41200000 + 8 * 0x1000, "term binary exceeds 8 pages")
}
