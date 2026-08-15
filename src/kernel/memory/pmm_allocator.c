#include <memory/pmm_allocator.h>

static const pmm_allocator_ops_t *current_allocator;

const pmm_allocator_ops_t *pmm_allocator_current(void)
{
    if (current_allocator == NULL)
        return pmm_allocator_bitmap();
    return current_allocator;
}

void pmm_allocator_set(const pmm_allocator_ops_t *ops)
{
    current_allocator = ops;
}
