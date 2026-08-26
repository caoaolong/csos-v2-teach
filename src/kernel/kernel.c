#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>
#include <assert.h>
#include <gdt.h>
#include <idt.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/heap.h>
#include <gfx/fb.h>
#include <gfx/logo.h>
#include <pic.h>
#include <pit.h>
#include <acpi.h>
#include <apic/lapic.h>

void kernel_main(boot_info_t *boot_info)
{
    serial_init();
    init_gdt();
    init_idt();

    init_pmm(boot_info);
    init_vmm(boot_info);
    init_heap();

    init_acpi(boot_info);
    init_lapic();

    init_pic();
    if (init_pit(PIT_DEFAULT_HZ) != 0)
        put_string("FATAL: init_pit failed\n");
    __asm__ volatile("sti");

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
