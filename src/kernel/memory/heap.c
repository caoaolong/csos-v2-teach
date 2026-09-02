#include <memory/heap.h>
#include <memory/pmm.h>
#include <serial.h>
#include <stdint.h>

#define HEAP_MAGIC 0x48454150u /* 'HEAP' */
#define HEAP_ALIGN 16u
#define HEAP_INIT_PAGES 4u

#define HEAP_MIN_TOTAL (sizeof(heap_block_t) + HEAP_ALIGN)

/* 块头固定 32 字节，保证页对齐块的 payload 按 16 字节对齐 */
typedef struct heap_block
{
    size_t size; /* 整块大小（含本头） */
    uint32_t magic;
    uint32_t used; /* 0 空闲，1 已用 */
    struct heap_block *next_free;
    uint64_t _pad;
} heap_block_t;

static heap_block_t *free_head;

static size_t align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static heap_block_t *payload_to_block(void *ptr)
{
    return ((heap_block_t *)ptr) - 1;
}

static int block_valid(heap_block_t *block)
{
    if (block == NULL)
        return 0;
    if (block->magic != HEAP_MAGIC)
        return 0;
    if (block->size < HEAP_MIN_TOTAL)
        return 0;
    return 1;
}

static void *block_payload(heap_block_t *block)
{
    return (void *)(block + 1);
}

/* 按地址排序插入，并与左右相邻空闲块合并 */
static void freelist_insert(heap_block_t *block)
{
    heap_block_t *prev = NULL;
    heap_block_t *cur = free_head;

    block->used = 0;
    block->next_free = NULL;

    while (cur != NULL && cur < block)
    {
        prev = cur;
        cur = cur->next_free;
    }

    if (prev == NULL)
        free_head = block;
    else
        prev->next_free = block;
    block->next_free = cur;

    /* 与后块合并 */
    if (cur != NULL &&
        (uint8_t *)block + block->size == (uint8_t *)cur)
    {
        block->size += cur->size;
        block->next_free = cur->next_free;
        cur->magic = 0;
    }

    /* 与前块合并 */
    if (prev != NULL &&
        (uint8_t *)prev + prev->size == (uint8_t *)block)
    {
        prev->size += block->size;
        prev->next_free = block->next_free;
        block->magic = 0;
    }
}

static heap_block_t *heap_add_page()
{
    void *page;
    heap_block_t *block;

    page = (void *)alloc_page();
    if (page == NULL)
        return NULL;

    block = (heap_block_t *)page;
    block->size = (size_t)PAGE_SIZE;
    block->magic = HEAP_MAGIC;
    block->used = 0;
    block->next_free = NULL;
    block->_pad = 0;

    freelist_insert(block);
    return block;
}

static heap_block_t *freelist_find_fit(size_t need_total)
{
    heap_block_t *cur = free_head;

    while (cur != NULL)
    {
        if (!cur->used && cur->size >= need_total)
            return cur;
        cur = cur->next_free;
    }

    return NULL;
}

static void freelist_remove(heap_block_t *block)
{
    heap_block_t *prev = NULL;
    heap_block_t *cur = free_head;

    while (cur != NULL && cur != block)
    {
        prev = cur;
        cur = cur->next_free;
    }

    if (cur == NULL)
        return;

    if (prev == NULL)
        free_head = block->next_free;
    else
        prev->next_free = block->next_free;

    block->next_free = NULL;
}

static int heap_expand(size_t need_total)
{
    size_t pages;
    size_t i;

    pages = (need_total + (size_t)PAGE_SIZE - 1u) / (size_t)PAGE_SIZE;
    if (pages == 0)
        pages = 1;

    for (i = 0; i < pages; i++)
    {
        if (heap_add_page() == NULL)
            return -1;
    }

    return 0;
}

static heap_block_t *block_split(heap_block_t *block, size_t need_total)
{
    size_t remain;
    heap_block_t *rest;

    if (block->size < need_total + HEAP_MIN_TOTAL)
        return block;

    remain = block->size - need_total;
    block->size = need_total;

    rest = (heap_block_t *)((uint8_t *)block + need_total);
    rest->size = remain;
    rest->magic = HEAP_MAGIC;
    rest->used = 0;
    rest->next_free = NULL;
    rest->_pad = 0;

    freelist_insert(rest);
    return block;
}

void init_heap()
{
    uint32_t i;

    free_head = NULL;

    for (i = 0; i < HEAP_INIT_PAGES; i++)
    {
        if (heap_add_page() == NULL)
        {
            put_string("[HEAP] init failed: alloc_page\n");
            return;
        }
    }

    fput_string("[HEAP] ready pages=%u hdr=%u\n",
                HEAP_INIT_PAGES, (uint32_t)sizeof(heap_block_t));
}

void *kmalloc(size_t size)
{
    size_t need_total;
    heap_block_t *block;

    if (size == 0)
        return NULL;

    need_total = align_up(sizeof(heap_block_t) + size, HEAP_ALIGN);
    if (need_total < HEAP_MIN_TOTAL)
        need_total = HEAP_MIN_TOTAL;

    block = freelist_find_fit(need_total);
    if (block == NULL)
    {
        if (heap_expand(need_total) != 0)
            return NULL;
        block = freelist_find_fit(need_total);
        if (block == NULL)
            return NULL;
    }

    freelist_remove(block);
    block = block_split(block, need_total);
    block->used = 1;
    block->next_free = NULL;

    return block_payload(block);
}

void kfree(void *ptr)
{
    heap_block_t *block;

    if (ptr == NULL)
        return;

    block = payload_to_block(ptr);
    if (!block_valid(block) || !block->used)
    {
        put_string("[HEAP] kfree: invalid pointer\n");
        return;
    }

    freelist_insert(block);
}