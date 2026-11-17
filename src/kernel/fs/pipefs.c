#include <kernel/fs/pipefs.h>
#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/heap.h>
#include <kernel/serial.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

typedef struct pipefs_node
{
    char name[VFS_NAME_MAX];
    pipe_shared_t *sh; /* 节点持有 1 份引用，永不释放（无 unlink） */
    struct pipefs_node *next;
} pipefs_node_t;

typedef struct pipefs_sb
{
    spinlock_t lock;
    pipefs_node_t *head;
} pipefs_sb_t;

static pipefs_node_t *pipefs_find_locked(pipefs_sb_t *sb, const char *rel)
{
    pipefs_node_t *n;

    for (n = sb->head; n != NULL; n = n->next)
    {
        if (kernel_strcmp(n->name, rel) == 0)
            return n;
    }
    return NULL;
}

static int pipefs_mkfifo(vfs_mount_t *m, const char *rel)
{
    pipefs_sb_t *sb = (pipefs_sb_t *)m->priv;
    pipefs_node_t *n;
    pipe_shared_t *sh;
    uint64_t irq;
    int rc = VFS_OK;
    const char *p;

    if (sb == NULL || rel == NULL || rel[0] == '\0')
        return VFS_ERR_INVAL;
    if (kernel_strlen(rel) >= (uint32_t)VFS_NAME_MAX)
        return VFS_ERR_INVAL;
    for (p = rel; *p != '\0'; p++)
    {
        if (*p == '/')
            return VFS_ERR_INVAL; /* 扁平空间：不接受子目录 */
    }

    sh = pipe_shared_new();
    if (sh == NULL)
        return VFS_ERR_NOMEM;
    pipe_shared_hold(sh); /* 节点持有 */
    n = (pipefs_node_t *)kmalloc(sizeof(*n));
    if (n == NULL)
    {
        pipe_shared_release(sh);
        return VFS_ERR_NOMEM;
    }
    kernel_strncpy(n->name, rel, sizeof(n->name));
    n->sh = sh;
    n->next = NULL;

    irq = spin_lock_irqsave(&sb->lock);
    if (pipefs_find_locked(sb, rel) != NULL)
        rc = VFS_ERR_EXIST;
    else
    {
        n->next = sb->head;
        sb->head = n;
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    if (rc != VFS_OK)
    {
        pipe_shared_release(sh);
        kfree(n);
    }
    return rc;
}

static int pipefs_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    pipefs_sb_t *sb = (pipefs_sb_t *)m->priv;
    pipefs_node_t *n;
    vfs_file_t *pf;
    uint64_t irq;
    int accmode;

    if (sb == NULL || rel == NULL || f == NULL)
        return VFS_ERR_INVAL;
    if ((flags & VFS_O_DIRECTORY) != 0)
        return VFS_ERR_NOTDIR;
    accmode = flags & 0x03;
    if (accmode != VFS_O_RDONLY && accmode != VFS_O_WRONLY)
        return VFS_ERR_INVAL; /* O_RDWR 暂不支持：端点分裂，无合一端 */
    /* O_CREAT/O_TRUNC/O_APPEND 对 fifo 无意义：存在即用，不存在也不建（建是 mkfifo 的活） */

    irq = spin_lock_irqsave(&sb->lock);
    n = pipefs_find_locked(sb, rel);
    spin_unlock_irqrestore(&sb->lock, irq);
    if (n == NULL)
        return VFS_ERR_NOENT;
    /* 节点永生：找到即稳定，无需 refs 钉住。
     * flags 原样透传（含 O_NONBLOCK，供 syscall 层做轮询；方向判定只看低 2 位）。 */
    pf = pipe_make_file(n->sh, accmode == VFS_O_WRONLY, m, flags);
    if (pf == NULL)
        return VFS_ERR_NOMEM;

    pipe_wait_peer(n->sh, accmode == VFS_O_WRONLY);
    f->mnt = pf->mnt;
    f->drv = pf->drv;
    f->flags = pf->flags;
    f->priv = pf->priv;
    f->pos = 0;
    f->size = 0;
    f->is_dir = 0;
    kfree(pf);
    return VFS_OK;
}

