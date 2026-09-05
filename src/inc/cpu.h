#ifndef CSOS_CPU_H
#define CSOS_CPU_H

#include <stdint.h>

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

/* 保存 RFLAGS 并 cli；用返回值做 irq_restore，避免嵌套时误 sti */
static inline uint64_t irq_save(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "rm"(flags)
        : "memory", "cc");
}

#endif /* CSOS_CPU_H */
