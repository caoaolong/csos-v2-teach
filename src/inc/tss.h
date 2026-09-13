#ifndef CSOS_TSS_H
#define CSOS_TSS_H

#include <stdint.h>

/* Long Mode 64-bit TSS（Intel SDM） */
typedef struct tss64
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

void tss_set_rsp0(uint64_t rsp0);

/* 按当前 lapic_id 加载本核 TSS（须在 init_lapic / lapic_ap_init 之后） */
void tss_load();

#endif /* CSOS_TSS_H */
