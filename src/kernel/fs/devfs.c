#include <fs/devfs.h>
#include <vdev.h>
#include <ata/ata.h>
#include <memory/heap.h>
#include <serial.h>
#include <spinlock.h>
#include <string.h>

typedef struct devfs_node
{
    char name[VFS_NAME_MAX];
    uint32_t is_dir;
    uint32_t refs; /* 打开句柄 + 目录游标；>0 时禁止 detach */
    vdev_t *dev;
    struct devfs_node *parent;
    struct devfs_node *child;
    struct devfs_node *sibling;
} devfs_node_t;

typedef struct devfs_sb
{
    spinlock_t lock;
    devfs_node_t *root;
    vdev_listener_t listener;
} devfs_sb_t;

static int devfs_name_eq(const char *name, const char *comp, uint32_t len)
{
    return kernel_strlen(name) == len && kernel_memcmp((void *)name, (void *)comp, len) == 0;
}

static devfs_node_t *devfs_child_find(devfs_node_t *dir, const char *comp, uint32_t len)
{
    devfs_node_t *c;

    for (c = dir->child; c; c = c->sibling)
        if (devfs_name_eq(c->name, comp, len))
            return c;
    return NULL;
}

/* 按名字升序插入，保证节点顺序稳定 */
static void devfs_child_link(devfs_node_t *dir, devfs_node_t *node)
{
    devfs_node_t **pp = &dir->child;

    while (*pp && kernel_strcmp((*pp)->name, node->name) < 0)
        pp = &(*pp)->sibling;
    node->sibling = *pp;
    *pp = node;
    node->parent = dir;
}

static void devfs_child_unlink(devfs_node_t *node)
{
    devfs_node_t **pp = &node->parent->child;

    while (*pp && *pp != node)
        pp = &(*pp)->sibling;
    if (*pp == node)
        *pp = node->sibling;
    node->sibling = NULL;
    node->parent = NULL;
}

static devfs_node_t *devfs_new_node(const char *name, uint32_t len, uint32_t is_dir)
{
    devfs_node_t *n = (devfs_node_t *)kmalloc(sizeof(devfs_node_t));

    if (!n)
        return NULL;
    kernel_memset(n, 0, (uint32_t)sizeof(*n));
    kernel_memcpy(n->name, (void *)name, len);
    n->name[len] = '\0';
    n->is_dir = is_dir;
    return n;
}

static int devfs_attach_locked(devfs_sb_t *sb, vdev_t *dev)
{
    const char *p = dev->name;
    devfs_node_t *cur = sb->root;
    char comp[VFS_NAME_MAX];

    if (dev->name[0] == '\0')
        return VFS_ERR_INVAL;
    for (;;)
    {
        uint32_t n = 0;
        devfs_node_t *child;
        int last;

        while (*p == '/')
            p++;
        if (!*p)
            return VFS_ERR_INVAL;
        while (*p && *p != '/')
        {
            if (n + 1 >= sizeof(comp))
                return VFS_ERR_INVAL;
            comp[n++] = *p++;
        }
        comp[n] = '\0';
        last = (*p == '\0');

        child = devfs_child_find(cur, comp, n);
        if (!child)
        {
            child = devfs_new_node(comp, n, last ? 0 : 1);
            if (!child)
                return VFS_ERR_NOMEM;
            devfs_child_link(cur, child);
        }
        if (last)
        {
            if (child->is_dir || child->dev)
                return VFS_ERR_EXIST;
            child->dev = dev;
            return VFS_OK;
        }
        if (!child->is_dir)
            return VFS_ERR_NOTDIR;
        cur = child;
    }
}

/* 调用者已持锁：按设备名查找节点，找不到返回 NULL */
static devfs_node_t *devfs_find_locked(devfs_sb_t *sb, const char *name)
{
    const char *p = name;
    devfs_node_t *cur = sb->root;

    for (;;)
    {
        const char *start;
        uint32_t len;
        devfs_node_t *c;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != '/')
            p++;
        len = (uint32_t)(p - start);
        if (!cur->is_dir)
            return NULL;
        c = devfs_child_find(cur, start, len);
        if (!c)
            return NULL;
        cur = c;
    }
    return cur;
}

