OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

SECTIONS
{
    . = 0x41000000;
    /* 与 init（0x40002000）错开基址，未来同地址空间双任务不重叠 */
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    /* 上限 8 页：GUI 代码（光标/合成/字体）会持续膨胀，窗口余量充足
     * （堆跟在映像末端之后，显存窗在 0x41800000，互不重叠） */
    ASSERT(. <= 0x41000000 + 8 * 0x1000, "compositor binary exceeds 8 pages")
}
