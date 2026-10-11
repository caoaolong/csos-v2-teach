#include <fs/ramfs.h>
#include <fs/vfs.h>
#include <memory/heap.h>
#include <spinlock.h>
#include <serial.h>
#include <string.h>

#define RAMFS_DEF_CAP 512

/* 树形节点：子项按名字升序挂在 child 链表上 */
typedef struct ramfs_node
{
    char name[VFS_NAME_MAX];
    uint8_t *data;
    uint64_t size;
    uint64_t cap;
    uint32_t is_dir;
    uint32_t refs; /* 打开句柄 + 目录游标；>0 时禁止 rmdir */
    struct ramfs_node *parent;
    struct ramfs_node *child;
    struct ramfs_node *sibling;
} ramfs_node_t;

typedef struct ramfs_sb
{
    spinlock_t lock;
    ramfs_node_t *root;
} ramfs_sb_t;

/* 取下一段路径分量 [*p, end)，跳过连续 '/'；返回分量长度，0 表示耗尽 */
static uint32_t ramfs_next_comp(const char **p, const char *end, const char **comp)
{
    const char *s = *p;
    while (s < end && *s == '/')
        s++;
    if (s >= end)
    {
        *p = s;
        return 0;
    }
    *comp = s;
    while (s < end && *s != '/')
        s++;
    *p = s;
    return (uint32_t)(s - *comp);
}

static int ramfs_name_eq(const char *name, const char *comp, uint32_t len)
{
    return kernel_strlen(name) == len && kernel_memcmp((void *)name, (void *)comp, len) == 0;
}

static ramfs_node_t *ramfs_child_find(ramfs_node_t *dir, const char *comp, uint32_t len)
{
    ramfs_node_t *c;
    for (c = dir->child; c; c = c->sibling)
        if (ramfs_name_eq(c->name, comp, len))
            return c;
    return NULL;
}

/* 按名字升序插入，保证 readdir 顺序稳定 */
static void ramfs_child_link(ramfs_node_t *dir, ramfs_node_t *node)
{
    ramfs_node_t **pp = &dir->child;

    while (*pp && kernel_strcmp((*pp)->name, node->name) < 0)
        pp = &(*pp)->sibling;
    node->sibling = *pp;
    *pp = node;
    node->parent = dir;
}

static void ramfs_child_unlink(ramfs_node_t *node)
{
    ramfs_node_t **pp = &node->parent->child;

    while (*pp && *pp != node)
        pp = &(*pp)->sibling;
    if (*pp == node)
        *pp = node->sibling;
    node->sibling = NULL;
    node->parent = NULL;
}

/* 拆出父路径长度与末级名字；无 '/' 时父为根（长度 0） */
static void ramfs_split(const char *rel, uint32_t len, uint32_t *parent_len,
                        const char **leaf, uint32_t *leaf_len)
{
    uint32_t i = len;

    while (i > 0 && rel[i - 1] != '/')
        i--;
    *leaf = rel + i;
    *leaf_len = len - i;
    *parent_len = i > 0 ? i - 1 : 0;
}

/* 调用者已持锁：按路径查找节点，空路径即根 */
static int ramfs_lookup(ramfs_sb_t *sb, const char *path, uint32_t len, ramfs_node_t **out)
{
    const char *p = path;
    const char *end = path + len;
    const char *comp;
    ramfs_node_t *cur = sb->root;
    ramfs_node_t *c;
    uint32_t clen;

    while ((clen = ramfs_next_comp(&p, end, &comp)) != 0)
    {
        if (!cur->is_dir)
            return VFS_ERR_NOTDIR;
        c = ramfs_child_find(cur, comp, clen);
        if (!c)
            return VFS_ERR_NOENT;
        cur = c;
    }
    *out = cur;
    return VFS_OK;
}

/* 调用者已持锁：在 rel 处新建文件或目录，父目录必须已存在 */
static int ramfs_create_locked(ramfs_sb_t *sb, const char *rel, uint32_t len,
                               uint32_t is_dir, ramfs_node_t **out)
{
    uint32_t plen, llen;
    const char *leaf;
    ramfs_node_t *parent;
    ramfs_node_t *n;
    int rc;

    ramfs_split(rel, len, &plen, &leaf, &llen);
    if (llen == 0 || llen >= VFS_NAME_MAX)
        return VFS_ERR_INVAL;
    rc = ramfs_lookup(sb, rel, plen, &parent);
    if (rc != VFS_OK)
        return rc;
    if (!parent->is_dir)
        return VFS_ERR_NOTDIR;
    if (ramfs_child_find(parent, leaf, llen))
        return VFS_ERR_EXIST;

    n = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!n)
        return VFS_ERR_NOMEM;
    kernel_memset(n, 0, (uint32_t)sizeof(*n));
    kernel_memcpy(n->name, (void *)leaf, llen);
    n->name[llen] = '\0';
    n->is_dir = is_dir;
    ramfs_child_link(parent, n);
    *out = n;
    return VFS_OK;
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

static void ramfs_free_tree(ramfs_node_t *n)
{
    ramfs_node_t *c = n->child;

    while (c)
    {
        ramfs_node_t *next = c->sibling;
        ramfs_free_tree(c);
        c = next;
    }
    if (n->data)
        kfree(n->data);
    kfree(n);
}

