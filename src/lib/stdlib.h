#ifndef CSOS_LIBC_STDLIB_H
#define CSOS_LIBC_STDLIB_H

/* 用户态堆分配（brk/sbrk 之上的空闲链表，K&R 风格，带合并与堆顶归还） */

void *malloc(unsigned long size);
void free(void *ptr);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *ptr, unsigned long size);

#endif /* CSOS_LIBC_STDLIB_H */
