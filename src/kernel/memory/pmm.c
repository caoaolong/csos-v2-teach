#include <memory/pmm.h>
#include <memory/pmm_allocator.h>

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

#define PMM_MAX_PHYS (512ULL * 1024 * 1024)

static inline uint64_t page_align_up(uint64_t addr)
{
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline uint64_t page_align_down(uint64_t addr)
{
    return addr & ~(PAGE_SIZE - 1);
}

static int region_is_usable(uint32_t type)
{
    switch (type)
    {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return 1;
    default:
        return 0;
    }
}

void init_pmm(boot_info_t *boot)
{
    const pmm_allocator_ops_t *alloc;
    uint8_t *ptr;
    uint8_t *end;
    uint64_t max_end = 0;
    uint64_t kernel_start;
    uint64_t kernel_end;
    uint64_t managed_pages;

    if (boot == NULL || boot->magic != CSOS_BOOT_INFO_MAGIC ||
        boot->descriptor_size == 0)
    {
        put_string("[PMM] init failed: bad boot_info\n");
        return;
    }

    alloc = pmm_allocator_current();

    if (alloc == NULL || alloc->init == NULL || alloc->mark_used == NULL || alloc->mark_free == NULL)
    {
        put_string("[PMM] init failed: no allocator\n");
        return;
    }

    ptr = (uint8_t *)(uintptr_t)boot->memory_map;
    end = ptr + boot->memory_map_size;

    while (ptr < end)
    {
        memory_descriptor_t *desc = (memory_descriptor_t *)(void *)ptr;
        uint64_t region_end = desc->physical_start + desc->number_of_pages * PAGE_SIZE;

        if (region_is_usable(desc->type) && region_end > max_end)
            max_end = region_end;

        ptr += boot->descriptor_size;
    }

    if (max_end > PMM_MAX_PHYS)
        max_end = PMM_MAX_PHYS;

    managed_pages = max_end / PAGE_SIZE;
    if (alloc->init(managed_pages) != 0)
    {
        put_string("[PMM] init failed: allocator ");
        put_string(alloc->name);
        put_string("\n");
        return;
    }

    /* 第二遍：把可用类型标记为空闲 */
    ptr = (uint8_t *)(uintptr_t)boot->memory_map;
    while (ptr < end)
    {
        memory_descriptor_t *desc = (memory_descriptor_t *)(void *)ptr;
        uint64_t start = desc->physical_start;
        uint64_t region_end = start + desc->number_of_pages * PAGE_SIZE;

        if (region_is_usable(desc->type))
            alloc->mark_free(start, region_end);

        ptr += boot->descriptor_size;
    }

    /* 内核镜像（含栈所在 BSS）标为已用 */
    kernel_start = page_align_down((uint64_t)(uintptr_t)__kernel_start);
    kernel_end = page_align_up((uint64_t)(uintptr_t)__kernel_end);
    alloc->mark_used(kernel_start, kernel_end);

    /* BootInfo 固定页 */
    alloc->mark_used(CSOS_BOOT_INFO_ADDR, CSOS_BOOT_INFO_ADDR + PAGE_SIZE);

    /* Memory Map 缓冲区本身可能落在 BootServicesData，保持占用直到不再需要 */
    alloc->mark_used(boot->memory_map, boot->memory_map + boot->memory_map_size);

    fput_string("[PMM] allocator=%s managed pages=%llu free pages=%llu kernel=0x%llx-0x%llx\n",
                alloc->name, alloc->total_pages(), alloc->free_pages(), kernel_start, kernel_end);
}

uint64_t alloc_page()
{
    const pmm_allocator_ops_t *alloc = pmm_allocator_current();

    if (alloc == NULL || alloc->alloc_page == NULL)
        return PMM_INVALID_ADDR;

    return alloc->alloc_page();
}

void free_page(uint64_t page)
{
    const pmm_allocator_ops_t *alloc = pmm_allocator_current();

    if (alloc == NULL || alloc->free_page == NULL)
        return;

    alloc->free_page(page);
}

uint64_t pmm_total_pages()
{
    const pmm_allocator_ops_t *alloc = pmm_allocator_current();

    if (alloc == NULL || alloc->total_pages == NULL)
        return 0;

    return alloc->total_pages();
}

uint64_t pmm_free_pages()
{
    const pmm_allocator_ops_t *alloc = pmm_allocator_current();

    if (alloc == NULL || alloc->free_pages == NULL)
        return 0;

    return alloc->free_pages();
}