static int pipefs_stat(vfs_mount_t *m, const char *rel, uint64_t *size_out,
                       uint32_t *is_dir_out)
{
    pipefs_sb_t *sb = (pipefs_sb_t *)m->priv;
    pipefs_node_t *n;
    uint64_t irq;

    if (sb == NULL || rel == NULL)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    n = pipefs_find_locked(sb, rel);
    spin_unlock_irqrestore(&sb->lock, irq);
    if (n == NULL)
        return VFS_ERR_NOENT;
    if (size_out != NULL)
        *size_out = 0; /* 管道无固定大小 */
    if (is_dir_out != NULL)
        *is_dir_out = 0;
    return VFS_OK;
}

static int pipefs_opendir(vfs_mount_t *m, const char *rel, vfs_dir_t *d)
{
    if (m == NULL || rel == NULL || d == NULL)
        return VFS_ERR_INVAL;
    if (rel[0] != '\0')
        return VFS_ERR_NOTDIR; /* 只有 /pipe 根是目录 */
    d->priv = m->priv;
    d->pos = NULL;
    return VFS_OK;
}

static int pipefs_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    pipefs_sb_t *sb = (pipefs_sb_t *)d->priv;
    pipefs_node_t *prev = (pipefs_node_t *)d->pos;
    pipefs_node_t *next;
    uint64_t irq;

    if (sb == NULL || out == NULL)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    next = prev != NULL ? prev->next : sb->head;
    if (next == NULL)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOENT; /* 遍历结束（节点永生，无需 refs 钉住） */
    }
    kernel_strncpy(out->name, next->name, sizeof(out->name));
    out->is_dir = 0;
    out->size = 0;
    d->pos = next;
    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int pipefs_closedir(vfs_dir_t *d)
{
    if (d == NULL)
        return VFS_OK;
    d->priv = NULL;
    d->pos = NULL;
    return VFS_OK;
}

static int pipefs_mount(vfs_mount_t *m, const char *dev)
{
    pipefs_sb_t *sb;

    (void)dev;
    sb = (pipefs_sb_t *)kmalloc(sizeof(*sb));
    if (sb == NULL)
        return VFS_ERR_NOMEM;
    spin_lock_init(&sb->lock);
    sb->head = NULL;
    m->priv = sb;
    return VFS_OK;
}

static int pipefs_unmount(vfs_mount_t *m)
{
    pipefs_sb_t *sb = (pipefs_sb_t *)m->priv;
    pipefs_node_t *n;
    pipefs_node_t *next;

    if (sb == NULL)
        return VFS_OK;
    /* 摘掉所有节点并释放节点的缓冲引用；仍打开的端点靠自己的引用续命 */
    n = sb->head;
    sb->head = NULL;
    for (; n != NULL; n = next)
    {
        next = n->next;
        pipe_shared_release(n->sh);
        kfree(n);
    }
    kfree(sb);
    m->priv = NULL;
    return VFS_OK;
}

const fs_driver_t pipefs_driver = {
    .name = "pipefs",
    .mount = pipefs_mount,
    .unmount = pipefs_unmount,
    .open = pipefs_open,
    .mkfifo = pipefs_mkfifo,
    .stat = pipefs_stat,
    .opendir = pipefs_opendir,
    .readdir = pipefs_readdir,
    .closedir = pipefs_closedir,
    /* close/read/write/seek 全走 f->drv（pipe_driver，由 pipe_make_file 指定） */
};

void init_pipefs()
{
    if (vfs_register_driver(&pipefs_driver) != VFS_OK)
        put_string("[PIPEFS] register driver failed\n");
}
