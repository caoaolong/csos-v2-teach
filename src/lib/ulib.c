#include <user.h>
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

/* 结束当前任务（v1：停住不返回） */
void exit(int code)
{
    syscall1(SYS_EXIT, (long)code);
    for (;;)
    {
    }
}