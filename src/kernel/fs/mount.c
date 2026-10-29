#include <fs/mount.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <fs/fat32.h>
#include <fs/ext4.h>
#include <part/part.h>
#include <config.h>
#include <serial.h>
#include <string.h>

static const char *const g_probe_fat[] = {
    "fat32",
};

static const char *const g_probe_ext[] = {
    "ext4",
};

/* 在指定设备上按序尝试各驱动（fstype=auto 用） */
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

/* 无 csos.cfg 时的旧行为：ramfs 根 + 自动探测 */
static void init_mounts_legacy(void)
{
    if (vfs_mount("ramfs", "/", "ramfs") != VFS_OK)
        put_string("[MOUNT] FATAL: root mount failed\n");

    /* 数据分区：自动探测（首个 FAT32 卷 -> /fat，首个 EXT4 卷 -> /ext4） */
    mount_auto("/fat", g_probe_fat, (int)(sizeof(g_probe_fat) / sizeof(g_probe_fat[0])));
    mount_auto("/ext4", g_probe_ext, (int)(sizeof(g_probe_ext) / sizeof(g_probe_ext[0])));
}

/* 有 csos.cfg 时：按 mount 行挂载，root= 兜底 /，失败回退 ramfs / */
static void init_mounts_config(void)
{
    int i;
    int root_done = 0;

    /* 第一遍：挂载点为 / 的行（配置文件顺序） */
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
        break; /* 只取第一条 / 行 */
    }

    /* root= 兜底：在指定设备上探测 fstype 挂为 / */
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

    /* 第二遍：其余挂载点（保持文件顺序） */
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

    /* 固定挂载：类型写死，不探测 */
    if (vfs_mount("devfs", "/dev", "devfs") != VFS_OK)
        put_string("[MOUNT] mount /dev failed\n");

    if (g_boot_config.present)
        init_mounts_config();
    else
        init_mounts_legacy();
}
