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
 * 建立 / 拆除 4K 映射（作用于当前 active 根）。
 * 成功返回 0，失败返回 -1。
 * unmap 只去掉映射，不释放物理页。
 */
int map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* 地址空间根管理：master 为内核常驻表；active 为 map/unmap 的目标。
 * 调度切任务时 vmm_switch_active 同步 CR3 与 active。 */
uint64_t *vmm_master_pml4(void);
uint64_t *vmm_active_pml4(void);
void vmm_switch_active(uint64_t *root);

/* 新建用户地址空间：复制内核半 + 私有 PDPT[1] 子树骨架（用户区为空）。
 * 要求 [start, end) 落在单个 PDPT 槽内；失败返回 NULL。 */
uint64_t *vmm_new_user_pml4(uint64_t start, uint64_t end);

/* 把 src 用户区已映射页拷贝到 dst（含 USER 标志原样保留，只读段亦然）。
 * 失败返回 -1（调用方用 vmm_free_user_pml4 回滚部分拷贝）。 */
int vmm_clone_user_range(uint64_t *dst_pml4, uint64_t *src_pml4,
                         uint64_t start, uint64_t end);

/* 释放任务私有子树：USER 叶子归还物理页，页表页全部回收（共享表不动）。 */
void vmm_free_user_pml4(uint64_t *pml4, uint64_t start, uint64_t end);

/* 拆除 [start, end) 内 4K 用户页映射并归还物理页（页表页暂不回收）。
 * 仅处理 leaf 且置位 PTE_USER 的表项，内核页/大页一律跳过。 */
void unmap_user_range(uint64_t start, uint64_t end);

uint64_t read_cr2();
uint64_t read_cr3();

#endif /* CSOS_MEMORY_VMM_H */
