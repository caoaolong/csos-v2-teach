#include <kernel/gdt.h>
#include <kernel/kernel.h>
#include <kernel/kernel_layout.h>
#include <kernel/serial.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>
#include <abi.h>
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
        return 0; /* 回绕 */
    if (vaddr + size > (uint64_t)0x0000800000000000ULL)
        return 0; /* 须在 canonical 低半部 */
    if (vaddr + size > KERNEL_VIRT_BASE)
        return 0;
    return 1;
}

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

    /* vfs 层已按 O_RDONLY/O_WRONLY 做方向校验 */
    for (;;)
    {
        n = vfs_write(f, (const void *)(uintptr_t)ubuf, len);
        if (n != VFS_ERR_AGAIN)
            break;
        yield();
    }
    frame->rax = n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

/* pipe：rdi=int[2] 用户缓冲区，填入读/写 fd；成功 0，失败 -1 */
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
        fd_close(rfd); /* 级联 vfs_close(rf) */
        vfs_close(wf);
        frame->rax = (uint64_t)-1;
        return;
    }
    ((int *)(uintptr_t)uaddr)[0] = rfd;
    ((int *)(uintptr_t)uaddr)[1] = wfd;
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

/* fork：子任务复制父地址空间 + trap 帧 + fd 表；父返回子 pid，子返回 0 */
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

/* wait：rdi=status(out,可0)，rsi=flags(仅WNOHANG)； */
static void do_sys_wait(exception_frame_t *frame)
{
    uint64_t ustatus = frame->rdi;
    uint64_t flags = frame->rsi;
    int pid;
    int code;

    if ((flags & ~(uint64_t)WNOHANG) != 0)
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    if (ustatus != 0 && !user_mem_ok(ustatus, sizeof(int)))
    {
        frame->rax = (uint64_t)-1;
        return;
    }
    if ((flags & WNOHANG) != 0)
    {
        int r = sched_wait_reap_nb(&pid, &code);

        if (r <= 0)
        {
            frame->rax = (uint64_t)r;
            return;
        }
        if (ustatus != 0)
            *(int *)(uintptr_t)ustatus = code;
        frame->rax = (uint64_t)pid;
        return;
    }
    for (;;)
    {
        int r = sched_wait_reap(&pid, &code);

        if (r < 0)
        {
            frame->rax = (uint64_t)-1;
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

/* getpid/getppid：取当前/父任务 pid；无父返回 -1 */
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

    /* 阻塞语义：AGAIN 时 yield 重试，其余错误直接回 -1 */
    for (;;)
    {
        n = vfs_read(f, (void *)(uintptr_t)ubuf, len);
        if (n != VFS_ERR_AGAIN)
            break;
        yield();
    }
    frame->rax = n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

/* 用户路径逐字节拷入内核栈缓冲（带越界检查），要求 NUL 结尾 */
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
        return -1; /* 无 NUL：ENAMETOOLONG */
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

    /* rdx mode 保留：vfs_open 暂无 mode 参数，CREAT 用默认权限 */

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
        frame->rax = (uint64_t)-1; /* EMFILE/NOMEM */
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

/* brk：rdi=new_brk（0=查询当前值）；成功返回调整后 brk，失败返回 -1 */
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
    if (new_brk < cur->heap_base || new_brk > USER_IMAGE_END)
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

/* execve 定义在表之后，先声明 */
static void do_sys_execve(exception_frame_t *frame);

static const sys_fn_t g_sys_table[SYS_PIPE + 1] = {
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
};

/* exec 参数包：16 项 × 每项上限 256（含 NUL），与 user.h EXEC_MAX_ARGS 对齐 */
#define EXEC_ARG_BUNDLE 4096U

/* 用户指针数组逐项拷入内核 bundle，返回个数；uaddr==0 视为 0 个 */
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

/* execve 改写 trap 帧（rip/rsp）实现替换，失败时原映像完好 */
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

    /* 成功不返回：sysret 按新 rip/rsp 进入新程序 */
    frame->rip = entry;
    frame->rsp = rsp;
    frame->rax = 0;
}

void handler_syscall(exception_frame_t *frame)
{
    uint64_t num = frame->rax;
    sys_fn_t fn;

    if (num > SYS_PIPE || g_sys_table[num] == NULL)
    {
        fput_string("[SYSCALL] unknown num=%llu\n",
                    (unsigned long long)num);
        frame->rax = (uint64_t)-1; /* ENOSYS */
        return;
    }

    fn = g_sys_table[num];
    fn(frame);
}
