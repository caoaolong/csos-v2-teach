#ifndef CSOS_KERNEL_H
#define CSOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

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
    __asm__ volatile("outb %[v], %[p]" : : [p] "d"(port), [v] "a"(data));
}

#endif /* CSOS_KERNEL_H */