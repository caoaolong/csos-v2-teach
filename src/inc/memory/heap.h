#ifndef CSOS_MEMORY_HEAP_H
#define CSOS_MEMORY_HEAP_H

#include <stddef.h>

/* 在 init_vmm 之后调用；向 PMM 要页建立空闲链表堆 */
void init_heap();

#endif /* CSOS_MEMORY_HEAP_H */
