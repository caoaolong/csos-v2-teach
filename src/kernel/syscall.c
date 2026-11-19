#include <kernel/gdt.h>
#include <kernel/kernel.h>
#include <kernel/kernel_layout.h>
#include <kernel/serial.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>
#include <abi.h>
#include <dirent.h>
#include <kernel/fd.h>
#include <kernel/sched.h>
#include <kernel/fs/vfs.h>
#include <kernel/gfx/console.h>
#include <kernel/user.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/fs/pipe.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE (1ULL << 0) /* 允许 SYSCALL/SYSRET */

void init_syscall()
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR,
          ((uint64_t)USER32_CS_SEG << 48) |
              ((uint64_t)KERNEL_CODE_SEG << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, 0);

    fput_string("[SYSCALL] syscall/sysret ready lstar=0x%llx\n",
                (unsigned long long)(uintptr_t)syscall_entry);
}

static int user_mem_ok(uint64_t vaddr, uint64_t size)
{
    if (size == 0)
        return 1;
    if (vaddr < PAGE_SIZE)
        return 0;
    if (vaddr + size < vaddr)
        return 0; /* 地址回绕 */
    if (vaddr + size > (uint64_t)0x0000800000000000ULL)
        return 0; /* 须在低半部 */
    if (vaddr + size > KERNEL_VIRT_BASE)
        return 0;
    return 1;
}

/* 用户路径拷入内核缓冲，要求 NUL 结尾 */
static int copy_path_from_user(uint64_t upath, char *kpath, uint32_t cap);

