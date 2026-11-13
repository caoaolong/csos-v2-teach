#include "stdlib.h"
#include "unistd.h"
#include "string.h"

/* K&R 空闲链表：块大小以 Header 为单位；sbrk 按字节申请，free 合并相邻块，
 * 堆顶空闲块整块 unlink 并收缩 brk，把物理页归还内核。单任务无锁（fork 后
 * 父子各持一份拷贝，无共享）。 */

typedef long Align;

union header
{
    struct
    {
        union header *next;
        unsigned long size; /* 含头在内的块长（Header 单位） */
    } s;
    Align x;
};
typedef union header Header;

static Header base;
static Header *freep = (Header *)0;
/* morecore 经 free 挂链时抑制 trim：否则刚要到的内存被立刻归还，malloc 重试即死循环 */
static int no_trim = 0;

#define NALLOC 1024 /* 每次向内核多要的最小单位数（含头） */

static void trim_heap_top(void)
{
    char *top = (char *)sbrk(0);
    Header *prev;
    Header *p;

    if (top == (char *)-1 || freep == (Header *)0)
        return;
    prev = freep;
    p = freep->s.next;
    for (;;)
    {
        if (p != &base && (char *)(p + p->s.size) == top)
        {
            if (p == p->s.next)
            {
                base.s.next = &base;
                freep = &base;
            }
            else
            {
                prev->s.next = p->s.next;
                freep = prev;
            }
            sbrk((char *)p - top); /* 整块归还；失败则保留已摘链（极少见） */
            return;
        }
        if (p == freep)
            return;
        prev = p;
        p = p->s.next;
    }
}

/* 向内核要至少 nunits 个 Header，挂入空闲链 */
static Header *morecore(unsigned long nunits)
{
    char *p;
    Header *hp;

    if (nunits < NALLOC)
        nunits = NALLOC;
    p = (char *)sbrk((long)(nunits * sizeof(Header)));
    if (p == (char *)-1)
        return (Header *)0;
    hp = (Header *)p;
    hp->s.size = nunits;
    no_trim = 1;
    free((void *)(hp + 1));
    no_trim = 0;
    return freep;
}

void *malloc(unsigned long nbytes)
{
    Header *p;
    Header *prevp;
    unsigned long nunits;

    if (nbytes == 0)
        nbytes = 1;
    nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
    if ((prevp = freep) == (Header *)0)
    {
        base.s.next = freep = prevp = &base;
        base.s.size = 0;
    }
    for (p = prevp->s.next;; prevp = p, p = p->s.next)
    {
        if (p->s.size >= nunits)
        {
            if (p->s.size == nunits)
                prevp->s.next = p->s.next;
            else
            {
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep)
        {
            p = morecore(nunits);
            if (p == (Header *)0)
                return (void *)0;
        }
    }
}

void free(void *ap)
{
    Header *bp;
    Header *p;

    if (ap == (void *)0 || freep == (Header *)0)
        return;
    bp = (Header *)ap - 1;
    for (p = freep; !(bp > p && bp < p->s.next); p = p->s.next)
    {
        if (p >= p->s.next && (bp > p || bp < p->s.next))
            break; /* 环绕：bp 在链首尾之外 */
    }
    if (bp + bp->s.size == p->s.next)
    {
        bp->s.size += p->s.next->s.size;
        bp->s.next = p->s.next->s.next;
    }
    else
        bp->s.next = p->s.next;
    if (p + p->s.size == bp)
    {
        p->s.size += bp->s.size;
        p->s.next = bp->s.next;
    }
    else
        p->s.next = bp;
    freep = p;
    if (!no_trim)
        trim_heap_top();
}

void *calloc(unsigned long nmemb, unsigned long size)
{
    unsigned long total;
    void *p;

    if (nmemb == 0 || size == 0)
        nmemb = size = 1;
    total = nmemb * size;
    if (size != 0 && total / size != nmemb)
        return (void *)0; /* 乘法溢出 */
    p = malloc(total);
    if (p != (void *)0)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, unsigned long size)
{
    Header *bp;
    unsigned long old;
    unsigned long n;
    void *np;

    if (ptr == (void *)0)
        return malloc(size);
    if (size == 0)
    {
        free(ptr);
        return (void *)0;
    }
    bp = (Header *)ptr - 1;
    old = (bp->s.size - 1) * sizeof(Header);
    if (old >= size)
        return ptr;
    np = malloc(size);
    if (np == (void *)0)
        return (void *)0;
    n = old < size ? old : size;
    memcpy(np, ptr, n);
    free(ptr);
    return np;
}