static int ramfs_mount(vfs_mount_t *m, const char *dev)
{
    ramfs_sb_t *sb;
    ramfs_node_t *root;

    (void)dev;
    sb = (ramfs_sb_t *)kmalloc(sizeof(ramfs_sb_t));
    if (!sb)
        return VFS_ERR_NOMEM;
    root = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!root)
    {
        kfree(sb);
        return VFS_ERR_NOMEM;
    }
    kernel_memset(sb, 0, (uint32_t)sizeof(*sb));
    kernel_memset(root, 0, (uint32_t)sizeof(*root));
    root->is_dir = 1;
    spin_lock_init(&sb->lock);
    sb->root = root;
    m->priv = sb;
    return VFS_OK;
}

static int ramfs_unmount(vfs_mount_t *m)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;

    if (!sb)
        return VFS_OK;
    ramfs_free_tree(sb->root);
    kfree(sb);
    m->priv = NULL;
    return VFS_OK;
}

static int ramfs_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n = NULL;
    uint32_t len = kernel_strlen(rel);
    uint64_t irq;
    int want_dir = (flags & VFS_O_DIRECTORY) != 0;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_lookup(sb, rel, len, &n);
    if (rc == VFS_ERR_NOENT && (flags & VFS_O_CREAT) && !want_dir)
        rc = ramfs_create_locked(sb, rel, len, 0, &n);
    if (rc == VFS_OK)
    {
        if (n->is_dir && !want_dir)
            rc = VFS_ERR_ISDIR;
        else if (!n->is_dir && want_dir)
            rc = VFS_ERR_NOTDIR;
        else if (!n->is_dir && (flags & VFS_O_TRUNC) && (flags & 0x03) != VFS_O_RDONLY)
            n->size = 0;
    }
    if (rc == VFS_OK)
    {
        n->refs++;
        f->priv = n;
        f->is_dir = n->is_dir;
        f->size = n->size;
        f->pos = (!n->is_dir && (flags & VFS_O_APPEND)) ? n->size : 0;
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int ramfs_close(vfs_file_t *f)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)f->mnt->priv;
    ramfs_node_t *n = (ramfs_node_t *)f->priv;
    uint64_t irq;

    if (!sb || !n)
        return VFS_OK;
    irq = spin_lock_irqsave(&sb->lock);
    n->refs--;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int64_t ramfs_read(vfs_file_t *f, void *buf, uint64_t len)
{
    ramfs_node_t *n = (ramfs_node_t *)f->priv;
    ramfs_sb_t *sb = (ramfs_sb_t *)f->mnt->priv;
    uint64_t avail, irq;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
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

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!n)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_grow(n, f->pos + len);
    if (rc != VFS_OK)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return rc;
    }
    /* seek 越过 EOF 产生的空洞必须读出为 0，kmalloc 不保证清零 */
    if (f->pos > n->size)
        kernel_memset(n->data + n->size, 0, (uint32_t)(f->pos - n->size));
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

    if (f->is_dir)
        return VFS_ERR_ISDIR;
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
    ramfs_node_t *n = NULL;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_lookup(sb, rel, kernel_strlen(rel), &n);
    if (rc == VFS_OK)
    {
        if (size_out)
            *size_out = n->size;
        if (is_dir_out)
            *is_dir_out = n->is_dir;
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int ramfs_mkdir(vfs_mount_t *m, const char *rel)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n = NULL;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_create_locked(sb, rel, kernel_strlen(rel), 1, &n);
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int ramfs_rmdir(vfs_mount_t *m, const char *rel)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n = NULL;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_lookup(sb, rel, kernel_strlen(rel), &n);
    if (rc == VFS_OK)
    {
        if (!n->is_dir)
            rc = VFS_ERR_NOTDIR;
        else if (n == sb->root)
            rc = VFS_ERR_BUSY;
        else if (n->child)
            rc = VFS_ERR_NOTEMPTY;
        else if (n->refs)
            rc = VFS_ERR_BUSY; /* 仍有句柄或游标引用 */
        else
        {
            ramfs_child_unlink(n);
            kfree(n);
        }
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int ramfs_opendir(vfs_mount_t *m, const char *rel, vfs_dir_t *d)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)m->priv;
    ramfs_node_t *n = NULL;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = ramfs_lookup(sb, rel, kernel_strlen(rel), &n);
    if (rc == VFS_OK)
    {
        if (!n->is_dir)
        {
            rc = VFS_ERR_NOTDIR;
        }
        else
        {
            n->refs++;
            d->priv = n;
            d->pos = NULL;
        }
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

/* 游标语义：pos 为最近返回的子项并被引用计数钉住，rmdir 无法删除它，链表指针始终有效 */
static int ramfs_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)d->mnt->priv;
    ramfs_node_t *dir = (ramfs_node_t *)d->priv;
    ramfs_node_t *prev = (ramfs_node_t *)d->pos;
    ramfs_node_t *next;
    uint64_t irq;

    if (!sb)
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
    out->size = next->size;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int ramfs_closedir(vfs_dir_t *d)
{
    ramfs_sb_t *sb = (ramfs_sb_t *)d->mnt->priv;
    uint64_t irq;

    if (!sb)
        return VFS_OK;
    irq = spin_lock_irqsave(&sb->lock);
    if (d->pos)
        ((ramfs_node_t *)d->pos)->refs--;
    ((ramfs_node_t *)d->priv)->refs--;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
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
    .mkdir = ramfs_mkdir,
    .rmdir = ramfs_rmdir,
    .opendir = ramfs_opendir,
    .readdir = ramfs_readdir,
    .closedir = ramfs_closedir,
};
