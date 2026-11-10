#include <fd.h>
#include <abi.h>
#include <sched.h>
#include <serial.h>
#include <spinlock.h>
#include <string.h>
#include <memory/heap.h>

/*
 * 单用户任务过渡：全局锁保护当前任务表。
 * 多任务/SMP 后改为 per-task 锁，且 fd_get 需 get/put 引用
 *（否则 read/write 取到 vf 后被他核 close 释放会有 UAF）。
 */
static spinlock_t g_fd_lock = SPINLOCK_INIT;

/* 调用者已持锁：包装 vf 并装入最小空闲槽 */
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
    return -1; /* EMFILE：表满 */
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

    /* task_create 已清零 fds，这里只做 open（open 失败的槽保持 NULL） */
    rc0 = vfs_open("/dev/kbd0", VFS_O_RDONLY, &f0);
    rc1 = vfs_open("/dev/console", VFS_O_WRONLY, &f1);
    rc2 = vfs_open("/dev/console", VFS_O_WRONLY, &f2);

    flags = spin_lock_irqsave(&g_fd_lock);
    /* 标准流固定占 0/1/2，不走最小空闲分配 */
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

int fd_close(int fd)
{
    task_t *t;
    fd_entry_t *e;
    vfs_file_t *vf = NULL;
    uint64_t flags;

    if (fd < 0 || fd >= FD_MAX)
        return VFS_ERR_INVAL;
    t = sched_current();
    if (t == NULL)
        return VFS_ERR_INVAL;
    flags = spin_lock_irqsave(&g_fd_lock);
    e = t->fds[fd];
    if (e == NULL)
    {
        spin_unlock_irqrestore(&g_fd_lock, flags);
        return VFS_ERR_INVAL; /* EBADF */
    }
    t->fds[fd] = NULL;
    if (--e->refs == 0)
        vf = e->vf;
    spin_unlock_irqrestore(&g_fd_lock, flags);

    /* 引用归零才真正 vfs_close + 释放；锁外做，避免嵌套 devfs 锁 */
    if (vf != NULL)
    {
        vfs_close(vf);
        kfree(e);
    }
    return VFS_OK;
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
    /* 先摘掉 newfd 原有描述（引用归零的延迟到锁外释放） */
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
