#ifndef CSOS_ACPI_H

#include <stdint.h>
#include <kernel.h>
#include <memory/pmm.h>

#define ACPI_MAX_CPUS 16
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_OVERRIDES 32

typedef struct acpi_sdt_header
{
    char signature[4];         // 表名，例如 "APIC"、"FACP"、"DSDT"
    uint32_t length;           // 整个表的总长度（包含头部）
    uint8_t revision;          // ACPI 表版本
    uint8_t checksum;          // 校验和，所有字节相加结果必须为 0
    char oem_id[6];            // OEM 厂商 ID
    char oem_table_id[8];      // OEM 表 ID
    uint32_t oem_revision;     // OEM 修订号
    uint32_t creator_id;       // 生成该表的工具/厂商 ID
    uint32_t creator_revision; // 生成工具版本
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct acpi_rsdp
{
    char signature[8];     // "RSD PTR "
    uint8_t checksum;      // ACPI 1.0 部分的校验和
    char oem_id[6];        // OEM 厂商 ID
    uint8_t revision;      // ACPI 版本号:0 = ACPI 1.0;2 = ACPI 2.0+
    uint32_t rsdt_address; // RSDT（Root System Description Table）的 32 位物理地址
    /* ACPI 2.0+ */
    uint32_t length;       // 整个 RSDP 结构的长度, ACPI 2.0 中通常为 36 字节
    uint64_t xsdt_address; // XSDT（Extended System Description Table）的 64 位物理地址, 现代 x86_64 操作系统应优先使用该字段
    uint8_t ext_checksum;  // ACPI 2.0 扩展校验和
    uint8_t reserved[3];   // 保留字段，必须为 0
} __attribute__((packed)) acpi_rsdp_t;

typedef struct acpi_cpu
{
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} acpi_cpu_t;

typedef struct acpi_ioapic
{
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct acpi_iso
{
    uint8_t bus;
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
} acpi_iso_t;

typedef struct madt_info
{
    uint64_t lapic_addr;
    uint32_t flags;

    uint32_t cpu_count;
    acpi_cpu_t cpus[ACPI_MAX_CPUS];

    uint32_t ioapic_count;
    acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];

    uint32_t override_count;
    acpi_iso_t overrides[ACPI_MAX_OVERRIDES];
} madt_info_t;

void init_acpi(boot_info_t *boot);

const madt_info_t *acpi_madt();

#endif /*CSOS_ACPI_H*/