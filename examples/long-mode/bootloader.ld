ENTRY(_start)
SECTIONS
{
    . = 0x8000;
    .text : AT(0) {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }
}
