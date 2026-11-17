#include <kernel/fd.h>
#include <abi.h>
#include <kernel/sched.h>
#include <kernel/serial.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/memory/heap.h>

/* fd 表：全局锁保护当前任务表 */
static spinlock_t g_fd_lock = SPINLOCK_INIT;

/* 包装 vf 装入最小空闲槽，调用者已持锁 */
static int fd_install_locked(struct task *t, vfs_file_t *vf)
{
    fd_entry_t *e;
    int i;

    if (t == NULL || vf == NULL)
        return -1;
    e = (fd_entry_t *)kmalloc(sizeof(*e));
    if (e == NULL)
        return -1;
    e->vf = vf;
    e->refs = 1;
    for (i = 0; i < FD_MAX; i++)
    {
        if (t->fds[i] == NULL)
        {
            t->fds[i] = e;
            return i;
        }
    }
    kfree(e);
    return -1; /* 表满 */
}

void fd_init_std(struct task *t)
{
    vfs_file_t *f0 = NULL;
    vfs_file_t *f1 = NULL;
    vfs_file_t *f2 = NULL;
    int rc0, rc1, rc2;
    uint64_t flags;

    if (t == NULL)
        return;

    /* task_create 已清零 fds，只做 open */
    rc0 = vfs_open("/dev/kbd0", VFS_O_RDONLY, &f0);
    rc1 = vfs_open("/dev/console", VFS_O_WRONLY, &f1);
    rc2 = vfs_open("/dev/console", VFS_O_WRONLY, &f2);

    flags = spin_lock_irqsave(&g_fd_lock);
    /* 标准流固定占 0/1/2 */
    if (rc0 == VFS_OK)
    {
        fd_entry_t *e = (fd_entry_t *)kmalloc(sizeof(*e));
        if (e != NULL)
        {
            e->vf = f0;
            e->refs = 1;
            t->fds[STDIN_FILENO] = e;
        }
        else
        {
            vfs_close(f0);
            rc0 = VFS_ERR_NOMEM;
        }
    }
    if (rc1 == VFS_OK)
    {
        fd_entry_t *e = (fd_entry_t *)kmalloc(sizeof(*e));
        if (e != NULL)
        {
            e->vf = f1;
            e->refs = 1;
            t->fds[STDOUT_FILENO] = e;
        }
        else
        {
            vfs_close(f1);
            rc1 = VFS_ERR_NOMEM;
        }
    }
    if (rc2 == VFS_OK)
    {
        fd_entry_t *e = (fd_entry_t *)kmalloc(sizeof(*e));
        if (e != NULL)
        {
            e->vf = f2;
            e->refs = 1;
            t->fds[STDERR_FILENO] = e;
        }
        else
        {
            vfs_close(f2);
            rc2 = VFS_ERR_NOMEM;
        }
    }
    spin_unlock_irqrestore(&g_fd_lock, flags);

    if (rc0 != VFS_OK || rc1 != VFS_OK || rc2 != VFS_OK)
        fput_string("[FD] stdio init rc=%d/%d/%d\n", rc0, rc1, rc2);
    else
        put_string("[FD] stdio 0=/dev/kbd0 1,2=/dev/console ready\n");
}

vfs_file_t *fd_get(int fd)
{
    task_t *t;
    fd_entry_t *e;
    uint64_t flags;

    if (fd < 0 || fd >= FD_MAX)
        return NULL;
    t = sched_current();
    if (t == NULL)
        return NULL;
    flags = spin_lock_irqsave(&g_fd_lock);
    e = t->fds[fd];
    spin_unlock_irqrestore(&g_fd_lock, flags);
    return e ? e->vf : NULL;
}

int fd_install(vfs_file_t *vf)
{
    task_t *t;
    uint64_t flags;
    int fd;

    if (vf == NULL)
        return -1;
    t = sched_current();
    if (t == NULL)
        return -1;
    flags = spin_lock_irqsave(&g_fd_lock);
    fd = fd_install_locked(t, vf);
    spin_unlock_irqrestore(&g_fd_lock, flags);
    return fd;
}