static void do_sys_write(exception_frame_t *frame)
{
    vfs_file_t *f;
    int64_t n;
    uint64_t fd = frame->rdi;
    uint64_t ubuf = frame->rsi;
    uint64_t len = frame->rdx;

    if (len == 0)
    {
        frame->rax = 0;
        return;
    }
    if (!user_mem_ok(ubuf, len))
    {
        frame->rax = (uint64_t)-1;
        return;
    }

    f = fd_get((int)fd);
    if (f == NULL)
    {
        frame->rax = (uint64_t)-1; /* EBADF */
        return;
    }

    /* vfs 层已做方向校验 */
    /* 非阻塞时 AGAIN 直接返回 */
    for (;;)
    {
        n = vfs_write(f, (const void *)(uintptr_t)ubuf, len);
        if (n != VFS_ERR_AGAIN)
            break;
        if (f->flags & VFS_O_NONBLOCK)
        {
            frame->rax = (uint64_t)-EAGAIN;
            return;
        }
        yield();
    }
    frame->rax = n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

/* pipe：填入读端与写端 fd */
static void do_sys_pipe(exception_frame_t *frame)
{
    uint64_t uaddr = frame->rdi;
    vfs_file_t *rf = NULL;
    vfs_file_t *wf = NULL;
    int rfd;
    int wfd;

    if (!user_mem_ok(uaddr, sizeof(int) * 2))
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    if (pipe_create(&rf, &wf) != VFS_OK)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    rfd = fd_install(rf);
    if (rfd < 0)
    {
        vfs_close(rf);
        vfs_close(wf);
        frame->rax = (uint64_t)-1;
        return;
    }
    wfd = fd_install(wf);
    if (wfd < 0)
    {
        fd_close(rfd); /* 级联关闭读端 */
        vfs_close(wf);
        frame->rax = (uint64_t)-1;
        return;
    }
    ((int *)(uintptr_t)uaddr)[0] = rfd;
    ((int *)(uintptr_t)uaddr)[1] = wfd;
    frame->rax = 0;
}

/* mkfifo：在 /pipe 下创建命名管道 */
static void do_sys_mkfifo(exception_frame_t *frame)
{
    char kpath[VFS_PATH_MAX];
    int rc;

    if (copy_path_from_user(frame->rdi, kpath, (uint32_t)sizeof(kpath)) != 0)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    rc = vfs_mkfifo(kpath);
    if (rc == VFS_OK)
    {
        frame->rax = 0;
        return;
    }
    if (rc == VFS_ERR_EXIST)
        frame->rax = (uint64_t)-EEXIST;
    else if (rc == VFS_ERR_NOENT)
        frame->rax = (uint64_t)-ENOENT;
    else if (rc == VFS_ERR_NOMEM)
        frame->rax = (uint64_t)-ENOMEM;
    else
        frame->rax = (uint64_t)-EINVAL;
}

/* chdir：切换全局工作目录（单用户教学 OS，无 per-task cwd） */
static void do_sys_chdir(exception_frame_t *frame)
{
    char kpath[VFS_PATH_MAX];
    int rc;

    if (copy_path_from_user(frame->rdi, kpath, (uint32_t)sizeof(kpath)) != 0)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    rc = vfs_chdir(kpath);
    if (rc == VFS_OK)
    {
        frame->rax = 0;
        return;
    }
    if (rc == VFS_ERR_NOENT)
        frame->rax = (uint64_t)-ENOENT;
    else if (rc == VFS_ERR_NOTDIR)
        frame->rax = (uint64_t)-ENOTDIR;
    else if (rc == VFS_ERR_NOMEM)
        frame->rax = (uint64_t)-ENOMEM;
    else
        frame->rax = (uint64_t)-EINVAL;
}

/* getcwd：拷出当前目录，要求 NUL（含）能装下 */
static void do_sys_getcwd(exception_frame_t *frame)
{
    uint64_t ubuf = frame->rdi;
    uint64_t size = frame->rsi;
    char kbuf[VFS_PATH_MAX];
    uint32_t i;
    int rc;

    if (size == 0 || size > VFS_PATH_MAX)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (!user_mem_ok(ubuf, size))
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    rc = vfs_getcwd(kbuf, (uint32_t)sizeof(kbuf));
    if (rc == VFS_OK)
    {
        for (i = 0; kbuf[i] != '\0'; i++)
            ((char *)(uintptr_t)ubuf)[i] = kbuf[i];
        if ((uint64_t)i + 1 > size)
        {
            frame->rax = (uint64_t)-ERANGE;
            return;
        }
        ((char *)(uintptr_t)ubuf)[i] = '\0';
        frame->rax = 0;
        return;
    }
    if (rc == VFS_ERR_RANGE)
        frame->rax = (uint64_t)-ERANGE;
    else
        frame->rax = (uint64_t)-EINVAL;
}

/* getdents：一口气列出 path 下目录项（小目录教学实现，无偏移续读） */
static void do_sys_getdents(exception_frame_t *frame)
{
    char kpath[VFS_PATH_MAX];
    uint64_t ubuf = frame->rsi;
    uint64_t cap = frame->rdx;
    csos_dirent_t *u = (csos_dirent_t *)(uintptr_t)ubuf;
    vfs_dir_t *d = NULL;
    uint64_t n = 0;
    int rc;

    if (cap == 0 || cap > 1024)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (!user_mem_ok(ubuf, cap * sizeof(csos_dirent_t)))
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (copy_path_from_user(frame->rdi, kpath, (uint32_t)sizeof(kpath)) != 0)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    rc = vfs_opendir(kpath, &d);
    if (rc != VFS_OK)
    {
        if (rc == VFS_ERR_NOENT)
            frame->rax = (uint64_t)-ENOENT;
        else if (rc == VFS_ERR_NOTDIR)
            frame->rax = (uint64_t)-ENOTDIR;
        else if (rc == VFS_ERR_NOMEM)
            frame->rax = (uint64_t)-ENOMEM;
        else
            frame->rax = (uint64_t)-EINVAL;
        return;
    }
    for (; n < cap; n++)
    {
        vfs_dirent_t e;
        uint32_t i;

        rc = vfs_readdir(d, &e);
        if (rc != VFS_OK)
            break; /* 末尾（NOENT）或驱动错误都收尾 */
        for (i = 0; i < CSOS_NAME_MAX; i++)
        {
            u[n].name[i] = e.name[i];
            if (e.name[i] == '\0')
                break;
        }
        u[n].name[CSOS_NAME_MAX - 1] = '\0';
        u[n].is_dir = e.is_dir;
        u[n].size = e.size;
    }
    vfs_closedir(d);
    frame->rax = n;
}

/* fbmap：映射显存到用户地址空间 */
static void do_sys_fbmap(exception_frame_t *frame)
{
    task_t *cur = sched_current();
    uint64_t base;
    uint64_t size;
    uint64_t pa_start;
    uint64_t off;
    uint64_t map_len;
    uint64_t va;
    uint64_t irqf;

    if (cur == NULL || cur->pgdir == NULL)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    vmm_framebuffer_info(&base, &size, NULL, NULL, NULL, NULL, NULL);
    if (base == 0 || size == 0)
    {
        frame->rax = (uint64_t)-1; /* 无显存 */
        return;
    }
    pa_start = base & ~(PAGE_SIZE - 1);
    off = base - pa_start;
    map_len = (off + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (off + size < off || off + size > USER_FB_MAX_SIZE)
    {
        frame->rax = (uint64_t)-1; /* 显存超出预留窗 */
        return;
    }
    irqf = irq_save();
    for (va = USER_FB_BASE; va < USER_FB_BASE + map_len; va += PAGE_SIZE)
    {
        /* MMIO 页只拆映射不释放 */
        if (map_page(va, pa_start + (va - USER_FB_BASE),
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                         PTE_WRITE_THROUGH | PTE_GLOBAL) != 0)
            break;
    }
    if (va != USER_FB_BASE + map_len)
    {
        unmap_user_range(USER_FB_BASE, va); /* 回滚本轮已映射页 */
        irq_restore(irqf);
        frame->rax = (uint64_t)-1;
        return;
    }
    irq_restore(irqf);
    frame->rax = USER_FB_BASE + off;
}

/* fbctl：屏幕归属仲裁 */
static void do_sys_fbctl(exception_frame_t *frame)
{
    uint64_t op = frame->rdi;

    if (op == FB_ACQUIRE)
        fb_console_detach();
    else if (op == FB_RELEASE)
        fb_console_attach();
    else
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    frame->rax = 0;
}

static void do_sys_exit(exception_frame_t *frame)
{
    fput_string("[USER] task '%s' exit(%llu)\n",
                sched_current() != NULL && sched_current()->name != NULL
                    ? sched_current()->name
                    : "?",
                (unsigned long long)frame->rdi);
    task_exit_current((int)frame->rdi);
}

/* fork：复制地址空间与 fd 表，父返回子 pid */
static void do_sys_fork(exception_frame_t *frame)
{
    task_t *cur = sched_current();
    task_t *child;
    uint64_t *pgdir;

    if (cur == NULL || cur->pgdir == NULL)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    child = task_alloc_fork(frame);
    if (child == NULL)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    pgdir = vmm_new_user_pml4(USER_IMAGE_BASE, USER_IMAGE_END);
    if (pgdir == NULL)
    {
        task_abort_fork(child);
        frame->rax = (uint64_t)-1;
        return;
    }
    if (vmm_clone_user_range(pgdir, cur->pgdir, USER_IMAGE_BASE, USER_IMAGE_END) != 0)
    {
        vmm_free_user_pml4(pgdir, USER_IMAGE_BASE, USER_IMAGE_END);
        task_abort_fork(child);
        frame->rax = (uint64_t)-1;
        return;
    }
    child->pgdir = pgdir;
    child->heap_base = cur->heap_base;
    child->heap_brk = cur->heap_brk;
    fd_clone_table(child, cur);
    task_enqueue(child);
    frame->rax = (uint64_t)child->pid;
}

/* wait：等待子进程退出 */
static void do_sys_wait(exception_frame_t *frame)
{
    uint64_t ustatus = frame->rdi;
    int pid;
    int code;

    if (frame->rsi != 0)
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if (ustatus != 0 && !user_mem_ok(ustatus, sizeof(int)))
    {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    for (;;)
    {
        int r = sched_wait_reap(&pid, &code);

        if (r < 0)
        {
            frame->rax = (uint64_t)-ECHILD;
            return;
        }
        if (r > 0)
        {
            if (ustatus != 0)
                *(int *)(uintptr_t)ustatus = code;
            frame->rax = (uint64_t)pid;
            return;
        }
        yield();
    }
}

/* 取当前或父进程 pid */
static void do_sys_getpid(exception_frame_t *frame)
{
    task_t *cur = sched_current();

    frame->rax = cur != NULL ? (uint64_t)cur->pid : (uint64_t)-1;
}

static void do_sys_getppid(exception_frame_t *frame)
{
    task_t *cur = sched_current();

    frame->rax = (cur != NULL && cur->parent != NULL)
                     ? (uint64_t)cur->parent->pid
                     : (uint64_t)-1;
}

static void do_sys_sleep(exception_frame_t *frame)
{
    msleep(frame->rdi);
    frame->rax = 0;
}

static void do_sys_read(exception_frame_t *frame)
{
    vfs_file_t *f;
    int64_t n;
    uint64_t fd = frame->rdi;
    uint64_t ubuf = frame->rsi;
    uint64_t len = frame->rdx;

    if (len == 0)
    {
        frame->rax = 0;
        return;
    }
    if (!user_mem_ok(ubuf, len))
    {
        frame->rax = (uint64_t)-1;
        return;
    }

    f = fd_get((int)fd);
    if (f == NULL)
    {
        frame->rax = (uint64_t)-1; /* EBADF */
        return;
    }

    /* 阻塞时 AGAIN 重试，非阻塞直接返回 */
    for (;;)
    {
        n = vfs_read(f, (void *)(uintptr_t)ubuf, len);
        if (n != VFS_ERR_AGAIN)
            break;
        if (f->flags & VFS_O_NONBLOCK)
        {
            frame->rax = (uint64_t)-EAGAIN;
            return;
        }
        yield();
    }
    frame->rax = n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

/* 用户路径拷入内核缓冲，要求 NUL 结尾 */
static int copy_path_from_user(uint64_t upath, char *kpath, uint32_t cap)
{
    uint32_t i;

    for (i = 0; i < cap; i++)
    {
        if (!user_mem_ok(upath + i, 1))
            return -1;
        kpath[i] = *(const char *)(uintptr_t)(upath + i);
        if (kpath[i] == '\0')
            break;
    }
    if (i >= cap)
        return -1; /* 缺 NUL 结尾 */
    if (i == 0)
        return -1; /* 空路径 */
    return 0;
}

static void do_sys_open(exception_frame_t *frame)
{
    char kpath[VFS_PATH_MAX];
    vfs_file_t *vf = NULL;
    int fd;
    int rc;
    uint64_t flags = frame->rsi;

    /* mode 参数暂保留 */
    if (copy_path_from_user(frame->rdi, kpath, (uint32_t)sizeof(kpath)) != 0)
    {
        frame->rax = (uint64_t)-1;
        return;
    }

    rc = vfs_open(kpath, (int)flags, &vf);
    if (rc != VFS_OK)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    fd = fd_install(vf);
    if (fd < 0)
    {
        vfs_close(vf);
        frame->rax = (uint64_t)-1; /* 表满或无内存 */
        return;
    }
    frame->rax = (uint64_t)fd;
}

static void do_sys_close(exception_frame_t *frame)
{
    if (fd_close((int)frame->rdi) != VFS_OK)
        frame->rax = (uint64_t)-1; /* EBADF */
    else
        frame->rax = 0;
}

static void do_sys_dup2(exception_frame_t *frame)
{
    int r = fd_dup2((int)frame->rdi, (int)frame->rsi);

    frame->rax = r < 0 ? (uint64_t)-1 : (uint64_t)r; /* EBADF */
}

static void do_sys_lseek(exception_frame_t *frame)
{
    vfs_file_t *f;
    int64_t n;

    f = fd_get((int)frame->rdi);
    if (f == NULL)
    {
        frame->rax = (uint64_t)-1; /* EBADF */
        return;
    }
    n = vfs_seek(f, (int64_t)frame->rsi, (int)frame->rdx);
    frame->rax = n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

/* brk：调整堆顶，失败返回 -1 */
static void do_sys_brk(exception_frame_t *frame)
{
    task_t *cur = sched_current();
    uint64_t new_brk = frame->rdi;
    uint64_t old_brk;
    uint64_t old_top;
    uint64_t new_top;
    uint64_t va;
    uint64_t irqf;

    if (cur == NULL || cur->pgdir == NULL)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    if (new_brk == 0)
    {
        frame->rax = cur->heap_brk;
        return;
    }
    if (new_brk < cur->heap_base || new_brk > USER_FB_BASE)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    old_brk = cur->heap_brk;
    if (new_brk == old_brk)
    {
        frame->rax = old_brk;
        return;
    }
    old_top = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    new_top = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_top == old_top)
    {
        cur->heap_brk = new_brk;
        frame->rax = new_brk;
        return;
    }
    irqf = irq_save();
    if (new_top > old_top)
    {
        for (va = old_top; va < new_top; va += PAGE_SIZE)
        {
            uint64_t pa = alloc_page();

            if (pa == PMM_INVALID_ADDR || pa == 0)
                break;
            kernel_memset((void *)(uintptr_t)pa, 0, (uint32_t)PAGE_SIZE);
            if (map_page(va, pa, PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0)
            {
                free_page(pa);
                break;
            }
        }
        if (va != new_top)
        {
            unmap_user_range(old_top, va); /* 回滚本轮已映射页 */
            irq_restore(irqf);
            frame->rax = (uint64_t)-1;
            return;
        }
    }
    else
    {
        unmap_user_range(new_top, old_top);
    }
    irq_restore(irqf);
    cur->heap_brk = new_brk;
    frame->rax = new_brk;
}

typedef void (*sys_fn_t)(exception_frame_t *frame);

/* execve 在表后定义，先声明 */
static void do_sys_execve(exception_frame_t *frame);

static const sys_fn_t g_sys_table[SYS_GETDENTS + 1] = {
    [SYS_WRITE] = do_sys_write,
    [SYS_EXIT] = do_sys_exit,
    [SYS_SLEEP] = do_sys_sleep,
    [SYS_READ] = do_sys_read,
    [SYS_OPEN] = do_sys_open,
    [SYS_CLOSE] = do_sys_close,
    [SYS_DUP2] = do_sys_dup2,
    [SYS_LSEEK] = do_sys_lseek,
    [SYS_EXECVE] = do_sys_execve,
    [SYS_FORK] = do_sys_fork,
    [SYS_WAIT] = do_sys_wait,
    [SYS_GETPID] = do_sys_getpid,
    [SYS_GETPPID] = do_sys_getppid,
    [SYS_BRK] = do_sys_brk,
    [SYS_PIPE] = do_sys_pipe,
    [SYS_MKFIFO] = do_sys_mkfifo,
    [SYS_FBMAP] = do_sys_fbmap,
    [SYS_FBCTL] = do_sys_fbctl,
    [SYS_CHDIR] = do_sys_chdir,
    [SYS_GETCWD] = do_sys_getcwd,
    [SYS_GETDENTS] = do_sys_getdents,
};

/* exec 参数包上限 4KB */
#define EXEC_ARG_BUNDLE 4096U

/* 用户参数拷入内核，返回个数 */
static int copy_args_from_user(uint64_t uaddr, char *bundle, char *kptrs[])
{
    int n = 0;
    uint32_t off = 0;

    if (uaddr == 0)
        return 0;
    for (;;)
    {
        uint64_t base = uaddr + (uint64_t)n * 8ULL;
        uint64_t v;
        uint32_t i;

        if (n >= EXEC_MAX_ARGS)
            return -1;
        if (!user_mem_ok(base, 8))
            return -1;
        v = *(const uint64_t *)(uintptr_t)base;
        if (v == 0)
            break;
        if (off + VFS_PATH_MAX > EXEC_ARG_BUNDLE)
            return -1;
        for (i = 0; i < (uint32_t)VFS_PATH_MAX; i++)
        {
            if (!user_mem_ok(v + i, 1))
                return -1;
            bundle[off + i] = *(const char *)(uintptr_t)(v + i);
            if (bundle[off + i] == '\0')
                break;
        }
        if (i >= (uint32_t)VFS_PATH_MAX)
            return -1; /* 单项超限 */
        kptrs[n++] = bundle + off;
        off += i + 1;
    }
    return n;
}

/* execve 改写陷阱帧实现替换 */
static void do_sys_execve(exception_frame_t *frame)
{
    char kpath[VFS_PATH_MAX];
    char *kabundle;
    char *kenbundle;
    char *kargv[EXEC_MAX_ARGS];
    char *kenvp[EXEC_MAX_ARGS];
    int kargc;
    int kenvc;
    uint64_t entry;
    uint64_t rsp;

    if (copy_path_from_user(frame->rdi, kpath, (uint32_t)sizeof(kpath)) != 0)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    kabundle = (char *)kmalloc(EXEC_ARG_BUNDLE);
    kenbundle = (char *)kmalloc(EXEC_ARG_BUNDLE);
    if (kabundle == NULL || kenbundle == NULL)
    {
        if (kabundle != NULL)
            kfree(kabundle);
        if (kenbundle != NULL)
            kfree(kenbundle);
        frame->rax = (uint64_t)-1;
        return;
    }
    kargc = copy_args_from_user(frame->rsi, kabundle, kargv);
    kenvc = copy_args_from_user(frame->rdx, kenbundle, kenvp);
    if (kargc < 0 || kenvc < 0 ||
        user_exec_replace(kpath, kargv, kargc, kenvp, kenvc, &entry, &rsp) != 0)
    {
        kfree(kabundle);
        kfree(kenbundle);
        frame->rax = (uint64_t)-1;
        return;
    }
    kfree(kabundle);
    kfree(kenbundle);

    /* 成功后按新地址进入新程序 */
    frame->rip = entry;
    frame->rsp = rsp;
    frame->rax = 0;
}

void handler_syscall(exception_frame_t *frame)
{
    uint64_t num = frame->rax;
    sys_fn_t fn;

    if (num > SYS_GETDENTS || g_sys_table[num] == NULL)
    {
        fput_string("[SYSCALL] unknown num=%llu\n",
                    (unsigned long long)num);
        frame->rax = (uint64_t)-1; /* 无效系统调用号 */
        return;
    }

    fn = g_sys_table[num];
    fn(frame);
}
