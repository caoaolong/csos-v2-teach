#ifndef CSOS_LIBC_STDIO_H
#define CSOS_LIBC_STDIO_H

#include <stdarg.h>

/* 格式化输出到 stdout：支持 %d %i %u %x %X %o %s %c %% %p */
/* 整数支持 l / ll 修饰；返回输出字符数，失败返回 -1 */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

#endif /* CSOS_LIBC_STDIO_H */
