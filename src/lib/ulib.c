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
