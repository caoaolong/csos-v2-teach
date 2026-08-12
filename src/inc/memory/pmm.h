#ifndef CSOS_MEMORY_PMM_H
#define CSOS_MEMORY_PMM_H

#include <stdint.h>
#include <stddef.h>

/* 与 EfiBoot BootInfo 布局 / 地址约定保持一致 */
#define CSOS_BOOT_INFO_ADDR 0x40000ULL
#define CSOS_BOOT_INFO_MAGIC 0xC505B007ULL

typedef struct boot_info
{
    uint64_t magic;

    uint64_t memory_map;
    uint64_t memory_map_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;

    uint64_t rsdp;
} boot_info_t;

#endif /* CSOS_MEMORY_PMM_H */