/* 调用者已持锁：摘除设备节点，并向上清理变空的目录 */
static int devfs_detach_locked(devfs_sb_t *sb, vdev_t *dev)
{
    devfs_node_t *n = devfs_find_locked(sb, dev->name);
    devfs_node_t *parent;

    if (!n || n->dev != dev)
        return VFS_ERR_NOENT;
    if (n->refs > 0)
        return VFS_ERR_BUSY; /* 仍有 open 句柄或目录游标引用 */

    parent = n->parent;
    devfs_child_unlink(n);
    kfree(n);

    while (parent && parent != sb->root && !parent->child && !parent->dev && parent->refs == 0)
    {
        devfs_node_t *up = parent->parent;
        devfs_child_unlink(parent);
        kfree(parent);
        parent = up;
    }
    return VFS_OK;
}

static int devfs_on_added(void *ctx, vdev_t *dev)
{
    devfs_sb_t *sb = (devfs_sb_t *)ctx;
    uint64_t flags = spin_lock_irqsave(&sb->lock);
    int rc = devfs_attach_locked(sb, dev);

    spin_unlock_irqrestore(&sb->lock, flags);
    return rc;
}

static int devfs_on_removed(void *ctx, vdev_t *dev)
{
    devfs_sb_t *sb = (devfs_sb_t *)ctx;
    uint64_t flags = spin_lock_irqsave(&sb->lock);
    int rc = devfs_detach_locked(sb, dev);

    spin_unlock_irqrestore(&sb->lock, flags);
    return rc;
}

static void devfs_free_tree(devfs_node_t *n)
{
    devfs_node_t *c = n->child;

    while (c)
    {
        devfs_node_t *next = c->sibling;
        devfs_free_tree(c);
        c = next;
    }
    kfree(n);
}

static int devfs_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    devfs_sb_t *sb = (devfs_sb_t *)m->priv;
    devfs_node_t *n;
    vdev_t *dev;
    uint64_t irq;
    int want_dir;
    int rc = VFS_OK;

    if (!sb || !rel || !f)
        return VFS_ERR_INVAL;
    if (flags & (VFS_O_CREAT | VFS_O_TRUNC))
        return VFS_ERR_NOTSUP; /* 设备节点只来自 vdev_register，不可新建/截断 */
    want_dir = (flags & VFS_O_DIRECTORY) != 0;

    irq = spin_lock_irqsave(&sb->lock);
    n = devfs_find_locked(sb, rel);
    if (!n)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT;
    }
    if (n->is_dir)
    {
        if (!want_dir)
        {
            spin_unlock_irqrestore(&sb->lock, irq);
            return VFS_ERR_ISDIR;
        }
        n->refs++;
        f->priv = n;
        f->is_dir = 1;
        f->size = 0;
        f->pos = 0;
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_OK;
    }
    if (want_dir)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOTDIR;
    }
    if (!n->dev)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT;
    }
    n->refs++; /* 钉住节点：detach 见 refs>0 会返回 BUSY，节点不会被释放 */
    f->priv = n;
    f->is_dir = 0;
    f->size = 0;
    f->pos = 0;
    dev = n->dev;
    if (dev->type == VDEV_BLOCK)
        f->size = dev->nblocks * dev->block_size;
    spin_unlock_irqrestore(&sb->lock, irq);

    if (dev->type == VDEV_CHAR)
    {
        const vdev_char_ops_t *ops = dev->ops;
        if (ops && ops->open)
        {
            rc = ops->open(dev, f);
            if (rc != VFS_OK)
            {
                irq = spin_lock_irqsave(&sb->lock);
                n->refs--;
                spin_unlock_irqrestore(&sb->lock, irq);
                f->priv = NULL;
            }
        }
    }
    else if (dev->type == VDEV_BLOCK)
    {
        const vdev_block_ops_t *bops = dev->bops;
        if (bops && bops->open)
        {
            rc = bops->open(dev, f);
            if (rc != VFS_OK)
            {
                irq = spin_lock_irqsave(&sb->lock);
                n->refs--;
                spin_unlock_irqrestore(&sb->lock, irq);
                f->priv = NULL;
            }
        }
    }
    else
    {
        irq = spin_lock_irqsave(&sb->lock);
        n->refs--;
        spin_unlock_irqrestore(&sb->lock, irq);
        f->priv = NULL;
        rc = VFS_ERR_NOTSUP;
    }
    return rc;
}

