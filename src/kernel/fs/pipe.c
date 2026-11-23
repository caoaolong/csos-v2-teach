#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/heap.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#define PIPE_BUF_SIZE 4096

/* 环形缓冲，引用计数管理生存期 */
typedef struct pipe_shared
{
    spinlock_t lock;
    uint8_t data[PIPE_BUF_SIZE];
    uint32_t head;  /* 读位置 */
    uint32_t tail;  /* 写位置 */
    uint32_t count; /* 有效字节数 */
    int readers;    /* 读端数 */
    int writers;    /* 写端数 */
    int refs;       /* 持有者数，归零释放 */
} pipe_shared_t;

typedef struct pipe_end
{
    pipe_shared_t *sh;
    int is_write; /* 读写端标志 */
} pipe_end_t;

/* 匿名管道用静态假挂载点 */
static vfs_mount_t g_pipe_mount;

pipe_shared_t *pipe_shared_new(void)
{
    pipe_shared_t *sh = (pipe_shared_t *)kmalloc(sizeof(*sh));

    if (sh == NULL)
        return NULL;
    spin_lock_init(&sh->lock);
    sh->head = 0;
    sh->tail = 0;
    sh->count = 0;
    sh->readers = 0;
    sh->writers = 0;
    sh->refs = 0;
    return sh;
}

void pipe_shared_hold(pipe_shared_t *sh)
{
    if (sh == NULL)
        return;
    __sync_fetch_and_add(&sh->refs, 1);
}

void pipe_shared_release(pipe_shared_t *sh)
{
    if (sh == NULL)
        return;
    if (__sync_sub_and_fetch(&sh->refs, 1) == 0)
        kfree(sh);
}

/* 建端点并持有缓冲引用 */
static pipe_end_t *pipe_end_new(pipe_shared_t *sh, int is_write)
{
    pipe_end_t *e;
    uint64_t flags;

    if (sh == NULL)
        return NULL;
    e = (pipe_end_t *)kmalloc(sizeof(*e));
    if (e == NULL)
        return NULL;
    e->sh = sh;
    e->is_write = is_write ? 1 : 0;
    flags = spin_lock_irqsave(&sh->lock);
    if (e->is_write)
        sh->writers++;
    else
        sh->readers++;
    spin_unlock_irqrestore(&sh->lock, flags);
    pipe_shared_hold(sh);
    return e;
}

/* 拆端点并释放缓冲引用 */
static void pipe_end_free(pipe_end_t *e)
{
    pipe_shared_t *sh;
    uint64_t flags;

    if (e == NULL)
        return;
    sh = e->sh;
    flags = spin_lock_irqsave(&sh->lock);
    if (e->is_write)
        sh->writers--;
    else
        sh->readers--;
    spin_unlock_irqrestore(&sh->lock, flags);
    kfree(e);
    pipe_shared_release(sh);
}

/* 建端点句柄，挂载点计数配对 */
vfs_file_t *pipe_make_file(pipe_shared_t *sh, int is_write,
                           vfs_mount_t *mnt, int flags)
{
    pipe_end_t *e;
    vfs_file_t *f;

    if (sh == NULL || mnt == NULL)
        return NULL;
    e = pipe_end_new(sh, is_write);
    if (e == NULL)
        return NULL;
    f = (vfs_file_t *)kmalloc(sizeof(*f));
    if (f == NULL)
    {
        pipe_end_free(e);
        return NULL;
    }
    kernel_memset(f, 0, (uint32_t)sizeof(*f));
    f->mnt = mnt;
    f->drv = &pipe_driver;
    f->flags = flags;
    f->priv = e;
    __sync_fetch_and_add(&mnt->open_count, 1);
    return f;
}

/* 阻塞等对端打开 */
void pipe_wait_peer(pipe_shared_t *sh, int is_write)
{
    for (;;)
    {
        int present;
        uint64_t flags;

        if (sh == NULL)
            return;
        flags = spin_lock_irqsave(&sh->lock);
        present = is_write ? sh->readers > 0 : sh->writers > 0;
        spin_unlock_irqrestore(&sh->lock, flags);
        if (present)
            return;
        yield();
    }
}

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
        int w = sh->writers;

        spin_unlock_irqrestore(&sh->lock, flags);
        /* 写端全关返回结束，否则重试 */
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
    if (!sh->readers)
    {
        spin_unlock_irqrestore(&sh->lock, flags);
        return VFS_ERR_INVAL; /* 无读端 */
    }
    if (sh->count == PIPE_BUF_SIZE)
    {
        spin_unlock_irqrestore(&sh->lock, flags);
        return VFS_ERR_AGAIN; /* 阻塞调用方 yield 重试，非阻塞直接回 EAGAIN */
    }
    if ((f->flags & VFS_O_NONBLOCK) != 0 && len <= PIPE_BUF_SIZE &&
        len > PIPE_BUF_SIZE - sh->count)
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

    if (e == NULL)
        return VFS_OK;
    f->priv = NULL;
    pipe_end_free(e);
    return VFS_OK;
}

const fs_driver_t pipe_driver = {
    .name = "pipe",
    .close = pipe_close,
    .read = pipe_read,
    .write = pipe_write,
    /* 端点只经专用接口创建 */
};

int pipe_create(vfs_file_t **r_out, vfs_file_t **w_out)
{
    pipe_shared_t *sh;
    vfs_file_t *rf;
    vfs_file_t *wf;

    if (r_out == NULL || w_out == NULL)
        return VFS_ERR_INVAL;
    *r_out = NULL;
    *w_out = NULL;
    sh = pipe_shared_new();
    if (sh == NULL)
        return VFS_ERR_NOMEM;
    rf = pipe_make_file(sh, 0, &g_pipe_mount, VFS_O_RDONLY);
    if (rf == NULL)
    {
        pipe_shared_release(sh);
        return VFS_ERR_NOMEM;
    }
    wf = pipe_make_file(sh, 1, &g_pipe_mount, VFS_O_WRONLY);
    if (wf == NULL)
    {
        /* 回滚读端并配平计数 */
        pipe_close(rf);
        __sync_fetch_and_sub(&g_pipe_mount.open_count, 1);
        kfree(rf);
        return VFS_ERR_NOMEM;
    }
    *r_out = rf;
    *w_out = wf;
    return VFS_OK;
}
