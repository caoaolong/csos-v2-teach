#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>
#include <assert.h>

void kernel_main(void)
{
    serial_init();
    put_string("UEFI -> Bootloader -> Kernel OK\n");

    // fput_string test
    fput_string("fput_string test: %s, n=%d, hex=0x%x\n", "hello", 42, 0xDEAD);
    fput_string("fput_string test: signed=%d, unsigned=%u, oct=%o\n", -7, 7u, 8);

    assert(1 + 1 == 2);
    assert(0);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
