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
#include <smp.h>
#include <user.h>
#include <syscall.h>
#include <ata/ata.h>
#include <string.h>

static void ata_run_test()
{
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    static const char msg[] = "Hello,World!";
    const ata_ops_t *ops = ata_current();
    int rc = ops->init();

    if (rc == 0)
    {
        uint8_t drive = 2;
        uint64_t cap = ops->drive_sectors(drive);

        fput_string("[ATA] ops=%s drive=%d capacity=%llu sectors (%llu MB)\n",
                    ops->name, drive,
                    cap, cap / 2048);
        kernel_memset(wbuf, 0, (uint32_t)sizeof(wbuf));
        kernel_memcpy(wbuf, (void *)msg, (uint32_t)(sizeof(msg) - 1));

        rc = ops->write_sectors(drive, 0, 1, wbuf);
        if (rc != 0)
        {
            fput_string("[ATA] LBA0 write failed rc=%d\n", rc);
        }
        else if (ops->read_sectors(drive, 0, 1, rbuf) != 0)
        {
            put_string("[ATA] LBA0 read failed\n");
        }
        else
        {
            /* wbuf 已清零，读回串以 NUL 结尾，可直接 %s 打印 */
            fput_string("[ATA] LBA0 read: %s\n", (char *)rbuf);
        }
    }
    else
    {
        fput_string("[ATA] no disk (rc=%d), continue diskless boot\n", rc);
    }
}

static void user_task()
{
    user_enter_demo();
    /* user_enter_demo 不返回；防御性兜底 */
    for (;;)
        __asm__ volatile("hlt");
}

void kernel_main(boot_info_t *boot_info)
{
    serial_init();
    init_gdt();
    init_idt();
    init_syscall();

    init_pmm(boot_info);
    init_vmm(boot_info);
    init_heap();

    init_acpi(boot_info);
    init_lapic();
    init_ioapic();
    init_kbd();
    init_apic_timer(APIC_TIMER_DEFAULT_HZ);
    init_sched();
    init_smp();

    ata_run_test();

    if (task_create_pinned(user_task, "user") == NULL)
        put_string("FATAL: user task_create failed\n");

    __asm__ volatile("sti");

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);
}
