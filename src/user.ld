OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

/* 全用户程序统一链接脚本（init / compositor / term / cmd）：各任务独占
 * pgdir，同一虚址互不干扰，故共享基址；工具链缺省脚本在 freestanding 下
 * 不可用，此文件即本项目的“默认脚本”，改布局只改这里。
 */
SECTIONS
{
    /* 基址 0x40002000：固定用户栈页占 [0x40001000, 0x40002000)，映像若从
     * 0x40000000 起一旦超过 1 页即被栈映射覆盖（map_page 无条件重写）。
     * 映像整体落在栈页之上，与栈/堆（= max(映像末端, 栈顶)）互不重叠。
     * 入口 stub 固定在二进制起始处，其余代码随后 */
    . = 0x40002000;
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    /* 唯一红线：映像末端（含 bss）不得撞上显存窗 0x42000000；
     * 堆跟在映像末端之后（user.c 跟随），加载器亦按段拒绝越界 */
    ASSERT(. < 0x42000000, "user image overlaps fb window")
}
