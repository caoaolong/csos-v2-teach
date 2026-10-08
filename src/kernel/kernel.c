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
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <string.h>

static void vfs_test()
{
    vfs_file_t *f = NULL;
    static const char msg[] = "Hello,VFS!";
    char rbuf[32];
    int64_t n;
    if (vfs_register_driver(&ramfs_driver) != VFS_OK)
    {
        put_string("[VFS-TEST] register failed\n");
        return;
    }
    if (vfs_mount("ramfs", "/ram", "mem0") != VFS_OK)
    {
        put_string("[VFS-TEST] mount failed\n");
        return;
    }
    if (vfs_open("/ram/a.txt", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &f) != VFS_OK)
    {
        put_string("[VFS-TEST] open for write failed\n");
        return;
    }
    vfs_write(f, msg, sizeof(msg) - 1);
    vfs_close(f);

    if (vfs_open("/ram/a.txt", VFS_O_RDONLY, &f) != VFS_OK)
    {
        put_string("[VFS-TEST] reopen failed\n");
        return;
    }
    kernel_memset(rbuf, 0, (uint32_t)sizeof(rbuf));
    n = vfs_read(f, rbuf, sizeof(rbuf) - 1);
    vfs_close(f);
    fput_string("[VFS-TEST] read n=%lld data='%s' %s\n",
                n, rbuf, kernel_strcmp(rbuf, msg) == 0 ? "OK" : "MISMATCH");
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

    if (!ata_current()->init())
        put_string("FATAL: ata initialze failed\n");

    init_vfs();
    vfs_test();

    if (task_create_pinned(user_task, "user") == NULL)
        put_string("FATAL: user task_create failed\n");

    __asm__ volatile("sti");

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);
}
