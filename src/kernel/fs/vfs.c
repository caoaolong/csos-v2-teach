#include <fs/vfs.h>
#include <memory/heap.h>
#include <spinlock.h>
#include <serial.h>
#include <string.h>

static const fs_driver_t *g_drivers[VFS_MAX_DRIVERS];
static int g_driver_count;
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static spinlock_t g_vfs_lock = SPINLOCK_INIT;
static int g_vfs_ready;

/* 路径规范化：去重复 '/'，去末尾 '/'（根 "/" 保留） */
static void vfs_normalize(const char *src, char *dst, uint32_t dst_size)
{
    uint32_t j = 0;
    int prev_slash = 0;
    uint32_t i = 0;

    if (!src || !dst || !dst_size)
        return;
    if (src[0] != '/')
    {
        kernel_strncpy(dst, "/", dst_size);
        return;
    }
    dst[j++] = '/';
    prev_slash = 1;
    for (i = 1; src[i] && j + 1 < dst_size; i++)
    {
        if (src[i] == '/')
        {
            if (prev_slash)
                continue;
            prev_slash = 1;
            dst[j++] = '/';
        }
        else
        {
            prev_slash = 0;
            dst[j++] = src[i];
        }
    }
    /* 去末尾 '/' */
    while (j > 1 && dst[j - 1] == '/')
        j--;
    dst[j] = '\0';
}

/* 最长前缀匹配：返回挂载点，否则 NULL；rel 输出去掉前缀后的相对路径 */
static vfs_mount_t *vfs_resolve(const char *path, const char **rel_out)
{
    vfs_mount_t *best = NULL;
    uint32_t best_len = 0;
    int i;

    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        uint32_t ml;
        const char *mp;
        if (!g_mounts[i].used)
            continue;
        mp = g_mounts[i].mountpoint;
        ml = kernel_strlen(mp);
        if (ml == 1)
        {
            /* "/" 兜底所有绝对路径 */
            if (best == NULL)
            {
                best = &g_mounts[i];
                best_len = ml;
            }
            continue;
        }
        if (path[0] != mp[0])
            continue;
        /* 前缀相等且边界为 '/' 或字符串结束 */
        if (kernel_strncmp(mp, path, ml) != 0)
            continue;
        if (path[ml] != '\0' && path[ml] != '/')
            continue;
        if (ml > best_len)
        {
            best = &g_mounts[i];
            best_len = ml;
        }
    }
    if (!best)
        return NULL;
    if (best_len == 1)
        *rel_out = path + 1; /* "/" -> rel 为 path 去首 '/' */
    else
        *rel_out = path[best_len] == '/' ? path + best_len + 1 : path + best_len;
    return best;
}

void init_vfs()
{
    int i;
    uint64_t flags = spin_lock_irqsave(&g_vfs_lock);
    for (i = 0; i < VFS_MAX_DRIVERS; i++)
        g_drivers[i] = NULL;
    g_driver_count = 0;
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        g_mounts[i].used = 0;
        g_mounts[i].drv = NULL;
        g_mounts[i].priv = NULL;
        g_mounts[i].mountpoint[0] = '\0';
        g_mounts[i].dev[0] = '\0';
    }
    g_vfs_ready = 1;
    spin_unlock_irqrestore(&g_vfs_lock, flags);
    fput_string("[VFS] init facade+strategy ready\n");
}

int vfs_register_driver(const fs_driver_t *drv)
{
    uint64_t flags;
    int i;
    if (!g_vfs_ready || !drv || !drv->name || !drv->mount || !drv->open)
        return VFS_ERR_INVAL;
    flags = spin_lock_irqsave(&g_vfs_lock);
    for (i = 0; i < g_driver_count; i++)
    {
        if (kernel_strcmp(g_drivers[i]->name, drv->name) == 0)
        {
            spin_unlock_irqrestore(&g_vfs_lock, flags);
            return VFS_ERR_EXIST;
        }
    }
    if (g_driver_count >= VFS_MAX_DRIVERS)
    {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return VFS_ERR_NOSPC;
    }
    g_drivers[g_driver_count++] = drv;
    spin_unlock_irqrestore(&g_vfs_lock, flags);
    fput_string("[VFS] register driver='%s'\n", drv->name);
    return VFS_OK;
}

static const fs_driver_t *vfs_find_driver(const char *name)
{
    int i;
    for (i = 0; i < g_driver_count; i++)
        if (kernel_strcmp(g_drivers[i]->name, name) == 0)
            return g_drivers[i];
    return NULL;
}

