#include <stddef.h>
#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>
#include <kernel/assert.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/heap.h>
#include <kernel/gfx/fb.h>
#include <kernel/gfx/console.h>
#include <kernel/pic.h>
#include <kernel/pit.h>
#include <kernel/acpi.h>
#include <kernel/apic.h>
#include <kernel/apic/ioapic.h>
#include <kernel/kbd.h>
#include <kernel/timer.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/panic.h>
#include <kernel/user.h>
#include <kernel/syscall.h>
#include <kernel/ata/ata.h>
#include <kernel/fs/devfs.h>
#include <kernel/vdev.h>
#include <kernel/devices.h>
#include <kernel/string.h>
#include <kernel/part/part.h>
#include <kernel/fs/mount.h>
#include <kernel/config.h>
#include <kernel/fd.h>

static void user_task()
{
    /* 当前任务即 user 任务：先装好 0/1/2 再进用户态 */
    fd_init_std(sched_current());
    user_enter_init(g_boot_config.init);
    /* user_enter_* 不返回；防御性兜底 */
    for (;;)
        __asm__ volatile("hlt");
}

void kernel_main(boot_info_t *boot_info)
{
    parse_apply_boot_config(boot_info);
    boot_config_summary();

    init_gdt();
    init_idt();
    init_syscall();

    init_pmm(boot_info);
    init_vmm(boot_info);
    init_heap();

    /* splash + 文本区初始化 */
    fb_console_init_boot(boot_info);
    {
        uint32_t cols = 0;
        uint32_t rows = 0;

        fb_console_size(&cols, &rows);
        fput_string("[CONSOLE] READY %ux%u\n", cols, rows);
    }

    init_acpi(boot_info);
    init_lapic();
    init_ioapic();
    init_kbd();
    init_apic_timer(APIC_TIMER_DEFAULT_HZ);
    init_sched();
    init_smp();

    init_ata();

    init_vfs();
    init_vdev();
    init_devfs();
    init_devices();
    init_partitions();
    init_mounts();

    if (task_create_pinned(user_task, "user") == NULL)
        panic("FATAL: user task_create failed\n");

    __asm__ volatile("sti");
}
