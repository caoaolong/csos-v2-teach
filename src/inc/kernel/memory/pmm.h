#ifndef CSOS_MEMORY_PMM_H
#define CSOS_MEMORY_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096ULL

/* alloc_page 失败时的返回值（成功时可返回 0x0） */
#define PMM_INVALID_ADDR ((uint64_t)-1)

/* 与 EfiBoot BootInfo 布局 / 地址约定保持一致 */
#define CSOS_BOOT_INFO_ADDR 0x40000ULL
#define CSOS_BOOT_INFO_MAGIC 0xC505B007ULL

typedef enum
{
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType,
    MEMORY_TYPE_OEM_RESERVED_MIN = 0x70000000,
    MEMORY_TYPE_OEM_RESERVED_MAX = 0x7FFFFFFF,
    MEMORY_TYPE_OS_RESERVED_MIN = 0x80000000,
    MEMORY_TYPE_OS_RESERVED_MAX = 0xFFFFFFFF
} efi_memory_type;

/* 与 UEFI 内存描述符布局一致。 */
/* Type 后有 4 字节对齐填充。 */
/* 遍历须按 descriptor_size 步进。 */
typedef struct memory_descriptor
{
    uint32_t type;
    uint32_t _pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} memory_descriptor_t;

typedef struct boot_info
{
    uint64_t magic;

    uint64_t framebuffer_base;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pixels_per_scanline;
    uint32_t framebuffer_pixel_format; /* EFI_GRAPHICS_PIXEL_FORMAT */

    uint64_t memory_map;
    uint64_t memory_map_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;

    uint64_t rsdp;

    uint64_t config_addr;
    uint64_t config_size;

    /* 显示器物理尺寸（EDID，毫米；0 为未知，按 96DPI 保底）。 */
    /* 缺字段时读到 0 即保底。 */
    uint32_t framebuffer_phys_w_mm;
    uint32_t framebuffer_phys_h_mm;
} boot_info_t;

/* 扫描 Memory Map，建立空闲页 bitmap，标记内核占用页 */
void init_pmm(boot_info_t *boot_info);

/* 分配 / 释放一页；返回物理地址（当前 identity map 下可直接当指针用） */
uint64_t alloc_page();
void free_page(uint64_t addr);

/* 分配或释放 n 个连续物理页。 */
/* 失败返回 PMM_INVALID_ADDR。 */
/* 地址须页对齐。 */
uint64_t alloc_pages(uint64_t n);
void free_pages(uint64_t addr, uint64_t n);

uint64_t pmm_total_pages();
uint64_t pmm_free_pages();

#endif /* CSOS_MEMORY_PMM_H */