int vfs_mount(const char *fs_name, const char *mountpoint, const char *dev)
{
    char norm[VFS_PATH_MAX];
    const fs_driver_t *drv;
    vfs_mount_t *slot = NULL;
    uint64_t flags;
    int i, rc;

    if (!g_vfs_ready || !fs_name || !mountpoint || mountpoint[0] != '/')
        return VFS_ERR_INVAL;
    vfs_normalize(mountpoint, norm, sizeof(norm));

    flags = spin_lock_irqsave(&g_vfs_lock);
    drv = vfs_find_driver(fs_name);
    if (!drv)
    {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return VFS_ERR_NODEV;
    }
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && kernel_strcmp(g_mounts[i].mountpoint, norm) == 0)
        {
            spin_unlock_irqrestore(&g_vfs_lock, flags);
            return VFS_ERR_BUSY;
        }
    }
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!g_mounts[i].used)
        {
            slot = &g_mounts[i];
            break;
        }
    }
    if (!slot)
    {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return VFS_ERR_NOSPC;
    }
    slot->used = 1; /* 先占位，mount 回调失败再回收 */
    slot->drv = drv;
    slot->priv = NULL;
    kernel_strncpy(slot->mountpoint, norm, sizeof(slot->mountpoint));
    kernel_strncpy(slot->dev, dev ? dev : "", sizeof(slot->dev));
    spin_unlock_irqrestore(&g_vfs_lock, flags);

    rc = drv->mount(slot, dev ? dev : "");
    if (rc != VFS_OK)
    {
        flags = spin_lock_irqsave(&g_vfs_lock);
        slot->used = 0;
        slot->drv = NULL;
        slot->priv = NULL;
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return rc;
    }
    fput_string("[VFS] mount %s -> %s driver='%s'\n", slot->dev, slot->mountpoint, fs_name);
    return VFS_OK;
}

int vfs_unmount(const char *mountpoint)
{
    char norm[VFS_PATH_MAX];
    uint64_t flags;
    int i, rc;

    if (!g_vfs_ready || !mountpoint)
        return VFS_ERR_INVAL;
    vfs_normalize(mountpoint, norm, sizeof(norm));

    flags = spin_lock_irqsave(&g_vfs_lock);
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && kernel_strcmp(g_mounts[i].mountpoint, norm) == 0)
            break;
    }
    if (i >= VFS_MAX_MOUNTS)
    {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return VFS_ERR_NOENT;
    }
    spin_unlock_irqrestore(&g_vfs_lock, flags);

    if (g_mounts[i].drv->unmount)
    {
        rc = g_mounts[i].drv->unmount(&g_mounts[i]);
        if (rc != VFS_OK)
            return rc;
    }
    flags = spin_lock_irqsave(&g_vfs_lock);
    g_mounts[i].used = 0;
    g_mounts[i].drv = NULL;
    g_mounts[i].priv = NULL;
    spin_unlock_irqrestore(&g_vfs_lock, flags);
    return VFS_OK;
}

int vfs_open(const char *path, int flags, vfs_file_t **out)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;
    vfs_file_t *f;
    int rc;

    if (!g_vfs_ready || !path || path[0] != '/' || !out)
        return VFS_ERR_INVAL;
    vfs_normalize(path, norm, sizeof(norm));

    m = vfs_resolve(norm, &rel);
    if (!m)
        return VFS_ERR_NOENT;

    f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!f)
        return VFS_ERR_NOMEM;
    kernel_memset(f, 0, (uint32_t)sizeof(*f));
    f->mnt = m;
    f->drv = m->drv;
    f->flags = flags;
    f->pos = 0;

    rc = m->drv->open(m, rel, flags, f);
    if (rc != VFS_OK)
    {
        kfree(f);
        return rc;
    }
    if ((flags & VFS_O_APPEND) && f->drv->seek)
        f->drv->seek(f, 0, VFS_SEEK_END);
    *out = f;
    return VFS_OK;
}

int vfs_close(vfs_file_t *f)
{
    int rc;
    if (!f)
        return VFS_ERR_INVAL;
    rc = f->drv->close ? f->drv->close(f) : VFS_OK;
    kfree(f);
    return rc;
}

int64_t vfs_read(vfs_file_t *f, void *buf, uint64_t len)
{
    if (!f || (!buf && len))
        return VFS_ERR_INVAL;
    if ((f->flags & 0x03) == VFS_O_WRONLY)
        return VFS_ERR_INVAL;
    if (!f->drv->read)
        return VFS_ERR_NOTSUP;
    return f->drv->read(f, buf, len);
}

int64_t vfs_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    if (!f || (!buf && len))
        return VFS_ERR_INVAL;
    if ((f->flags & 0x03) == VFS_O_RDONLY)
        return VFS_ERR_INVAL;
    if (!f->drv->write)
        return VFS_ERR_NOTSUP;
    return f->drv->write(f, buf, len);
}

int64_t vfs_seek(vfs_file_t *f, int64_t off, int whence)
{
    if (!f || (whence != VFS_SEEK_SET && whence != VFS_SEEK_CUR && whence != VFS_SEEK_END))
        return VFS_ERR_INVAL;
    if (!f->drv->seek)
        return VFS_ERR_NOTSUP;
    return f->drv->seek(f, off, whence);
}

int vfs_stat(const char *path, uint64_t *size_out, uint32_t *is_dir_out)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;

    if (!g_vfs_ready || !path || path[0] != '/')
        return VFS_ERR_INVAL;
    vfs_normalize(path, norm, sizeof(norm));
    m = vfs_resolve(norm, &rel);
    if (!m)
        return VFS_ERR_NOENT;
    if (!m->drv->stat)
        return VFS_ERR_NOTSUP;
    return m->drv->stat(m, rel, size_out, is_dir_out);
}
