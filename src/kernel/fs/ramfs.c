#include <fs/ramfs.h>
#include <fs/vfs.h>
#include <memory/heap.h>
#include <spinlock.h>
#include <serial.h>
#include <string.h>

#define RAMFS_MAX_NAME 60
#define RAMFS_DEF_CAP 512

typedef struct ramfs_node
{
    char name[VFS_PATH_MAX]; /* 挂载点内的相对路径，如 "a.txt" / "dir/b.txt" */
    uint8_t *data;
    uint64_t size;
    uint64_t cap;
    uint32_t is_dir;
    struct ramfs_node *next;
} ramfs_node_t;

typedef struct ramfs_sb
{
    spinlock_t lock;
    ramfs_node_t *head;
} ramfs_sb_t;

static uint32_t ramfs_strlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static int ramfs_streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void ramfs_strcpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i = 0;
    if (!dst || !src || !n)
        return;
    while (src[i] && i + 1 < n)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static ramfs_node_t *ramfs_find(ramfs_sb_t *sb, const char *rel)
{
    ramfs_node_t *cur = sb->head;
    while (cur)
    {
        if (ramfs_streq(cur->name, rel))
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int ramfs_grow(ramfs_node_t *n, uint64_t need)
{
    uint64_t ncap = n->cap ? n->cap : RAMFS_DEF_CAP;
    uint8_t *ndata;
    if (n->data && need <= ncap)
        return VFS_OK;
    while (ncap < need)
        ncap *= 2;
    ndata = (uint8_t *)kmalloc((size_t)ncap);
    if (!ndata)
        return VFS_ERR_NOMEM;
    if (n->data && n->size)
        kernel_memcpy(ndata, n->data, (uint32_t)n->size);
    if (n->data)
        kfree(n->data);
    n->data = ndata;
    n->cap = ncap;
    return VFS_OK;
}

static int ramfs_mount(vfs_mount_t *m, const char *dev)
{
    ramfs_sb_t *sb;
    (void)dev;
    sb = (ramfs_sb_t *)kmalloc(sizeof(ramfs_sb_t));
    if (!sb)
        return VFS_ERR_NOMEM;
    kernel_memset(sb, 0, (uint32_t)sizeof(*sb));
    spin_lock_init(&sb->lock);
    m->priv = sb;
    return VFS_OK;
}

static int ramfs_unmount(vfs_mount_t *m)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *cur, *next;
    if (!sb)
        return VFS_OK;
    cur = sb->head;
    while (cur)
    {
        next = cur->next;
        if (cur->data)
            kfree(cur->data);
        kfree(cur);
        cur = next;
    }
    kfree(sb);
    m->priv = NULL;
    return VFS_OK;
}

static int ramfs_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n;
    uint64_t irq;
    int rc = VFS_OK;

    if (!sb || !rel)
        return VFS_ERR_INVAL;
    if (rel[0] == '\0')
        return VFS_ERR_ISDIR; /* open 挂载点本身：视为目录，演示用直接拒绝 */

    irq = spin_lock_irqsave(&sb->lock);
    n = ramfs_find(sb, rel);
    if (!n)
    {
        if (!(flags & VFS_O_CREAT))
        {
            rc = VFS_ERR_NOENT;
            goto out;
        }
        if (ramfs_strlen(rel) >= VFS_PATH_MAX)
        {
            rc = VFS_ERR_INVAL;
            goto out;
        }
        n = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
        if (!n)
        {
            rc = VFS_ERR_NOMEM;
            goto out;
        }
        kernel_memset(n, 0, (uint32_t)sizeof(*n));
        ramfs_strcpy(n->name, rel, sizeof(n->name));
        n->next = sb->head;
        sb->head = n;
    }
    else if (n->is_dir)
    {
        rc = VFS_ERR_ISDIR;
        goto out;
    }
    if ((flags & VFS_O_TRUNC) && (flags & 0x03) != VFS_O_RDONLY)
        n->size = 0;
    f->priv = n;
    f->pos = (flags & VFS_O_APPEND) ? n->size : 0;
    f->size = n->size;
    f->is_dir = 0;
out:
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int ramfs_close(vfs_file_t *f)
{
    (void)f;
    return VFS_OK;
}

static int64_t ramfs_read(vfs_file_t *f, void *buf, uint64_t len)
{
    ramfs_node_t *n = (ramfs_node_t *)f->priv;
    ramfs_sb_t *sb = (ramfs_sb_t *)f->mnt->priv;
    uint64_t avail, irq;

    if (!n)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    avail = n->size > f->pos ? n->size - f->pos : 0;
    if (len > avail)
        len = avail;
    if (len)
        kernel_memcpy(buf, n->data + f->pos, (uint32_t)len);
    f->pos += len;
    spin_unlock_irqrestore(&sb->lock, irq);
    return (int64_t)len;
}

static int64_t ramfs_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    ramfs_node_t *n = (ramfs_node_t *)f->priv;
    ramfs_sb_t *sb = (ramfs_sb_t *)f->mnt->priv;
    uint64_t irq;
    int rc;

    if (!n)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_grow(n, f->pos + len);
    if (rc != VFS_OK)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return rc;
    }
    if (len)
        kernel_memcpy(n->data + f->pos, (void *)buf, (uint32_t)len);
    f->pos += len;
    if (f->pos > n->size)
        n->size = f->pos;
    f->size = n->size;
    spin_unlock_irqrestore(&sb->lock, irq);
    return (int64_t)len;
}

static int64_t ramfs_seek(vfs_file_t *f, int64_t off, int whence)
{
    ramfs_node_t *n = (ramfs_node_t *)f->priv;
    int64_t base;
    if (!n)
        return VFS_ERR_INVAL;
    if (whence == VFS_SEEK_SET)
        base = off;
    else if (whence == VFS_SEEK_CUR)
        base = (int64_t)f->pos + off;
    else
        base = (int64_t)n->size + off;
    if (base < 0)
        return VFS_ERR_INVAL;
    f->pos = (uint64_t)base;
    return (int64_t)f->pos;
}

static int ramfs_stat(vfs_mount_t *m, const char *rel, uint64_t *size_out, uint32_t *is_dir_out)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n;
    uint64_t irq;
    int rc = VFS_OK;

    if (!sb || !rel)
        return VFS_ERR_INVAL;
    if (rel[0] == '\0')
    {
        if (size_out)
            *size_out = 0;
        if (is_dir_out)
            *is_dir_out = 1;
        return VFS_OK;
    }
    irq = spin_lock_irqsave(&sb->lock);
    n = ramfs_find(sb, rel);
    if (!n)
        rc = VFS_ERR_NOENT;
    else
    {
        if (size_out)
            *size_out = n->size;
        if (is_dir_out)
            *is_dir_out = n->is_dir;
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

const fs_driver_t ramfs_driver = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
    .seek = ramfs_seek,
    .stat = ramfs_stat,
};
