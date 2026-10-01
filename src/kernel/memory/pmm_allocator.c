#include <memory/pmm_allocator.h>

extern const pmm_allocator_ops_t pmm_allocator_bitmap_ops;

static const pmm_allocator_ops_t *current_allocator;

const pmm_allocator_ops_t *pmm_allocator_current()
{
    if (current_allocator == NULL)
        return &pmm_allocator_bitmap_ops;
    return current_allocator;
}

void pmm_allocator_set(const pmm_allocator_ops_t *ops)
{
    current_allocator = ops;
}
