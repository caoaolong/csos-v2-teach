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
#include <apic.h>
#include <apic/ioapic.h>
#include <kbd.h>
#include <timer.h>
#include <sched.h>

static void thread_a(void)
{
    for (;;)
    {
        put_string("A");
        msleep(100);
    }
}

static void thread_b(void)
{
    for (;;)
    {
        put_string("B");
        msleep(100);
    }
}

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
    init_ioapic();
    init_kbd();
    init_apic_timer(APIC_TIMER_DEFAULT_HZ);
    init_sched();

    if (task_create(thread_a, "A") == NULL || task_create(thread_b, "B") == NULL)
        put_string("FATAL: task_create failed\n");

    __asm__ volatile("sti");

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
