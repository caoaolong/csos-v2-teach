#include <fd.h>
#include <abi.h>
#include <serial.h>
#include <spinlock.h>
#include <string.h>

static vfs_file_t *g_fds[FD_MAX];
static spinlock_t g_fd_lock = SPINLOCK_INIT;

void init_fd()
{
    vfs_file_t *f0 = NULL;
    vfs_file_t *f1 = NULL;
    vfs_file_t *f2 = NULL;
    int rc0, rc1, rc2;
    uint64_t flags;

    flags = spin_lock_irqsave(&g_fd_lock);
    kernel_memset(g_fds, 0, (uint32_t)sizeof(g_fds));
    spin_unlock_irqrestore(&g_fd_lock, flags);

    /* 标准流独立 open：1/2 可分别重定向，互不共享 offset */
    rc0 = vfs_open("/dev/kbd0", VFS_O_RDONLY, &f0);
    rc1 = vfs_open("/dev/console", VFS_O_WRONLY, &f1);
    rc2 = vfs_open("/dev/console", VFS_O_WRONLY, &f2);

    flags = spin_lock_irqsave(&g_fd_lock);
    if (rc0 == VFS_OK)
        g_fds[STDIN_FILENO] = f0;
    if (rc1 == VFS_OK)
        g_fds[STDOUT_FILENO] = f1;
    if (rc2 == VFS_OK)
        g_fds[STDERR_FILENO] = f2;
    spin_unlock_irqrestore(&g_fd_lock, flags);

    if (rc0 != VFS_OK || rc1 != VFS_OK || rc2 != VFS_OK)
        fput_string("[FD] stdio init rc=%d/%d/%d\n", rc0, rc1, rc2);
    else
        put_string("[FD] stdio 0=/dev/kbd0 1,2=/dev/console ready\n");
}

vfs_file_t *fd_get(int fd)
{
    vfs_file_t *f = NULL;
    uint64_t flags;

    if (fd < 0 || fd >= FD_MAX)
        return NULL;
    flags = spin_lock_irqsave(&g_fd_lock);
    f = g_fds[fd];
    spin_unlock_irqrestore(&g_fd_lock, flags);
    return f;
}