static int devfs_close(vfs_file_t *f)
{
    devfs_sb_t *sb;
    devfs_node_t *n = (devfs_node_t *)f->priv;
    vdev_t *dev;
    uint64_t irq;

    if (!n)
        return VFS_OK;
    sb = (devfs_sb_t *)f->mnt->priv;
    dev = n->dev; /* 节点被 refs 钉住，detach 无法释放它，dev 指针稳定 */
    if (dev && dev->type == VDEV_CHAR && dev->ops && dev->ops->close)
        dev->ops->close(dev, f);
    else if (dev && dev->type == VDEV_BLOCK && dev->bops && dev->bops->close)
        dev->bops->close(dev, f);
    if (sb)
    {
        irq = spin_lock_irqsave(&sb->lock);
        n->refs--;
        spin_unlock_irqrestore(&sb->lock, irq);
    }
    f->priv = NULL;
    return VFS_OK;
}

static int64_t devfs_block_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    uint64_t bs = dev->block_size;
    uint64_t size = dev->nblocks * bs;
    uint8_t *p = (uint8_t *)buf;
    uint64_t done = 0;
    int rc;

    if (!dev->bops || !dev->bops->read_blocks)
        return VFS_ERR_NOTSUP;
    if (bs == 0)
        return VFS_ERR_INVAL;
    if (len == 0)
        return 0;
    if (f->pos >= size)
        return 0; /* EOF */
    if (len > size - f->pos)
        len = size - f->pos;

    /* 头部非对齐部分：读一整块再拷贝 */
    if (f->pos % bs)
    {
        uint64_t lba = f->pos / bs;
        uint64_t off = f->pos % bs;
        uint64_t chunk = bs - off;
        uint8_t *tmp;

        if (chunk > len)
            chunk = len;
        tmp = (uint8_t *)kmalloc((size_t)bs);
        if (!tmp)
            return VFS_ERR_NOMEM;
        rc = dev->bops->read_blocks(dev, lba, 1, tmp);
        if (rc != 0)
        {
            kfree(tmp);
            return done ? (int64_t)done : (int64_t)rc;
        }
        kernel_memcpy(p, tmp + off, (uint32_t)chunk);
        kfree(tmp);
        p += chunk;
        f->pos += chunk;
        done += chunk;
        len -= chunk;
        if (len == 0)
            return (int64_t)done;
    }

    /* 中间对齐部分：直接整块传输 */
    if (len >= bs)
    {
        uint64_t lba = f->pos / bs;
        uint32_t n = (uint32_t)(len / bs);
        /* 单次 ATA 调用块数过大时拆分，避免 count 溢出 */
        while (n > 0)
        {
            uint32_t cur = n > 128 ? 128 : n;
            rc = dev->bops->read_blocks(dev, lba, cur, p);
            if (rc != 0)
                return done ? (int64_t)done : (int64_t)rc;
            lba += cur;
            p += (uint64_t)cur * bs;
            f->pos += (uint64_t)cur * bs;
            done += (uint64_t)cur * bs;
            len -= (uint64_t)cur * bs;
            n -= cur;
        }
        if (len == 0)
            return (int64_t)done;
    }

    /* 尾部不足一块：读一整块再拷贝 */
    if (len > 0)
    {
        uint64_t lba = f->pos / bs;
        uint8_t *tmp = (uint8_t *)kmalloc((size_t)bs);

        if (!tmp)
            return done ? (int64_t)done : VFS_ERR_NOMEM;
        rc = dev->bops->read_blocks(dev, lba, 1, tmp);
        if (rc != 0)
        {
            kfree(tmp);
            return done ? (int64_t)done : (int64_t)rc;
        }
        kernel_memcpy(p, tmp, (uint32_t)len);
        kfree(tmp);
        f->pos += len;
        done += len;
    }
    return (int64_t)done;
}

