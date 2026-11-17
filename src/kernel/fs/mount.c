#include <kernel/fs/mount.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/ramfs.h>
#include <kernel/fs/pipefs.h>
#include <kernel/fs/fat32.h>
#include <kernel/fs/ext4.h>
#include <kernel/part/part.h>
#include <kernel/config.h>
#include <kernel/serial.h>
#include <kernel/string.h>

static const char *const g_probe_fat[] = {
    "fat32",
};

static const char *const g_probe_ext[] = {
    "ext4",
};

/* 在设备上按序尝试驱动 */
static int mount_dev_probe(const char *mp, const char *dev,
                           const char *const *probe, int nprobe)
{
    int k;

    for (k = 0; k < nprobe; k++)
    {
        int rc = vfs_mount(probe[k], mp, dev);

        if (rc == VFS_OK)
        {
            fput_string("[MOUNT] %s -> %s driver='%s'\n", dev, mp, probe[k]);
            return VFS_OK;
        }
        if (rc != VFS_ERR_NODEV && rc != VFS_ERR_NOTSUP)
            fput_string("[MOUNT] probe %s with '%s' rc=%d\n", dev, probe[k], rc);
    }
    return VFS_ERR_NODEV;
}

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

/* 无配置时挂 ramfs 根并自动探测 */
static void init_mounts_legacy(void)
{
    if (vfs_mount("ramfs", "/", "ramfs") != VFS_OK)
        put_string("[MOUNT] FATAL: root mount failed\n");

    /* 自动探测数据分区 */
    mount_auto("/fat", g_probe_fat, (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0])));
    mount_auto("/ext4", g_probe_ext, (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0])));
}

/* 有配置时按表挂载 */
static void init_mounts_config(void)
{
    int i;
    int root_done = 0;

    /* 先挂载根目录 */
    for (i = 0; i < g_boot_config.mount_count; i++)
    {
        config_mount_t *m = &g_boot_config.mounts[i];
        int rc;

        if (kernel_strcmp(m->mountpoint, "/") != 0)
            continue;
        if (kernel_strcmp(m->fstype, "auto") == 0)
        {
            if (mount_dev_probe("/", m->dev, g_probe_fat,
                                (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0]))) == VFS_OK ||
                mount_dev_probe("/", m->dev, g_probe_ext,
                                (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0]))) == VFS_OK)
                root_done = 1;
            else
                fput_string("[MOUNT] root %s (auto) failed\n", m->dev);
        }
        else
        {
            rc = vfs_mount(m->fstype, "/", m->dev);
            if (rc == VFS_OK)
            {
                fput_string("[MOUNT] %s -> %s driver='%s'\n", m->dev, m->mountpoint, m->fstype);
                root_done = 1;
            }
            else
                fput_string("[MOUNT] root %s (%s) failed rc=%d\n", m->dev, m->fstype, rc);
        }
        break; /* 只取第一条根记录 */
    }

    /* 用 root 参数兜底根目录 */
    if (!root_done && g_boot_config.root[0] != '\0')
    {
        if (mount_dev_probe("/", g_boot_config.root, g_probe_fat,
                            (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0]))) == VFS_OK ||
            mount_dev_probe("/", g_boot_config.root, g_probe_ext,
                            (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0]))) == VFS_OK)
            root_done = 1;
    }

    if (!root_done)
    {
        if (vfs_mount("ramfs", "/", "ramfs") != VFS_OK)
            put_string("[MOUNT] FATAL: root mount failed\n");
    }

    /* 再挂载其余挂载点 */
    for (i = 0; i < g_boot_config.mount_count; i++)
    {
        config_mount_t *m = &g_boot_config.mounts[i];
        int rc;

        if (kernel_strcmp(m->mountpoint, "/") == 0)
            continue;
        if (kernel_strcmp(m->fstype, "auto") == 0)
        {
            if (mount_dev_probe(m->mountpoint, m->dev, g_probe_fat,
                                (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0]))) == VFS_OK)
                continue;
            rc = mount_dev_probe(m->mountpoint, m->dev, g_probe_ext,
                                 (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0])));
        }
        else
        {
            rc = vfs_mount(m->fstype, m->mountpoint, m->dev);
            if (rc == VFS_OK)
                fput_string("[MOUNT] %s -> %s driver='%s'\n", m->dev, m->mountpoint, m->fstype);
        }
        if (rc != VFS_OK)
            fput_string("[MOUNT] %s -> %s (%s) failed rc=%d, skip\n",
                        m->dev, m->mountpoint, m->fstype, rc);
    }
}

void init_mounts()
{
    if (vfs_register_driver(&ramfs_driver) != VFS_OK)
        put_string("[MOUNT] register ramfs failed\n");
    if (vfs_register_driver(&fat32_driver) != VFS_OK)
        put_string("[MOUNT] register fat32 failed\n");
    if (vfs_register_driver(&ext4_driver) != VFS_OK)
        put_string("[MOUNT] register ext4 failed\n");
    if (vfs_register_driver(&pipefs_driver) != VFS_OK)
        put_string("[MOUNT] register pipefs failed\n");

    /* 固定挂载，不探测 */
    if (vfs_mount("devfs", "/dev", "devfs") != VFS_OK)
        put_string("[MOUNT] mount /dev failed\n");
    if (vfs_mount("pipefs", "/pipe", "pipefs") != VFS_OK)
        put_string("[MOUNT] mount /pipe failed\n");

    if (g_boot_config.present)
        init_mounts_config();
    else
        init_mounts_legacy();
}
