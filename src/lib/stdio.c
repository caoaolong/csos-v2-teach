#include "unistd.h"
#include "string.h"
#include "stdio.h"

static long emit(int fd, const char *s, unsigned long n, long *total)
{
    long w;

    if (n == 0)
        return 0;
    w = write(fd, s, n);
    if (w > 0)
        *total += w;
    return w;
}

static int fmt_uint(char *buf, unsigned long long v, int base, int upper)
{
    static const char dig_lo[] = "0123456789abcdef";
    static const char dig_hi[] = "0123456789ABCDEF";
    const char *dig = upper ? dig_hi : dig_lo;
    char rev[32];
    int n = 0;
    int i = 0;

    if (v == 0)
        rev[n++] = '0';
    while (v > 0 && n < 32)
    {
        rev[n++] = dig[v % (unsigned)base];
        v /= (unsigned)base;
    }
    while (n > 0)
        buf[i++] = rev[--n];
    return i;
}

int vprintf(const char *fmt, va_list ap)
{
    long total = 0;
    long w;
    char tmp[32];
    int n;

    if (fmt == NULL)
        return -1;
    for (; *fmt != '\0'; fmt++)
    {
        if (*fmt != '%')
        {
            w = emit(STDOUT_FILENO, fmt, 1, &total);
            if (w < 0)
                return -1;
            continue;
        }
        fmt++;
        {
            int longs = 0;

            while (*fmt == 'l')
            {
                longs++;
                fmt++;
            }
            switch (*fmt)
            {
            case '\0':
                goto done;
            case '%':
                w = emit(STDOUT_FILENO, "%", 1, &total);
                break;
            case 'c':
            {
                char ch = (char)va_arg(ap, int);

                w = emit(STDOUT_FILENO, &ch, 1, &total);
                break;
            }
            case 's':
            {
                const char *s = va_arg(ap, const char *);

                if (s == NULL)
                    s = "(null)";
                w = emit(STDOUT_FILENO, s, strlen(s), &total);
                break;
            }
            case 'd':
            case 'i':
            {
                long long v;

                if (longs >= 2)
                    v = va_arg(ap, long long);
                else if (longs == 1)
                    v = va_arg(ap, long);
                else
                    v = va_arg(ap, int);
                if (v < 0)
                {
                    w = emit(STDOUT_FILENO, "-", 1, &total);
                    if (w < 0)
                        return -1;
                    v = -v;
                }
                n = fmt_uint(tmp, (unsigned long long)v, 10, 0);
                w = emit(STDOUT_FILENO, tmp, (unsigned long)n, &total);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'p':
            {
                unsigned long long v;
                int base = 10;
                int upper = 0;

                if (*fmt == 'x' || *fmt == 'p')
                    base = 16;
                else if (*fmt == 'X')
                {
                    base = 16;
                    upper = 1;
                }
                else if (*fmt == 'o')
                    base = 8;
                if (*fmt == 'p')
                    v = (unsigned long long)(unsigned long)va_arg(ap, void *);
                else if (longs >= 2)
                    v = va_arg(ap, unsigned long long);
                else if (longs == 1)
                    v = va_arg(ap, unsigned long);
                else
                    v = va_arg(ap, unsigned int);
                if (*fmt == 'p')
                {
                    w = emit(STDOUT_FILENO, "0x", 2, &total);
                    if (w < 0)
                        return -1;
                }
                n = fmt_uint(tmp, v, base, upper);
                w = emit(STDOUT_FILENO, tmp, (unsigned long)n, &total);
                break;
            }
            default:
                /* 未知转换：原样输出 % 与该字符 */
                w = emit(STDOUT_FILENO, "%", 1, &total);
                if (w < 0)
                    return -1;
                w = emit(STDOUT_FILENO, fmt, 1, &total);
                break;
            }
            if (w < 0)
                return -1;
        }
    }
done:
    return (int)total;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
