#ifndef CSOS_MEMORY_HEAP_H
#define CSOS_MEMORY_HEAP_H

#include <stddef.h>

/* 在 init_vmm 之后调用；向 PMM 要页建立空闲链表堆 */
void init_heap();

/* 16 字节对齐；失败返回 NULL */
void *kmalloc(size_t size);

/* NULL 安全；非法指针忽略 */
void kfree(void *ptr);

#endif /* CSOS_MEMORY_HEAP_H */
