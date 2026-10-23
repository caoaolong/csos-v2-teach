#include <fs/mount.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <fs/fat32.h>
#include <part/part.h>
#include <serial.h>
#include <string.h>

static const char *const g_probe_fs[] = {
    "fat32",
};

static int mount_auto(const char *mp)
{
    char name[PART_NAME_MAX];
    int total = part_count();
    int i, k;

    for (i = 0; i < total; i++)
    {
        if (part_name(i, name, sizeof(name)) != VFS_OK)
            continue;
        for (k = 0; k < (int)(sizeof(g_probe_fs) / sizeof(g_probe_fs[0])); k++)
        {
            int rc = vfs_mount(g_probe_fs[k], mp, name);

            if (rc == VFS_OK)
            {
                fput_string("[MOUNT] %s -> %s driver='%s'\n", name, mp, g_probe_fs[k]);
                return VFS_OK;
            }
            if (rc != VFS_ERR_NODEV && rc != VFS_ERR_NOTSUP)
                fput_string("[MOUNT] probe %s with '%s' rc=%d\n", name, g_probe_fs[k], rc);
        }
    }
    fput_string("[MOUNT] no probeable fs for %s, skip\n", mp);
    return VFS_ERR_NODEV;
}

void init_mounts()
{
    if (vfs_register_driver(&ramfs_driver) != VFS_OK)
        put_string("[MOUNT] register ramfs failed\n");
    if (vfs_register_driver(&fat32_driver) != VFS_OK)
        put_string("[MOUNT] register fat32 failed\n");

    /* 固定挂载：类型写死，不探测 */
    if (vfs_mount("devfs", "/dev", "devfs") != VFS_OK)
        put_string("[MOUNT] mount /dev failed\n");
    if (vfs_mount("ramfs", "/", "ramfs") != VFS_OK)
        put_string("[MOUNT] FATAL: root mount failed\n");

    /* 数据分区：自动探测（首个 FAT32 卷 -> /fat） */
    mount_auto("/fat");
}

void test_fat_read()
{
    static const char path[] = "/fat/usr/local/bin/helloworld.txt";
    vfs_file_t *f = NULL;
    char buf[128];
    int64_t n;
    int i;

    /* 只读打开并打印内容 */
    if (vfs_open(path, VFS_O_RDONLY, &f) != VFS_OK)
    {
        fput_string("[TEST] open '%s' FAIL\n", path);
        return;
    }
    kernel_memset(buf, 0, sizeof(buf));
    n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    fput_string("[TEST] read '%s' n=%lld data='%s' %s\n",
                path, n, buf, n > 0 ? "OK" : "FAIL");
}
