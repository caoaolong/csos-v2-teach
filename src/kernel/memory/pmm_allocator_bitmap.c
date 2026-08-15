#include <memory/pmm_allocator.h>
#include <memory/bitmap.h>
#include <memory/pmm.h>

/* 管理上限：512MB（QEMU 默认 128MB，bitmap 约 16KB） */
#define PMM_MAX_PHYS (512ULL * 1024 * 1024)
#define PMM_MAX_PAGES (PMM_MAX_PHYS / PAGE_SIZE)
#define PMM_BITMAP_BYTES ((PMM_MAX_PAGES + 7) / 8)

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

static int bitmap_allocator_init(uint64_t pages)
{
    if (pages > PMM_MAX_PAGES)
        return -1;

    bitmap_init(&page_bitmap, page_bitmap_storage, PMM_MAX_PAGES);
    /* 全部标为已用，再由 init_pmm 按 Memory Map 放开可用区 */
    bitmap_set_all(&page_bitmap, 1);
    free_pages = 0;
    managed_pages = pages;
    page_bitmap.size = pages;
    return 0;
}

static void bitmap_mark_used(uint64_t start, uint64_t end)
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

static void bitmap_mark_free(uint64_t start, uint64_t end)
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

static uint64_t bitmap_alloc_page(void)
{
    uint64_t page = bitmap_find_first_clear(&page_bitmap);

    if (page == (uint64_t)-1)
        return (uint64_t)-1;

    bitmap_set(&page_bitmap, page);
    free_pages--;
    return (uint64_t)(uintptr_t)(page * PAGE_SIZE);
}

static void bitmap_free_page(uint64_t page)
{
    uint64_t addr;
    uint64_t idx;

    if (page == (uint64_t)-1)
        return;

    addr = (uint64_t)(uintptr_t)page;
    if (addr & (PAGE_SIZE - 1))
        return;

    idx = addr / PAGE_SIZE;
    if (idx >= managed_pages)
        return;

    if (bitmap_test(&page_bitmap, idx))
    {
        bitmap_clear(&page_bitmap, idx);
        free_pages++;
    }
}

static uint64_t bitmap_total_pages(void)
{
    return managed_pages;
}

static uint64_t bitmap_free_pages(void)
{
    return free_pages;
}

static const pmm_allocator_ops_t bitmap_allocator = {
    .name = "bitmap",
    .init = bitmap_allocator_init,
    .mark_used = bitmap_mark_used,
    .mark_free = bitmap_mark_free,
    .alloc_page = bitmap_alloc_page,
    .free_page = bitmap_free_page,
    .total_pages = bitmap_total_pages,
    .free_pages = bitmap_free_pages,
};

const pmm_allocator_ops_t *pmm_allocator_bitmap(void)
{
    return &bitmap_allocator;
}
