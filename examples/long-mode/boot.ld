ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : AT(0) {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }
}