/* 关闭指定任务的 fd，调用方已持锁 */
static int fd_close_for_locked(task_t *t, int fd, vfs_file_t **vfp, fd_entry_t **ep)
{
    fd_entry_t *e = t->fds[fd];

    *vfp = NULL;
    *ep = NULL;
    if (e == NULL)
        return -1;
    t->fds[fd] = NULL;
    if (--e->refs == 0)
    {
        *vfp = e->vf;
        *ep = e;
    }
    return 0;
}

int fd_close(int fd)
{
    task_t *t;
    fd_entry_t *e = NULL;
    vfs_file_t *vf = NULL;
    uint64_t flags;
    int rc;

    if (fd < 0 || fd >= FD_MAX)
        return VFS_ERR_INVAL;
    t = sched_current();
    if (t == NULL)
        return VFS_ERR_INVAL;
    flags = spin_lock_irqsave(&g_fd_lock);
    rc = fd_close_for_locked(t, fd, &vf, &e);
    spin_unlock_irqrestore(&g_fd_lock, flags);
    if (rc != 0)
        return VFS_ERR_INVAL; /* EBADF */

    /* 引用归零才真正关闭，锁外执行 */
    if (vf != NULL)
    {
        vfs_close(vf);
        kfree(e);
    }
    return VFS_OK;
}

/* fork 子表复用父描述 */
void fd_clone_table(task_t *dst, task_t *src)
{
    uint64_t flags;
    int i;

    if (dst == NULL || src == NULL || dst == src)
        return;
    flags = spin_lock_irqsave(&g_fd_lock);
    for (i = 0; i < FD_MAX; i++)
    {
        dst->fds[i] = src->fds[i];
        if (dst->fds[i] != NULL)
            dst->fds[i]->refs++;
    }
    spin_unlock_irqrestore(&g_fd_lock, flags);
}

/* 任务回收，逐项关闭 */
void fd_table_cleanup(task_t *t)
{
    fd_entry_t *es[FD_MAX];
    vfs_file_t *vfs[FD_MAX];
    uint64_t flags;
    int i;
    int n = 0;

    if (t == NULL)
        return;
    flags = spin_lock_irqsave(&g_fd_lock);
    for (i = 0; i < FD_MAX; i++)
    {
        vfs_file_t *vf = NULL;
        fd_entry_t *e = NULL;

        if (fd_close_for_locked(t, i, &vf, &e) != 0)
            continue;
        if (vf == NULL)
            continue;
        vfs[n] = vf;
        es[n] = e;
        n++;
    }
    spin_unlock_irqrestore(&g_fd_lock, flags);

    for (i = 0; i < n; i++)
    {
        vfs_close(vfs[i]);
        kfree(es[i]);
    }
}

int fd_dup2(int oldfd, int newfd)
{
    task_t *t;
    fd_entry_t *e;
    fd_entry_t *victim = NULL;
    vfs_file_t *vvf = NULL;
    uint64_t flags;

    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX)
        return -1;
    t = sched_current();
    if (t == NULL)
        return -1;
    flags = spin_lock_irqsave(&g_fd_lock);
    e = t->fds[oldfd];
    if (e == NULL)
    {
        spin_unlock_irqrestore(&g_fd_lock, flags);
        return -1; /* EBADF */
    }
    if (oldfd == newfd)
    {
        spin_unlock_irqrestore(&g_fd_lock, flags);
        return newfd;
    }
    /* 先摘掉新 fd 原有描述 */
    victim = t->fds[newfd];
    if (victim != NULL)
    {
        t->fds[newfd] = NULL;
        if (--victim->refs == 0)
            vvf = victim->vf;
    }
    e->refs++;
    t->fds[newfd] = e;
    spin_unlock_irqrestore(&g_fd_lock, flags);

    if (vvf != NULL)
    {
        vfs_close(vvf);
        kfree(victim);
    }
    return newfd;
}
