OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

INPUT(crt0.o)
GROUP(libc.a)

SECTIONS
{
    . = 0x40002000;
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }

    ASSERT(. < 0x42000000, "term image overlaps fb window")
}
