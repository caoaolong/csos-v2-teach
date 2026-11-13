#include <abi.h>
#include "unistd.h"

static long syscall1(long num, long arg)
{
    long ret;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg)
        : "rcx", "r11", "memory");
    return ret;
}

/* 睡眠 ms 毫秒（由内核定时器唤醒） */
void sleep(unsigned long ms)
{
    syscall1(SYS_SLEEP, (long)ms);
}

static long syscall3(long num, long a, long b, long c)
{
    long ret;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

/* 写串口+帧缓冲：返回写入字节数，<0 出错 */
long write(int fd, const void *buf, unsigned long len)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)len);
}

/* 读 fd=0（键盘）：返回实际字节数，阻塞直到有数据；<0 出错 */
long read(int fd, void *buf, unsigned long len)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)len);
}

/* 结束当前任务（v1：停住不返回） */
void exit(int code)
{
    syscall1(SYS_EXIT, (long)code);
    for (;;)
    {
    }
}

/* 打开路径：flags 取 O_*；成功返回 fd（>=0），失败返回 -1 */
long open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, (long)flags, 0);
}

/* 关闭 fd：成功返回 0，失败返回 -1（EBADF） */
long close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/* oldfd 共享到 newfd：成功返回 newfd，失败返回 -1 */
long dup2(int oldfd, int newfd)
{
    return syscall3(SYS_DUP2, (long)oldfd, (long)newfd, 0);
}

/* 定位：成功返回新位置，失败返回 -1（字符设备等不可 seek） */
long lseek(int fd, long offset, int whence)
{
    return syscall3(SYS_LSEEK, (long)fd, offset, (long)whence);
}

/* 替换为 path 指定的程序：成功不返回，失败返回 -1（原程序不受影响） */
long execve(const char *path, char *const argv[], char *const envp[])
{
    return syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

/* 复制当前任务：父返回子 pid，子返回 0，失败返回 -1 */
long fork(void)
{
    return syscall1(SYS_FORK, 0);
}

/* 阻塞回收一个已退出子进程：status 回填 exit 码（可传 0）；无子返回 -1 */
long wait(int *status)
{
    return waitpid(-1, status, 0);
}

/* 回收子进程：仅支持 pid=-1（任意子进程）；options 仅支持 WNOHANG */
long waitpid(int pid, int *status, int options)
{
    if (pid != -1)
        return -1;
    return syscall3(SYS_WAIT, (long)status, (long)options, 0);
}

/* 取当前任务 pid；失败返回 -1 */
long getpid(void)
{
    return syscall1(SYS_GETPID, 0);
}

/* 取父任务 pid；无父返回 -1 */
long getppid(void)
{
    return syscall1(SYS_GETPPID, 0);
}

/* 堆顶设为 addr：成功返回 0，失败返回 -1 */
long brk(void *addr)
{
    long r = syscall1(SYS_BRK, (long)addr);

    return r < 0 ? -1 : 0;
}

/* 匿名管道：fds[0]=读端，fds[1]=写端；成功 0，失败 -1 */
long pipe(int fds[2])
{
    return syscall3(SYS_PIPE, (long)fds, 0, 0);
}

/* 堆顶增 incr 字节并返回旧堆顶；incr=0 查询；失败返回 (void *)-1 */
void *sbrk(long incr)
{
    long cur = syscall1(SYS_BRK, 0);
    long req;

    if (cur < 0)
        return (void *)-1;
    if (incr == 0)
        return (void *)cur;
    req = cur + incr;
    if (req <= 0 || (incr > 0 && req < cur) || (incr < 0 && req > cur))
        return (void *)-1; /* 回绕/越过零页：内核侧必拒，提前拦 */
    if (syscall1(SYS_BRK, req) < 0)
        return (void *)-1;
    return (void *)cur;
}
