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