#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/heap.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* 匿名管道：4KB 环形缓冲 + 双端点各一个 vfs_file_t */

#define PIPE_BUF_SIZE 4096

typedef struct pipe_shared
{
    spinlock_t lock;
    uint8_t data[PIPE_BUF_SIZE];
    uint32_t head;  /* 读位置 */
    uint32_t tail;  /* 写位置 */
    uint32_t count; /* 有效字节数 */
    int read_open;
    int write_open;
} pipe_shared_t;

typedef struct pipe_end
{
    pipe_shared_t *sh;
    int is_write; /* 0=读端，1=写端 */
} pipe_end_t;

/* 假挂载点：vfs_close 必调 vfs_unpin(f->mnt)，给个永不卸载的静态位。
 * open_count 靠 __sync 加减保持平衡（实际无人 unmount，仅卫生）。 */
static vfs_mount_t g_pipe_mount;

static int64_t pipe_read(vfs_file_t *f, void *buf, uint64_t len)
{
    pipe_end_t *e = (pipe_end_t *)f->priv;
    pipe_shared_t *sh;
    uint64_t flags;
    uint64_t n = 0;
    uint8_t *p = (uint8_t *)buf;

    if (e == NULL || e->is_write)
        return VFS_ERR_INVAL;
    if (len == 0)
        return 0;
    sh = e->sh;
    flags = spin_lock_irqsave(&sh->lock);
    if (sh->count == 0)
    {
        int w = sh->write_open;

        spin_unlock_irqrestore(&sh->lock, flags);
        return w ? VFS_ERR_AGAIN : 0;
    }
    while (n < len && sh->count > 0)
    {
        p[n++] = sh->data[sh->head];
        sh->head = (sh->head + 1) % PIPE_BUF_SIZE;
        sh->count--;
    }
    spin_unlock_irqrestore(&sh->lock, flags);
    return (int64_t)n;
}

static int64_t pipe_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    pipe_end_t *e = (pipe_end_t *)f->priv;
    pipe_shared_t *sh;
    uint64_t flags;
    uint64_t n = 0;
    const uint8_t *p = (const uint8_t *)buf;

    if (e == NULL || !e->is_write)
        return VFS_ERR_INVAL;
    if (len == 0)
        return 0;
    sh = e->sh;
    flags = spin_lock_irqsave(&sh->lock);
    if (!sh->read_open)
    {
        spin_unlock_irqrestore(&sh->lock, flags);
        return VFS_ERR_INVAL; /* broken pipe */
    }
    if (sh->count == PIPE_BUF_SIZE)
    {
        spin_unlock_irqrestore(&sh->lock, flags);
        return VFS_ERR_AGAIN;
    }
    while (n < len && sh->count < PIPE_BUF_SIZE)
    {
        sh->data[sh->tail] = p[n++];
        sh->tail = (sh->tail + 1) % PIPE_BUF_SIZE;
        sh->count++;
    }
    spin_unlock_irqrestore(&sh->lock, flags);
    return (int64_t)n;
}

static int pipe_close(vfs_file_t *f)
{
    pipe_end_t *e = (pipe_end_t *)f->priv;
    pipe_shared_t *sh;
    uint64_t flags;
    int free_sh = 0;

    if (e == NULL)
        return VFS_OK;
    sh = e->sh;
    flags = spin_lock_irqsave(&sh->lock);
    if (e->is_write)
        sh->write_open = 0;
    else
        sh->read_open = 0;
    if (!sh->read_open && !sh->write_open)
        free_sh = 1; /* 锁内判定：恰好一个关闭者看到双关 */
    spin_unlock_irqrestore(&sh->lock, flags);
    kfree(e);
    f->priv = NULL;
    if (free_sh)
        kfree(sh);
    return VFS_OK;
}

static const fs_driver_t pipe_driver = {
    .name = "pipe",
    .close = pipe_close,
    .read = pipe_read,
    .write = pipe_write,
    /* open/seek/mkdir 等全 NULL：不走路径，seek 得 NOTSUP */
};

int pipe_create(vfs_file_t **r_out, vfs_file_t **w_out)
{
    pipe_shared_t *sh;
    pipe_end_t *re;
    pipe_end_t *we;
    vfs_file_t *rf;
    vfs_file_t *wf;

    if (r_out == NULL || w_out == NULL)
        return VFS_ERR_INVAL;
    *r_out = NULL;
    *w_out = NULL;
    sh = (pipe_shared_t *)kmalloc(sizeof(*sh));
    re = (pipe_end_t *)kmalloc(sizeof(*re));
    we = (pipe_end_t *)kmalloc(sizeof(*we));
    rf = (vfs_file_t *)kmalloc(sizeof(*rf));
    wf = (vfs_file_t *)kmalloc(sizeof(*wf));
    if (sh == NULL || re == NULL || we == NULL || rf == NULL || wf == NULL)
    {
        if (sh != NULL)
            kfree(sh);
        if (re != NULL)
            kfree(re);
        if (we != NULL)
            kfree(we);
        if (rf != NULL)
            kfree(rf);
        if (wf != NULL)
            kfree(wf);
        return VFS_ERR_NOMEM;
    }
    spin_lock_init(&sh->lock);
    sh->head = 0;
    sh->tail = 0;
    sh->count = 0;
    sh->read_open = 1;
    sh->write_open = 1;
    re->sh = sh;
    re->is_write = 0;
    we->sh = sh;
    we->is_write = 1;
    kernel_memset(rf, 0, (uint32_t)sizeof(*rf));
    rf->mnt = &g_pipe_mount;
    rf->drv = &pipe_driver;
    rf->flags = VFS_O_RDONLY;
    rf->priv = re;
    kernel_memset(wf, 0, (uint32_t)sizeof(*wf));
    wf->mnt = &g_pipe_mount;
    wf->drv = &pipe_driver;
    wf->flags = VFS_O_WRONLY;
    wf->priv = we;
    __sync_fetch_and_add(&g_pipe_mount.open_count, 2);
    *r_out = rf;
    *w_out = wf;
    return VFS_OK;
}
