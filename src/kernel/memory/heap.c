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

static heap_block_t *heap_add_page(void)
{
    void *page;
    heap_block_t *block;

    page = alloc_page();
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
