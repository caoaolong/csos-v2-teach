#include <fs/vfs.h>
#include <memory/heap.h>
#include <spinlock.h>
#include <serial.h>
#include <string.h>

static const fs_driver_t *g_drivers[VFS_MAX_DRIVERS];
static int g_driver_count;
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static char g_cwd[VFS_PATH_MAX];
static spinlock_t g_vfs_lock = SPINLOCK_INIT;
static int g_vfs_ready;

/* 规范化绝对路径：合并重复 '/'，处理 '.' 与 '..'，根为 "/" */
static int vfs_normalize(const char *src, char *dst, uint32_t dst_size)
{
    uint32_t i = 0;
    uint32_t j = 1;
    uint32_t start, len, need;

    if (!src || !dst || dst_size < 2 || src[0] != '/')
        return VFS_ERR_INVAL;
    dst[0] = '/';
    while (src[i])
    {
        while (src[i] == '/')
            i++;
        if (src[i] == '\0')
            break;
        start = i;
        while (src[i] && src[i] != '/')
            i++;
        len = i - start;
        if (len == 1 && src[start] == '.')
            continue;
        if (len == 2 && src[start] == '.' && src[start + 1] == '.')
        {
            /* 弹出最后一级；根目录上的 ".." 停在根 */
            while (j > 1 && dst[j - 1] != '/')
                j--;
            if (j > 1)
                j--;
            continue;
        }
        need = j + (j > 1 ? 1 : 0) + len;
        if (need >= dst_size)
            return VFS_ERR_INVAL;
        if (j > 1)
            dst[j++] = '/';
        kernel_memcpy(dst + j, (void *)(src + start), len);
        j += len;
    }
    dst[j] = '\0';
    return VFS_OK;
}

/* 相对路径基于当前工作目录拼接后再规范化 */
static int vfs_abspath(const char *path, char *out, uint32_t out_size)
{
    char joined[VFS_PATH_MAX];
    char cwd[VFS_PATH_MAX];
    uint64_t irq;
    uint32_t cl, pl;

    if (!path || path[0] == '\0')
        return VFS_ERR_INVAL;
    if (path[0] == '/')
        return vfs_normalize(path, out, out_size);

    irq = spin_lock_irqsave(&g_vfs_lock);
    kernel_strncpy(cwd, g_cwd, sizeof(cwd));
    spin_unlock_irqrestore(&g_vfs_lock, irq);

    cl = kernel_strlen(cwd);
    pl = kernel_strlen(path);
    if (cl + 1 + pl >= sizeof(joined))
        return VFS_ERR_INVAL;
    kernel_memcpy(joined, cwd, cl);
    joined[cl] = '/';
    kernel_memcpy(joined + cl + 1, (void *)path, pl);
    joined[cl + 1 + pl] = '\0';
    return vfs_normalize(joined, out, out_size);
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

static int vfs_locate(const char *path, char *norm, uint32_t norm_size,
                      vfs_mount_t **m_out, const char **rel_out)
{
    uint64_t irq;
    int rc = vfs_abspath(path, norm, norm_size);

    if (rc != VFS_OK)
        return rc;
    irq = spin_lock_irqsave(&g_vfs_lock);
    *m_out = vfs_resolve(norm, rel_out);
    if (*m_out)
        (*m_out)->open_count++;
    spin_unlock_irqrestore(&g_vfs_lock, irq);
    return *m_out ? VFS_OK : VFS_ERR_NOENT;
}

static void vfs_unpin(vfs_mount_t *m)
{
    uint64_t irq = spin_lock_irqsave(&g_vfs_lock);
    m->open_count--;
    spin_unlock_irqrestore(&g_vfs_lock, irq);
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
    kernel_strncpy(g_cwd, "/", sizeof(g_cwd));
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
    if (vfs_normalize(mountpoint, norm, sizeof(norm)) != VFS_OK)
        return VFS_ERR_INVAL;

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
    if (vfs_normalize(mountpoint, norm, sizeof(norm)) != VFS_OK)
        return VFS_ERR_INVAL;

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
    if (g_mounts[i].open_count > 0)
    {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return VFS_ERR_BUSY; /* 仍有句柄，禁止释放 superblock */
    }
    g_mounts[i].used = 0; /* 摘除：之后 vfs_locate 无法再找到它，不会有新句柄 */
    spin_unlock_irqrestore(&g_vfs_lock, flags);

    if (g_mounts[i].drv->unmount)
    {
        rc = g_mounts[i].drv->unmount(&g_mounts[i]);
        if (rc != VFS_OK)
        {
            flags = spin_lock_irqsave(&g_vfs_lock);
            g_mounts[i].used = 1;
            spin_unlock_irqrestore(&g_vfs_lock, flags);
            return rc;
        }
    }
    flags = spin_lock_irqsave(&g_vfs_lock);
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

    if (!g_vfs_ready || !path || !out)
        return VFS_ERR_INVAL;
    rc = vfs_locate(path, norm, sizeof(norm), &m, &rel);
    if (rc != VFS_OK)
        return rc;

    f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!f)
    {
        vfs_unpin(m);
        return VFS_ERR_NOMEM;
    }
    kernel_memset(f, 0, (uint32_t)sizeof(*f));
    f->mnt = m;
    f->drv = m->drv;
    f->flags = flags;
    f->pos = 0;

    rc = m->drv->open(m, rel, flags, f);
    if (rc != VFS_OK)
    {
        kfree(f);
        vfs_unpin(m);
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
    vfs_unpin(f->mnt);
    kfree(f);
    return rc;
}

int64_t vfs_read(vfs_file_t *f, void *buf, uint64_t len)
{
    if (!f || (!buf && len))
        return VFS_ERR_INVAL;
    if (f->is_dir)
        return VFS_ERR_ISDIR;
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
    if (f->is_dir)
        return VFS_ERR_ISDIR;
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
    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!f->drv->seek)
        return VFS_ERR_NOTSUP;
    return f->drv->seek(f, off, whence);
}

