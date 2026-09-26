#include "ulib.h"

unsigned long strlen(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned long i;
    char *d = (char *)dst;
    const char *s = (const char *)src;

    for (i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *s, int c, unsigned long n)
{
    unsigned long i;
    char *p = (char *)s;

    for (i = 0; i < n; i++)
        p[i] = (char)c;
    return s;
}

/* 系统调用桩：RAX=调用号，RDI=参数，int 0x80 陷入内核 */
static long syscall1(long num, long arg)
{
    long ret;

    __asm__ volatile(
        "int $0x80\n\t"
        : "=a"(ret)
        : "a"(num), "D"(arg)
        : "memory");
    return ret;
}

/* 睡眠 ms 毫秒（由内核定时器唤醒） */
void sleep(unsigned long ms)
{
    syscall1(SYS_SLEEP, (long)ms);
}