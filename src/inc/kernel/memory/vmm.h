#ifndef CSOS_MEMORY_VMM_H
#define CSOS_MEMORY_VMM_H

#include <kernel/memory/pmm.h>

/* 页表项标志（与 x86-64 PTE 一致） */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY (1ULL << 6)
#define PTE_HUGE (1ULL << 7) /* PD/PDPT 大页 */
#define PTE_GLOBAL (1ULL << 8)
#define PTE_NX (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* 自建 4 级页表，identity map PMM 托管的物理内存，并加载到 CR3 */
void init_vmm(boot_info_t *boot);

/*
 * 建立 / 拆除 4K 映射。
 * 成功返回 0，失败返回 -1。
 * unmap 只去掉映射，不释放物理页。
 */
int map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* 拆除 [start, end) 内 4K 用户页映射并归还物理页（页表页暂不回收）。
 * 仅处理 leaf 且置位 PTE_USER 的表项，内核页/大页一律跳过。 */
void unmap_user_range(uint64_t start, uint64_t end);

uint64_t read_cr2();
uint64_t read_cr3();

#endif /* CSOS_MEMORY_VMM_H */
