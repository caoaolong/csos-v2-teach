#include <fs/mount.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <fs/fat32.h>
#include <fs/ext4.h>
#include <part/part.h>
#include <serial.h>
#include <string.h>

static const char *const g_probe_fat[] = {
    "fat32",
};

static const char *const g_probe_ext[] = {
    "ext4",
};

static int mount_auto(const char *mp, const char *const *probe, int nprobe)
{
    char name[PART_NAME_MAX];
    int total = part_count();
    int i, k;

    for (i = 0; i < total; i++)
    {
        if (part_name(i, name, sizeof(name)) != VFS_OK)
            continue;
        for (k = 0; k < nprobe; k++)
        {
            int rc = vfs_mount(probe[k], mp, name);

            if (rc == VFS_OK)
            {
                fput_string("[MOUNT] %s -> %s driver='%s'\n", name, mp, probe[k]);
                return VFS_OK;
            }
            if (rc != VFS_ERR_NODEV && rc != VFS_ERR_NOTSUP)
                fput_string("[MOUNT] probe %s with '%s' rc=%d\n", name, probe[k], rc);
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
    if (vfs_register_driver(&ext4_driver) != VFS_OK)
        put_string("[MOUNT] register ext4 failed\n");

    /* 固定挂载：类型写死，不探测 */
    if (vfs_mount("devfs", "/dev", "devfs") != VFS_OK)
        put_string("[MOUNT] mount /dev failed\n");
    if (vfs_mount("ramfs", "/", "ramfs") != VFS_OK)
        put_string("[MOUNT] FATAL: root mount failed\n");

    /* 数据分区：自动探测（首个 FAT32 卷 -> /fat，首个 EXT4 卷 -> /ext4） */
    mount_auto("/fat", g_probe_fat, (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0])));
    mount_auto("/ext4", g_probe_ext, (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0])));
}

void test_ext4_read()
{
    vfs_file_t *f = NULL;
    char buf[1024];
    int64_t n;
    int rc;

    rc = vfs_open("/ext4/usr/local/bin/gcc", VFS_O_RDONLY, &f);
    fput_string("[TEST] open '/ext4/usr/local/bin/gcc' rc=%d %s\n",
                rc, rc == VFS_OK ? "OK" : "FAIL");
    if (rc != VFS_OK)
        return;

    n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    if (n < 0)
    {
        fput_string("[TEST] gcc read failed rc=%lld\n", (long long)n);
        return;
    }
    buf[n] = '\0';
    fput_string("[TEST] gcc content (%lld bytes):\n%s\n", (long long)n, buf);
}
