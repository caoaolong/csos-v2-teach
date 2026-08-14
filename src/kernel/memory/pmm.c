#include <memory/pmm.h>
#include <memory/bitmap.h>

/* 管理上限：512MB（QEMU 默认 128MB，bitmap 约 16KB） */
#define PMM_MAX_PHYS (512ULL * 1024 * 1024)
#define PMM_MAX_PAGES (PMM_MAX_PHYS / PAGE_SIZE)
#define PMM_BITMAP_BYTES ((PMM_MAX_PAGES + 7) / 8)

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

/* 1 = 已用，0 = 空闲 */
static uint8_t page_bitmap_storage[PMM_BITMAP_BYTES];
static bitmap_t page_bitmap;
static uint64_t managed_pages;
static uint64_t free_pages;

static inline uint64_t page_align_up(uint64_t addr)
{
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline uint64_t page_align_down(uint64_t addr)
{
    return addr & ~(PAGE_SIZE - 1);
}

static void mark_range_used(uint64_t start, uint64_t end)
{
    uint64_t page;
    uint64_t first = page_align_down(start) / PAGE_SIZE;
    uint64_t last = page_align_up(end) / PAGE_SIZE;

    if (last > managed_pages)
        last = managed_pages;

    for (page = first; page < last; page++)
    {
        if (page >= managed_pages)
            break;
        if (!bitmap_test(&page_bitmap, page))
        {
            bitmap_set(&page_bitmap, page);
            if (free_pages > 0)
                free_pages--;
        }
    }
}

static void mark_range_free(uint64_t start, uint64_t end)
{
    uint64_t page;
    uint64_t first = page_align_up(start) / PAGE_SIZE;
    uint64_t last = page_align_down(end) / PAGE_SIZE;

    if (last > managed_pages)
        last = managed_pages;

    for (page = first; page < last; page++)
    {
        if (page >= managed_pages)
            break;
        if (bitmap_test(&page_bitmap, page))
        {
            bitmap_clear(&page_bitmap, page);
            free_pages++;
        }
    }
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
    uint8_t *ptr;
    uint8_t *end;
    uint64_t max_end = 0;
    uint64_t kernel_start;
    uint64_t kernel_end;

    if (boot == NULL || boot->magic != CSOS_BOOT_INFO_MAGIC ||
        boot->descriptor_size == 0)
    {
        put_string("[PMM] init failed: bad boot_info\n");
        return;
    }

    bitmap_init(&page_bitmap, page_bitmap_storage, PMM_MAX_PAGES);
    /* 全部标为已用，再按 Memory Map 放开可用区 */
    bitmap_set_all(&page_bitmap, 1);
    free_pages = 0;
    managed_pages = 0;

    ptr = (uint8_t *)(uintptr_t)boot->memory_map;
    end = ptr + boot->memory_map_size;

    while (ptr < end)
    {
        memory_descriptor_t *desc = (memory_descriptor_t *)(void *)ptr;
        uint64_t start = desc->physical_start;
        uint64_t region_end = start + desc->number_of_pages * PAGE_SIZE;

        if (region_is_usable(desc->type) && region_end > max_end)
            max_end = region_end;

        ptr += boot->descriptor_size;
    }

    if (max_end > PMM_MAX_PHYS)
        max_end = PMM_MAX_PHYS;

    managed_pages = max_end / PAGE_SIZE;
    if (managed_pages > PMM_MAX_PAGES)
        managed_pages = PMM_MAX_PAGES;

    /* 实际管理的位数收紧到 managed_pages */
    page_bitmap.size = managed_pages;

    /* 第二遍：把可用类型标记为空闲 */
    ptr = (uint8_t *)(uintptr_t)boot->memory_map;
    while (ptr < end)
    {
        memory_descriptor_t *desc = (memory_descriptor_t *)(void *)ptr;
        uint64_t start = desc->physical_start;
        uint64_t region_end = start + desc->number_of_pages * PAGE_SIZE;

        if (region_is_usable(desc->type))
            mark_range_free(start, region_end);

        ptr += boot->descriptor_size;
    }

    /* 内核镜像（含栈所在 BSS）标为已用 */
    kernel_start = page_align_down((uint64_t)(uintptr_t)__kernel_start);
    kernel_end = page_align_up((uint64_t)(uintptr_t)__kernel_end);
    mark_range_used(kernel_start, kernel_end);

    /* BootInfo 固定页 */
    mark_range_used(CSOS_BOOT_INFO_ADDR, CSOS_BOOT_INFO_ADDR + PAGE_SIZE);

    /* Memory Map 缓冲区本身可能落在 BootServicesData，保持占用直到不再需要 */
    mark_range_used(boot->memory_map, boot->memory_map + boot->memory_map_size);

    fput_string("[PMM] managed pages=%llu free pages=%llu kernel=0x%llx-0x%llx\n",
                managed_pages, free_pages, kernel_start, kernel_end);
}