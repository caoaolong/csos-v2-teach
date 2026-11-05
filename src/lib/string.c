#include "string.h"

unsigned long strlen(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

/* 标准语义：至多写 n 字节，不足补 '\0'；src（含 '\0'）长度 ≥ n 时不封口 */
char *strncpy(char *dst, const char *src, unsigned long n)
{
    unsigned long i = 0;

    while (i < n && src[i] != '\0')
    {
        dst[i] = src[i];
        i++;
    }
    while (i < n)
        dst[i++] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    while (n > 0)
    {
        if (*a != *b)
            return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
        n--;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    while (*s != '\0')
    {
        if (*s == ch)
            return (char *)s;
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)NULL;
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

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    while (n > 0)
    {
        if (*p != *q)
            return (int)*p - (int)*q;
        p++;
        q++;
        n--;
    }
    return 0;
}
