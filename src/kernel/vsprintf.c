#include <kernel/kernel.h>
#include <kernel/string.h>

/* 不依赖 ctype 库 */
#define is_digit(c) ((c) >= '0' && (c) <= '9')

static int skip_atoi(const char **s)
{
    int i = 0;

    while (is_digit(**s))
        i = i * 10 + *((*s)++) - '0';
    return i;
}

#define ZEROPAD 1  /* 补零 */
#define SIGN 2     /* 有符号长整型 */
#define PLUS 4     /* 显示加号 */
#define SPACE 8    /* 加号位补空格 */
#define LEFT 16    /* 左对齐 */
#define SPECIAL 32 /* 加 0x 前缀 */
#define SMALL 64   /* 小写十六进制 */

/* 64 位无符号格式化 */
static char *number(char *str, unsigned long long num, int base, int size, int precision, int type)
{
    char c, sign, tmp[66];
    const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;

    if (type & SMALL)
        digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (type & LEFT)
        type &= ~ZEROPAD;
    if (base < 2 || base > 36)
        return 0;
    c = (type & ZEROPAD) ? '0' : ' ';
    if (type & SIGN && (long long)num < 0)
    {
        sign = '-';
        num = -(long long)num;
    }
    else
        sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
    if (sign)
        size--;
    if (type & SPECIAL)
        if (base == 16)
            size -= 2;
        else if (base == 8)
            size--;
    i = 0;
    if (num == 0)
        tmp[i++] = '0';
    else
        while (num != 0)
        {
            tmp[i++] = digits[num % (unsigned)base];
            num /= (unsigned)base;
        }
    if (i > precision)
        precision = i;
    size -= precision;
    if (!(type & (ZEROPAD + LEFT)))
        while (size-- > 0)
            *str++ = ' ';
    if (sign)
        *str++ = sign;
    if (type & SPECIAL)
        if (base == 8)
            *str++ = '0';
        else if (base == 16)
        {
            *str++ = '0';
            *str++ = digits[33];
        }
    if (!(type & LEFT))
        while (size-- > 0)
            *str++ = c;
    while (i < precision--)
        *str++ = '0';
    while (i-- > 0)
        *str++ = tmp[i];
    while (size-- > 0)
        *str++ = ' ';
    return str;
}

/* 按限定符取无符号参数 */
static unsigned long long get_uint_arg(va_list args, int qualifier)
{
    if (qualifier == 'L') /* ll */
        return va_arg(args, unsigned long long);
    if (qualifier == 'l')
        return va_arg(args, unsigned long);
    return va_arg(args, unsigned int);
}

static long long get_int_arg(va_list args, int qualifier)
{
    if (qualifier == 'L') /* ll */
        return va_arg(args, long long);
    if (qualifier == 'l')
        return va_arg(args, long);
    return va_arg(args, int);
}

int vsprintf(char *buf, const char *fmt, va_list args)
{
    int len;
    int i;
    char *str;
    char *s;
    int *ip;

    int flags; /* 传给 number 的标志 */

    int field_width; /* 输出字段宽度 */
    int precision;   /* 整数最小位数，字符串最大字符数 */
    int qualifier;   /* 整数长度修饰符 */

    for (str = buf; *fmt; ++fmt)
    {
        if (*fmt != '%')
        {
            *str++ = *fmt;
            continue;
        }

        /* 解析标志 */
        flags = 0;
    repeat:
        ++fmt; /* 跳过首个 % */
        switch (*fmt)
        {
        case '-':
            flags |= LEFT;
            goto repeat;
        case '+':
            flags |= PLUS;
            goto repeat;
        case ' ':
            flags |= SPACE;
            goto repeat;
        case '#':
            flags |= SPECIAL;
            goto repeat;
        case '0':
            flags |= ZEROPAD;
            goto repeat;
        }

        /* 取字段宽度 */
        field_width = -1;
        if (is_digit(*fmt))
            field_width = skip_atoi(&fmt);
        else if (*fmt == '*')
        {
            /* 宽度取自下一参数 */
            field_width = va_arg(args, int);
            if (field_width < 0)
            {
                field_width = -field_width;
                flags |= LEFT;
            }
        }

        /* 取精度 */
        precision = -1;
        if (*fmt == '.')
        {
            ++fmt;
            if (is_digit(*fmt))
                precision = skip_atoi(&fmt);
            else if (*fmt == '*')
            {
                /* 精度取自下一参数 */
                precision = va_arg(args, int);
            }
            if (precision < 0)
                precision = 0;
        }

        /* 取转换限定符，支持 l/ll */
        qualifier = -1;
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L')
        {
            qualifier = *fmt;
            ++fmt;
            if (qualifier == 'l' && *fmt == 'l')
            {
                qualifier = 'L';
                ++fmt;
            }
        }

        switch (*fmt)
        {
        case 'c':
            if (!(flags & LEFT))
                while (--field_width > 0)
                    *str++ = ' ';
            *str++ = (unsigned char)va_arg(args, int);
            while (--field_width > 0)
                *str++ = ' ';
            break;

        case 's':
            s = va_arg(args, char *);
            len = kernel_strlen(s);
            if (precision < 0)
                precision = len;
            else if (len > precision)
                len = precision;

            if (!(flags & LEFT))
                while (len < field_width--)
                    *str++ = ' ';
            for (i = 0; i < len; ++i)
                *str++ = *s++;
            while (len < field_width--)
                *str++ = ' ';
            break;

        case 'o':
            str = number(str, get_uint_arg(args, qualifier), 8,
                         field_width, precision, flags);
            break;

        case 'p':
            if (field_width == -1)
            {
                field_width = 16;
                flags |= ZEROPAD;
            }
            str = number(str,
                         (unsigned long long)(uintptr_t)va_arg(args, void *), 16,
                         field_width, precision, flags);
            break;

        case 'x':
            flags |= SMALL;
        case 'X':
            str = number(str, get_uint_arg(args, qualifier), 16,
                         field_width, precision, flags);
            break;

        case 'd':
        case 'i':
            flags |= SIGN;
            str = number(str, (unsigned long long)get_int_arg(args, qualifier), 10,
                         field_width, precision, flags);
            break;

        case 'u':
            str = number(str, get_uint_arg(args, qualifier), 10,
                         field_width, precision, flags);
            break;

        case 'n':
            ip = va_arg(args, int *);
            *ip = (str - buf);
            break;

        default:
            if (*fmt != '%')
                *str++ = '%';
            if (*fmt)
                *str++ = *fmt;
            else
                --fmt;
            break;
        }
    }
    *str = '\0';
    return str - buf;
}