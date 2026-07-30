#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>

void kernel_main(void)
{
    serial_init();
    put_string("UEFI -> Bootloader -> Kernel OK\n");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
