ENTRY(_start)
SECTIONS
{
    . = 0xA000;
    .text : AT(0) {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }
}
