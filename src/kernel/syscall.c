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
#include <kernel/memory/heap.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE (1ULL << 0) /* 允许 SYSCALL/SYSRET */

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

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

static uint64_t do_sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    vfs_file_t *f;
    int64_t n;

    if (len == 0)
        return 0;
    if (!user_mem_ok(ubuf, len))
        return (uint64_t)-1;

    f = fd_get((int)fd);
    if (f == NULL)
        return (uint64_t)-1; /* EBADF */

    /* vfs 层已按 O_RDONLY/O_WRONLY 做方向校验 */
    n = vfs_write(f, (const void *)(uintptr_t)ubuf, len);
    if (n < 0)
        return (uint64_t)-1;
    return (uint64_t)n;
}

static uint64_t do_sys_exit(uint64_t code, uint64_t dummy1, uint64_t dummy2)
{
    (void)dummy1;
    (void)dummy2;
    fput_string("[USER] exit(%llu), parked\n", (unsigned long long)code);
    fb_console_puts("\n[init exited]\n");
    for (;;)
        __asm__ volatile("hlt");
    return 0; /* 不可达 */
}

static uint64_t do_sys_sleep(uint64_t ms, uint64_t dummy1, uint64_t dummy2)
{
    (void)dummy1;
    (void)dummy2;
    msleep(ms);
    return 0;
}

static uint64_t do_sys_read(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    vfs_file_t *f;
    int64_t n;

    if (len == 0)
        return 0;
    if (!user_mem_ok(ubuf, len))
        return (uint64_t)-1;

    f = fd_get((int)fd);
    if (f == NULL)
        return (uint64_t)-1; /* EBADF */

    /* 阻塞语义：AGAIN 时 yield 重试，其余错误直接回 -1 */
    for (;;)
    {
        n = vfs_read(f, (void *)(uintptr_t)ubuf, len);
        if (n != VFS_ERR_AGAIN)
            break;
        yield();
    }
    if (n < 0)
        return (uint64_t)-1;
    return (uint64_t)n;
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

static uint64_t do_sys_open(uint64_t upath, uint64_t flags, uint64_t mode)
{
    char kpath[VFS_PATH_MAX];
    vfs_file_t *vf = NULL;
    int fd;
    int rc;

    (void)mode; /* 保留：vfs_open 暂无 mode 参数，CREAT 用默认权限 */

    if (copy_path_from_user(upath, kpath, (uint32_t)sizeof(kpath)) != 0)
        return (uint64_t)-1;

    rc = vfs_open(kpath, (int)flags, &vf);
    if (rc != VFS_OK)
        return (uint64_t)-1;
    fd = fd_install(vf);
    if (fd < 0)
    {
        vfs_close(vf);
        return (uint64_t)-1; /* EMFILE/NOMEM */
    }
    return (uint64_t)fd;
}

static uint64_t do_sys_close(uint64_t fd, uint64_t dummy1, uint64_t dummy2)
{
    (void)dummy1;
    (void)dummy2;
    if (fd_close((int)fd) != VFS_OK)
        return (uint64_t)-1; /* EBADF */
    return 0;
}

static uint64_t do_sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t dummy)
{
    int r;

    (void)dummy;
    r = fd_dup2((int)oldfd, (int)newfd);
    if (r < 0)
        return (uint64_t)-1; /* EBADF */
    return (uint64_t)r;
}

static uint64_t do_sys_lseek(uint64_t fd, uint64_t off, uint64_t whence)
{
    vfs_file_t *f;
    int64_t n;

    f = fd_get((int)fd);
    if (f == NULL)
        return (uint64_t)-1; /* EBADF */
    n = vfs_seek(f, (int64_t)off, (int)whence);
    if (n < 0)
        return (uint64_t)-1;
    return (uint64_t)n;
}

typedef uint64_t (*sys_fn_t)(uint64_t rdi, uint64_t rsi, uint64_t rdx);

static const sys_fn_t g_sys_table[SYS_LSEEK + 1] = {
    [SYS_WRITE] = do_sys_write,
    [SYS_EXIT] = do_sys_exit,
    [SYS_SLEEP] = do_sys_sleep,
    [SYS_READ] = do_sys_read,
    [SYS_OPEN] = do_sys_open,
    [SYS_CLOSE] = do_sys_close,
    [SYS_DUP2] = do_sys_dup2,
    [SYS_LSEEK] = do_sys_lseek,
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

/* execve 需改写 trap 帧（rip/rsp），不走通用三参表，失败时原映像完好 */
static void do_sys_execve_frame(exception_frame_t *frame)
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

    if (num == SYS_EXECVE)
    {
        do_sys_execve_frame(frame);
        return;
    }
    if (num > SYS_LSEEK || g_sys_table[num] == NULL)
    {
        fput_string("[SYSCALL] unknown num=%llu\n",
                    (unsigned long long)num);
        frame->rax = (uint64_t)-1; /* ENOSYS */
        return;
    }

    fn = g_sys_table[num];
    frame->rax = fn(frame->rdi, frame->rsi, frame->rdx);
}
