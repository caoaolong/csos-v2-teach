#include <apic/ioapic.h>
#include <acpi.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <serial.h>

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10

#define IOAPIC_ID 0x00
#define IOAPIC_VER 0x01
#define IOAPIC_REDTBL_BASE 0x10

#define IOAPIC_REDTBL_MASK (1u << 16)
#define IOAPIC_REDTBL_LEVEL (1u << 15)
#define IOAPIC_REDTBL_ACTIVE_LOW (1u << 13)

/* MADT ISO flags：极性 / 触发（MPS INTI） */
#define ACPI_MADT_POLARITY_MASK 0x3u
#define ACPI_MADT_TRIGGER_MASK 0xCu

typedef struct ioapic_dev
{
    volatile uint32_t *base;
    uint32_t gsi_base;
    uint32_t max_redir; /* 可重定向条目数 - 1，即最高 pin 下标 */
    uint8_t id;
} ioapic_dev_t;

static ioapic_dev_t g_ioapics[ACPI_MAX_IOAPICS];
static uint32_t g_ioapic_count;

static void ioapic_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

static uint32_t ioapic_read(ioapic_dev_t *dev, uint32_t reg)
{
    dev->base[IOAPIC_REGSEL / 4] = reg;
    return dev->base[IOAPIC_WINDOW / 4];
}

static void ioapic_write(ioapic_dev_t *dev, uint32_t reg, uint32_t value)
{
    dev->base[IOAPIC_REGSEL / 4] = reg;
    dev->base[IOAPIC_WINDOW / 4] = value;
}

static void ioapic_write_rte(ioapic_dev_t *dev, uint32_t pin, uint32_t low, uint32_t high)
{
    uint32_t reg = IOAPIC_REDTBL_BASE + pin * 2u;
    /* 先写高 32 位再写低 32 位（写低位可能立刻解除 mask） */
    ioapic_write(dev, reg + 1u, high);
    ioapic_write(dev, reg, low);
}

void init_ioapic()
{
    const madt_info_t *madt;
    uint32_t i;

    madt = acpi_madt();
    if (madt == NULL || madt->ioapic_count == 0)
        ioapic_halt("FATAL: no IOAPIC in MADT\n");

    g_ioapic_count = 0;

    for (i = 0; i < madt->ioapic_count; i++)
    {
        const acpi_ioapic_t *info = &madt->ioapics[i];
        uint64_t phys = (uint64_t)info->addr & ~(PAGE_SIZE - 1);
        ioapic_dev_t *dev;
        uint32_t ver;
        uint32_t pin;

        if (map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE) != 0)
            ioapic_halt("FATAL: map IOAPIC MMIO failed\n");

        if (g_ioapic_count >= ACPI_MAX_IOAPICS)
            break;

        dev = &g_ioapics[g_ioapic_count++];
        dev->base = (volatile uint32_t *)(uintptr_t)phys;
        dev->gsi_base = info->gsi_base;
        dev->id = info->id;
        ver = ioapic_read(dev, IOAPIC_VER);
        dev->max_redir = (ver >> 16) & 0xFFu;

        for (pin = 0; pin <= dev->max_redir; pin++)
            ioapic_write_rte(dev, pin, IOAPIC_REDTBL_MASK, 0);

        fput_string("[IOAPIC] id=%u addr=0x%x gsi_base=%u max_redir=%u\n",
                    (unsigned)dev->id,
                    (unsigned)info->addr,
                    (unsigned)dev->gsi_base,
                    (unsigned)dev->max_redir);
    }
}