int vfs_stat(const char *path, uint64_t *size_out, uint32_t *is_dir_out)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;
    int rc;

    if (!g_vfs_ready || !path)
        return VFS_ERR_INVAL;
    rc = vfs_locate(path, norm, sizeof(norm), &m, &rel);
    if (rc != VFS_OK)
        return rc;
    if (!m->drv->stat)
    {
        vfs_unpin(m);
        return VFS_ERR_NOTSUP;
    }
    rc = m->drv->stat(m, rel, size_out, is_dir_out);
    vfs_unpin(m);
    return rc;
}

int vfs_mkdir(const char *path)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;
    int rc;

    if (!g_vfs_ready || !path)
        return VFS_ERR_INVAL;
    rc = vfs_locate(path, norm, sizeof(norm), &m, &rel);
    if (rc != VFS_OK)
        return rc;
    if (rel[0] == '\0')
    {
        vfs_unpin(m);
        return VFS_ERR_EXIST; /* 挂载点本身已存在 */
    }
    if (!m->drv->mkdir)
    {
        vfs_unpin(m);
        return VFS_ERR_NOTSUP;
    }
    rc = m->drv->mkdir(m, rel);
    vfs_unpin(m);
    return rc;
}

int vfs_rmdir(const char *path)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;
    int rc;

    if (!g_vfs_ready || !path)
        return VFS_ERR_INVAL;
    rc = vfs_locate(path, norm, sizeof(norm), &m, &rel);
    if (rc != VFS_OK)
        return rc;
    if (rel[0] == '\0')
    {
        vfs_unpin(m);
        return VFS_ERR_BUSY; /* 挂载点根不可删除，需先 unmount */
    }
    if (!m->drv->rmdir)
    {
        vfs_unpin(m);
        return VFS_ERR_NOTSUP;
    }
    rc = m->drv->rmdir(m, rel);
    vfs_unpin(m);
    return rc;
}

int vfs_opendir(const char *path, vfs_dir_t **out)
{
    char norm[VFS_PATH_MAX];
    vfs_mount_t *m;
    const char *rel;
    vfs_dir_t *d;
    int rc;

    if (!g_vfs_ready || !path || !out)
        return VFS_ERR_INVAL;
    rc = vfs_locate(path, norm, sizeof(norm), &m, &rel);
    if (rc != VFS_OK)
        return rc;
    if (!m->drv->opendir || !m->drv->readdir || !m->drv->closedir)
    {
        vfs_unpin(m);
        return VFS_ERR_NOTSUP;
    }

    d = (vfs_dir_t *)kmalloc(sizeof(vfs_dir_t));
    if (!d)
    {
        vfs_unpin(m);
        return VFS_ERR_NOMEM;
    }
    kernel_memset(d, 0, (uint32_t)sizeof(*d));
    d->mnt = m;
    d->drv = m->drv;

    rc = m->drv->opendir(m, rel, d);
    if (rc != VFS_OK)
    {
        kfree(d);
        vfs_unpin(m);
        return rc;
    }
    *out = d;
    return VFS_OK;
}

int vfs_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    if (!d || !out)
        return VFS_ERR_INVAL;
    return d->drv->readdir(d, out);
}

int vfs_closedir(vfs_dir_t *d)
{
    int rc;
    if (!d)
        return VFS_ERR_INVAL;
    rc = d->drv->closedir(d);
    vfs_unpin(d->mnt);
    kfree(d);
    return rc;
}

int vfs_chdir(const char *path)
{
    char norm[VFS_PATH_MAX];
    uint64_t size = 0;
    uint32_t is_dir = 0;
    uint64_t irq;
    int rc;

    if (!g_vfs_ready || !path)
        return VFS_ERR_INVAL;
    rc = vfs_abspath(path, norm, sizeof(norm));
    if (rc != VFS_OK)
        return rc;
    rc = vfs_stat(norm, &size, &is_dir);
    if (rc != VFS_OK)
        return rc;
    if (!is_dir)
        return VFS_ERR_NOTDIR;

    irq = spin_lock_irqsave(&g_vfs_lock);
    kernel_strncpy(g_cwd, norm, sizeof(g_cwd));
    spin_unlock_irqrestore(&g_vfs_lock, irq);
    return VFS_OK;
}

int vfs_getcwd(char *buf, uint32_t size)
{
    uint64_t irq;
    uint32_t len;

    if (!g_vfs_ready || !buf || size == 0)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&g_vfs_lock);
    len = kernel_strlen(g_cwd);
    if (len + 1 > size)
    {
        spin_unlock_irqrestore(&g_vfs_lock, irq);
        return VFS_ERR_RANGE;
    }
    kernel_strncpy(buf, g_cwd, size);
    spin_unlock_irqrestore(&g_vfs_lock, irq);
    return VFS_OK;
}
