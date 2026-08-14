#include <memory/bitmap.h>
#include <string.h>

void bitmap_init(bitmap_t *bm, uint8_t *storage, uint64_t bit_count)
{
    bm->bits = storage;
    bm->size = bit_count;
}

void bitmap_set_all(bitmap_t *bm, int value)
{
    uint64_t nbytes = bitmap_bytes_for(bm->size);
    kernel_memset(bm->bits, value ? 0xFF : 0x00, (uint32_t)nbytes);
}

void bitmap_set(bitmap_t *bm, uint64_t index)
{
    if (index >= bm->size)
        return;
    bm->bits[index / 8] |= (uint8_t)(1u << (index % 8));
}

void bitmap_clear(bitmap_t *bm, uint64_t index)
{
    if (index >= bm->size)
        return;
    bm->bits[index / 8] &= (uint8_t)~(1u << (index % 8));
}

int bitmap_test(const bitmap_t *bm, uint64_t index)
{
    if (index >= bm->size)
        return 1;
    return (bm->bits[index / 8] >> (index % 8)) & 1;
}

uint64_t bitmap_find_first_clear(const bitmap_t *bm)
{
    uint64_t i;

    for (i = 0; i < bm->size; i++)
    {
        if (!bitmap_test(bm, i))
            return i;
    }

    return (uint64_t)-1;
}