static int64_t devfs_block_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    uint64_t bs = dev->block_size;
    uint64_t size = dev->nblocks * bs;
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t done = 0;
    int rc;

    if (!dev->bops || !dev->bops->write_blocks)
        return VFS_ERR_NOTSUP;
    if (bs == 0)
        return VFS_ERR_INVAL;
    if (len == 0)
        return 0;
    if (f->pos >= size)
        return VFS_ERR_NOSPC;
    if (len > size - f->pos)
        len = size - f->pos;

    /* 头部非对齐：读-改-写 */
    if (f->pos % bs)
    {
        uint64_t lba = f->pos / bs;
        uint64_t off = f->pos % bs;
        uint64_t chunk = bs - off;
        uint8_t *tmp;

        if (chunk > len)
            chunk = len;
        tmp = (uint8_t *)kmalloc((size_t)bs);
        if (!tmp)
            return VFS_ERR_NOMEM;
        rc = dev->bops->read_blocks(dev, lba, 1, tmp);
        if (rc != 0)
        {
            kfree(tmp);
            return (int64_t)rc;
        }
        kernel_memcpy(tmp + off, (void *)p, (uint32_t)chunk);
        rc = dev->bops->write_blocks(dev, lba, 1, tmp);
        kfree(tmp);
        if (rc != 0)
            return done ? (int64_t)done : (int64_t)rc;
        p += chunk;
        f->pos += chunk;
        done += chunk;
        len -= chunk;
        if (len == 0)
            goto out;
    }

    /* 中间对齐部分 */
    if (len >= bs)
    {
        uint64_t lba = f->pos / bs;
        uint32_t n = (uint32_t)(len / bs);

        while (n > 0)
        {
            uint32_t cur = n > 128 ? 128 : n;
            rc = dev->bops->write_blocks(dev, lba, cur, p);
            if (rc != 0)
                return done ? (int64_t)done : (int64_t)rc;
            lba += cur;
            p += (uint64_t)cur * bs;
            f->pos += (uint64_t)cur * bs;
            done += (uint64_t)cur * bs;
            len -= (uint64_t)cur * bs;
            n -= cur;
        }
        if (len == 0)
            goto out;
    }

    /* 尾部不足一块：读-改-写 */
    if (len > 0)
    {
        uint64_t lba = f->pos / bs;
        uint8_t *tmp = (uint8_t *)kmalloc((size_t)bs);

        if (!tmp)
            return done ? (int64_t)done : VFS_ERR_NOMEM;
        rc = dev->bops->read_blocks(dev, lba, 1, tmp);
        if (rc != 0)
        {
            kfree(tmp);
            return done ? (int64_t)done : (int64_t)rc;
        }
        kernel_memcpy(tmp, (void *)p, (uint32_t)len);
        rc = dev->bops->write_blocks(dev, lba, 1, tmp);
        kfree(tmp);
        if (rc != 0)
            return done ? (int64_t)done : (int64_t)rc;
        f->pos += len;
        done += len;
    }

out:
    if (done && dev->bops->flush)
        dev->bops->flush(dev);
    return (int64_t)done;
}

static int64_t devfs_read(vfs_file_t *f, void *buf, uint64_t len)
{
    devfs_node_t *n = (devfs_node_t *)f->priv;
    vdev_t *dev;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!n)
        return VFS_ERR_INVAL;
    dev = n->dev;
    if (!dev)
        return VFS_ERR_NOENT;
    if (dev->type == VDEV_BLOCK)
        return devfs_block_read(dev, f, buf, len);
    if (!dev->ops || !dev->ops->read)
        return VFS_ERR_NOTSUP;
    return dev->ops->read(dev, f, buf, len);
}

static int64_t devfs_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    devfs_node_t *n = (devfs_node_t *)f->priv;
    vdev_t *dev;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!n)
        return VFS_ERR_INVAL;
    dev = n->dev;
    if (!dev)
        return VFS_ERR_NOENT;
    if (dev->type == VDEV_BLOCK)
        return devfs_block_write(dev, f, buf, len);
    if (!dev->ops || !dev->ops->write)
        return VFS_ERR_NOTSUP;
    return dev->ops->write(dev, f, buf, len);
}

static int64_t devfs_seek(vfs_file_t *f, int64_t off, int whence)
{
    devfs_node_t *n = (devfs_node_t *)f->priv;
    vdev_t *dev;
    int64_t base;
    uint64_t size;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!n)
        return VFS_ERR_INVAL;
    dev = n->dev;
    if (!dev)
        return VFS_ERR_NOENT;
    if (dev->type != VDEV_BLOCK)
        return VFS_ERR_NOTSUP; /* 字符设备不可 seek */
    size = dev->nblocks * dev->block_size;
    if (whence == VFS_SEEK_SET)
        base = off;
    else if (whence == VFS_SEEK_CUR)
        base = (int64_t)f->pos + off;
    else
        base = (int64_t)size + off;
    if (base < 0 || (uint64_t)base > size)
        return VFS_ERR_INVAL;
    f->pos = (uint64_t)base;
    return (int64_t)f->pos;
}

