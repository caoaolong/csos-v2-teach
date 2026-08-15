#ifndef CSOS_MEMORY_PMM_ALLOCATOR_H
#define CSOS_MEMORY_PMM_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>

/*
 * 物理页分配器策略：抽象 alloc_page / free_page 及初始化阶段的区间标记。
 * 当前默认实现为 bitmap；后续可替换为 buddy、slab 等。
 */

typedef struct pmm_allocator_ops
{
    const char *name;

    /* 初始化后端；managed_pages 为 PMM 实际管理的页数 */
    int (*init)(uint64_t managed_pages);

    /* init_pmm 阶段标记物理页区间 */
    void (*mark_used)(uint64_t start, uint64_t end);
    void (*mark_free)(uint64_t start, uint64_t end);

    uint64_t (*alloc_page)(void);
    void (*free_page)(uint64_t page);

    uint64_t (*total_pages)(void);
    uint64_t (*free_pages)(void);
} pmm_allocator_ops_t;

/* 当前使用的分配器；未设置时默认为 bitmap */
const pmm_allocator_ops_t *pmm_allocator_current(void);

/* 切换分配器（测试或替换实现时使用，须在 init_pmm 之前调用） */
void pmm_allocator_set(const pmm_allocator_ops_t *ops);

/* 内置：bitmap 分配器 */
const pmm_allocator_ops_t *pmm_allocator_bitmap(void);

#endif /* CSOS_MEMORY_PMM_ALLOCATOR_H */
