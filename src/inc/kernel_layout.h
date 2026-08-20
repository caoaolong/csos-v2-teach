#ifndef CSOS_KERNEL_LAYOUT_H
#define CSOS_KERNEL_LAYOUT_H

#include <stdint.h>

/* 内核高半核虚拟基址；bootstrap 仍在物理 0x100000 运行 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_PHYS_BASE 0x100000ULL

/* 由 linker.ld 提供 */
extern char __bootstrap_start[];
extern char __bootstrap_end[];
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];
extern char __kernel_virt_start[];
extern char __kernel_start[];
extern char __kernel_end[];

static inline uint64_t kernel_phys_start(void)
{
    return (uint64_t)(uintptr_t)__kernel_phys_start;
}

static inline uint64_t kernel_phys_end(void)
{
    return (uint64_t)(uintptr_t)__kernel_phys_end;
}

static inline uint64_t kernel_virt_start(void)
{
    return (uint64_t)(uintptr_t)__kernel_virt_start;
}

/* 恒等映射下物理地址可直接访问；保留便于后续去掉 identity map */
static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)phys;
}

#endif /* CSOS_KERNEL_LAYOUT_H */