static int devfs_stat(vfs_mount_t *m, const char *rel, uint64_t *size_out, uint32_t *is_dir_out)
{
    devfs_sb_t *sb = (devfs_sb_t *)m->priv;
    devfs_node_t *n;
    uint64_t irq;

    if (!sb || !rel)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    n = devfs_find_locked(sb, rel);
    if (!n)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT;
    }
    if (size_out)
    {
        if (!n->is_dir && n->dev && n->dev->type == VDEV_BLOCK)
            *size_out = n->dev->nblocks * n->dev->block_size;
        else
            *size_out = 0; /* 字符设备无固定大小 */
    }
    if (is_dir_out)
        *is_dir_out = n->is_dir;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int devfs_mkdir(vfs_mount_t *m, const char *rel)
{
    (void)m;
    (void)rel;
    return VFS_ERR_NOTSUP; /* 目录由设备名自动派生，不可手动创建 */
}

static int devfs_rmdir(vfs_mount_t *m, const char *rel)
{
    (void)m;
    (void)rel;
    return VFS_ERR_NOTSUP;
}

static int devfs_opendir(vfs_mount_t *m, const char *rel, vfs_dir_t *d)
{
    devfs_sb_t *sb = (devfs_sb_t *)m->priv;
    devfs_node_t *n;
    uint64_t irq;

    if (!sb || !rel || !d)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    n = devfs_find_locked(sb, rel);
    if (!n)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT;
    }
    if (!n->is_dir)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOTDIR;
    }
    n->refs++;
    d->priv = n;
    d->pos = NULL;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

/* 游标语义与 ramfs 一致：pos 为最近返回的子项并被 refs 钉住 */
static int devfs_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    devfs_sb_t *sb = (devfs_sb_t *)d->mnt->priv;
    devfs_node_t *dir = (devfs_node_t *)d->priv;
    devfs_node_t *prev = (devfs_node_t *)d->pos;
    devfs_node_t *next;
    uint64_t irq;

    if (!sb || !dir || !out)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    next = prev ? prev->sibling : dir->child;
    if (!next)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT; /* 遍历结束 */
    }
    next->refs++;
    if (prev)
        prev->refs--;
    d->pos = next;
    kernel_strncpy(out->name, next->name, sizeof(out->name));
    out->is_dir = next->is_dir;
    if (!next->is_dir && next->dev && next->dev->type == VDEV_BLOCK)
        out->size = next->dev->nblocks * next->dev->block_size;
    else
        out->size = 0;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int devfs_closedir(vfs_dir_t *d)
{
    devfs_sb_t *sb = (devfs_sb_t *)d->mnt->priv;
    uint64_t irq;

    if (!sb)
        return VFS_OK;
    irq = spin_lock_irqsave(&sb->lock);
    if (d->pos)
        ((devfs_node_t *)d->pos)->refs--;
    ((devfs_node_t *)d->priv)->refs--;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int devfs_mount(vfs_mount_t *m, const char *dev)
{
    devfs_sb_t *sb;
    devfs_node_t *root;

    (void)dev;
    sb = (devfs_sb_t *)kmalloc(sizeof(devfs_sb_t));
    if (!sb)
        return VFS_ERR_NOMEM;
    root = devfs_new_node("", 0, 1);
    if (!root)
    {
        kfree(sb);
        return VFS_ERR_NOMEM;
    }
    kernel_memset(sb, 0, (uint32_t)sizeof(*sb));
    spin_lock_init(&sb->lock);
    sb->root = root;
    sb->listener.added = devfs_on_added;
    sb->listener.removed = devfs_on_removed;
    sb->listener.ctx = sb;
    m->priv = sb;

    vdev_listen(&sb->listener);
    return VFS_OK;
}

static int devfs_unmount(vfs_mount_t *m)
{
    devfs_sb_t *sb = (devfs_sb_t *)m->priv;

    if (!sb)
        return VFS_OK;
    vdev_unlisten(&sb->listener);
    devfs_free_tree(sb->root);
    kfree(sb);
    m->priv = NULL;
    return VFS_OK;
}

const fs_driver_t devfs_driver = {
    .name = "devfs",
    .mount = devfs_mount,
    .unmount = devfs_unmount,
    .open = devfs_open,
    .close = devfs_close,
    .read = devfs_read,
    .write = devfs_write,
    .seek = devfs_seek,
    .stat = devfs_stat,
    .mkdir = devfs_mkdir,
    .rmdir = devfs_rmdir,
    .opendir = devfs_opendir,
    .readdir = devfs_readdir,
    .closedir = devfs_closedir,
};

void init_devfs()
{
    if (vfs_register_driver(&devfs_driver) != VFS_OK)
    {
        put_string("[DEVFS] register driver failed\n");
        return;
    }
    if (vfs_mount("devfs", "/dev", "devfs") != VFS_OK)
        put_string("[DEVFS] mount /dev failed\n");
}
