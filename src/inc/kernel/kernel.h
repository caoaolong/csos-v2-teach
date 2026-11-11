#ifndef CSOS_KERNEL_H
#define CSOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define KERNEL_CODE_SEG (1 * 8)
#define KERNEL_DATA_SEG (2 * 8)
#define USER32_CS_SEG (3 * 8 | 3) /* 0x1B：GDT[3] 32位兼容用户代码段（仅作 STAR 基址） */
#define USER_DATA_SEG (4 * 8 | 3) /* 0x23：GDT[4] 用户数据段 DPL3 */
#define USER_CODE_SEG (5 * 8 | 3) /* 0x2B：GDT[5] 64位用户代码段 DPL3，L=1 */
#define TSS_SEG (6 * 8)           /* 0x30：GDT[6..7]，与 GDT_TSS_BASE_INDEX 一致 */

static inline uint8_t inb(uint16_t port)
{
    uint8_t rv;
    __asm__ volatile("inb %[p], %[v]" : [v] "=a"(rv) : [p] "d"(port));
    return rv;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t rv;
    __asm__ volatile("in %1, %0" : "=a"(rv) : "dN"(port));
    return rv;
}

static inline void outb(uint16_t port, uint8_t data)
{
    __asm__ volatile("outb %[v], %[p]" : : [p] "d"(port), [v] "a"(data));
}

static inline void outw(uint16_t port, uint16_t data)
{
    __asm__ volatile("outw %[v], %[p]" : : [p] "d"(port), [v] "a"(data));
}

int vsprintf(char *buf, const char *fmt, va_list args);

#endif /* CSOS_KERNEL_H */