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

static void vfs_check(const char *what, int got, int want)
{
    fput_string("[VFS-TEST] %s rc=%d (expect %d) %s\n",
                what, got, want, got == want ? "OK" : "FAIL");
}

static void vfs_check_cwd(const char *want)
{
    char cwd[VFS_PATH_MAX];

    vfs_getcwd(cwd, sizeof(cwd));
    fput_string("[VFS-TEST] cwd='%s' (expect '%s') %s\n",
                cwd, want, kernel_strcmp(cwd, want) == 0 ? "OK" : "FAIL");
}

static void vfs_list(const char *path)
{
    vfs_dir_t *d = NULL;
    vfs_dirent_t de;

    if (vfs_opendir(path, &d) != VFS_OK)
    {
        fput_string("[VFS-TEST] opendir %s failed\n", path);
        return;
    }
    fput_string("[VFS-TEST] ls %s:\n", path);
    while (vfs_readdir(d, &de) == VFS_OK)
        fput_string("[VFS-TEST]   %s%s\n", de.name, de.is_dir ? "/" : "");
    vfs_closedir(d);
}

static void vfs_test()
{
    vfs_dir_t *d = NULL;
    vfs_dirent_t de;

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

    /* mkdir：空盘上只测目录创建 */
    vfs_check("mkdir /ram/dir", vfs_mkdir("/ram/dir"), VFS_OK);
    vfs_check("mkdir again", vfs_mkdir("/ram/dir"), VFS_ERR_EXIST);
    vfs_check("mkdir missing parent", vfs_mkdir("/ram/x/y"), VFS_ERR_NOENT);

    /* chdir / getcwd / 相对路径（只用目录） */
    vfs_check("chdir /ram/dir", vfs_chdir("/ram/dir"), VFS_OK);
    vfs_check_cwd("/ram/dir");
    vfs_check("chdir missing", vfs_chdir("/ram/none"), VFS_ERR_NOENT);
    vfs_check("mkdir relative sub", vfs_mkdir("sub"), VFS_OK);
    vfs_check("mkdir relative z", vfs_mkdir("z"), VFS_OK);

    vfs_check("chdir ..", vfs_chdir(".."), VFS_OK);
    vfs_check_cwd("/ram");

    /* rmdir 错误路径（不依赖文件） */
    vfs_check("rmdir non-empty dir", vfs_rmdir("dir"), VFS_ERR_NOTEMPTY);
    vfs_check("rmdir missing", vfs_rmdir("none"), VFS_ERR_NOENT);
    vfs_check("rmdir mount point", vfs_rmdir("/ram"), VFS_ERR_BUSY);
    vfs_list("/ram");

    /* 游标遍历 + 游标钉住：正在被游标指向的子目录不可删除
     * dir 下只有 sub、z（有序），#1=sub、#2=z，此时 z 被钉住 */
    if (vfs_opendir("/ram/dir", &d) == VFS_OK)
    {
        vfs_readdir(d, &de);
        fput_string("[VFS-TEST] cursor #1='%s'\n", de.name);
        vfs_readdir(d, &de);
        fput_string("[VFS-TEST] cursor #2='%s'\n", de.name);
        vfs_check("rmdir cursor-pinned 'z'", vfs_rmdir("/ram/dir/z"), VFS_ERR_BUSY);
        vfs_check("readdir at end", vfs_readdir(d, &de), VFS_ERR_NOENT);
        vfs_closedir(d);
    }
    vfs_check("rmdir sub after closedir", vfs_rmdir("/ram/dir/sub"), VFS_OK);
    vfs_check("rmdir z", vfs_rmdir("/ram/dir/z"), VFS_OK);
    vfs_list("/ram/dir");
    vfs_check("rmdir emptied dir", vfs_rmdir("/ram/dir"), VFS_OK);
    vfs_list("/ram");
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

    if (ata_current()->init())
        put_string("FATAL: ata initialze failed\n");

    init_vfs();
    vfs_test();

    if (task_create_pinned(user_task, "user") == NULL)
        put_string("FATAL: user task_create failed\n");

    __asm__ volatile("sti");

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);
}
