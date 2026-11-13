OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

SECTIONS
{
    /* 基址 0x40002000：固定用户栈页占 [0x40001000, 0x40002000)，映像若从
     * 0x40000000 起一旦超过 1 页即被栈映射覆盖（map_page 无条件重写）。
     * 映像整体落在栈页之上，与栈/堆（= max(映像末端, 栈顶)）互不重叠。 */
    . = 0x40002000;
    /* 入口 stub 固定在二进制起始处，其余代码随后 */
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    /* 加载器支持多页段；上限 8 页（32KB）防二进制无节制膨胀
     * （USER_IMAGE 窗口 32MB，display 基址 0x41000000 处仍有充足余量） */
    ASSERT(. <= 0x40002000 + 8 * 0x1000, "init binary exceeds 8 pages")
}
