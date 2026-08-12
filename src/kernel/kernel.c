#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>
#include <assert.h>
#include <gdt.h>
#include <idt.h>
#include <memory/pmm.h>

void kernel_main(boot_info_t *boot_info)
{
    init_gdt();
    init_idt();

    serial_init();

    put_string("before int3\n");
    __asm__ volatile("int3");
    /* 若门有效，这行不会出现 */
    put_string("after int3\n");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
