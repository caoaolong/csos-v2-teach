#ifndef CSOS_MEMORY_BITMAP_H
#define CSOS_MEMORY_BITMAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct bitmap
{
    uint8_t *bits;
    uint64_t size; /* 位数量 */
} bitmap_t;

static inline uint64_t bitmap_bytes_for(uint64_t bit_count)
{
    return (bit_count + 7) / 8;
}

void bitmap_init(bitmap_t *bm, uint8_t *storage, uint64_t bit_count);
void bitmap_set_all(bitmap_t *bm, int value);

void bitmap_set(bitmap_t *bm, uint64_t index);
void bitmap_clear(bitmap_t *bm, uint64_t index);
int bitmap_test(const bitmap_t *bm, uint64_t index);

/* 查找第一个为 0 的位，找不到返回 (uint64_t)-1 */
uint64_t bitmap_find_first_clear(const bitmap_t *bm);

#endif /* CSOS_MEMORY_BITMAP_H */