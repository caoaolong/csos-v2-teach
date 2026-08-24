#include <acpi.h>
#include <serial.h>
#include <string.h>

enum
{
    MADT_LOCAL_APIC = 0,
    MADT_IO_APIC = 1,
    MADT_ISO = 2,
    MADT_NMI = 3,
    MADT_LAPIC_NMI = 4,
    MADT_LAPIC_ADDR_OVERRIDE = 5,
    MADT_IOAPIC_NMI = 6,
    MADT_LOCAL_X2APIC = 9,
};

typedef struct acpi_madt
{
    acpi_sdt_header_t header;
    uint32_t lapic_address;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct madt_entry_header
{
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

typedef struct madt_local_apic
{
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_local_apic_t;

typedef struct madt_io_apic
{
    madt_entry_header_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_io_apic_t;

typedef struct madt_iso
{
    madt_entry_header_t header;
    uint8_t bus;
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) madt_iso_t;

typedef struct madt_lapic_addr_override
{
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t lapic_addr;
} __attribute__((packed)) madt_lapic_addr_override_t;

static madt_info_t g_madt;
static int g_madt_ready;

const madt_info_t *acpi_madt(void)
{
    if (!g_madt_ready)
        return NULL;
    return &g_madt;
}

static void acpi_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

static int acpi_checksum_ok(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    uint32_t i;

    for (i = 0; i < len; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static int sdt_sig_eq(const acpi_sdt_header_t *hdr, const char *sig4)
{
    return kernel_memcmp((void *)hdr->signature, (void *)sig4, 4) == 0;
}

static const acpi_sdt_header_t *find_madt_in_xsdt(const acpi_sdt_header_t *xsdt)
{
    uint32_t n;
    uint32_t i;
    const uint64_t *entries;

    if (!acpi_checksum_ok(xsdt, xsdt->length))
        acpi_halt("FATAL: ACPI XSDT checksum bad\n");

    n = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
    entries = (const uint64_t *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));

    for (i = 0; i < n; i++)
    {
        const acpi_sdt_header_t *hdr =
            (const acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (hdr == NULL)
            continue;
        if (!sdt_sig_eq(hdr, "APIC"))
            continue;
        if (!acpi_checksum_ok(hdr, hdr->length))
            acpi_halt("FATAL: ACPI MADT checksum bad\n");
        return hdr;
    }
    return NULL;
}

static const acpi_sdt_header_t *find_madt_in_rsdt(const acpi_sdt_header_t *rsdt)
{
    uint32_t n;
    uint32_t i;
    const uint32_t *entries;

    if (!acpi_checksum_ok(rsdt, rsdt->length))
        acpi_halt("FATAL: ACPI RSDT checksum bad\n");

    n = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    entries = (const uint32_t *)((const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));

    for (i = 0; i < n; i++)
    {
        const acpi_sdt_header_t *hdr =
            (const acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (hdr == NULL)
            continue;
        if (!sdt_sig_eq(hdr, "APIC"))
            continue;
        if (!acpi_checksum_ok(hdr, hdr->length))
            acpi_halt("FATAL: ACPI MADT checksum bad\n");
        return hdr;
    }
    return NULL;
}

static void parse_madt_entries(const acpi_madt_t *madt)
{
    const uint8_t *base = (const uint8_t *)madt;
    const uint8_t *end = base + madt->header.length;
    const uint8_t *p = base + sizeof(acpi_madt_t);

    g_madt.lapic_addr = madt->lapic_address;
    g_madt.flags = madt->flags;
    g_madt.cpu_count = 0;
    g_madt.ioapic_count = 0;
    g_madt.override_count = 0;

    while (p + sizeof(madt_entry_header_t) <= end)
    {
        const madt_entry_header_t *eh = (const madt_entry_header_t *)p;

        if (eh->length < sizeof(madt_entry_header_t))
            acpi_halt("FATAL: ACPI MADT entry length < 2\n");
        if (p + eh->length > end)
            acpi_halt("FATAL: ACPI MADT entry exceeds table\n");

        switch (eh->type)
        {
        case MADT_LOCAL_APIC:
            if (eh->length < sizeof(madt_local_apic_t))
                acpi_halt("FATAL: ACPI MADT local APIC entry too short\n");
            if (g_madt.cpu_count < ACPI_MAX_CPUS)
            {
                const madt_local_apic_t *e = (const madt_local_apic_t *)p;
                acpi_cpu_t *cpu = &g_madt.cpus[g_madt.cpu_count++];
                cpu->acpi_processor_id = e->acpi_processor_id;
                cpu->apic_id = e->apic_id;
                cpu->flags = e->flags;
            }
            break;

        case MADT_IO_APIC:
            if (eh->length < sizeof(madt_io_apic_t))
                acpi_halt("FATAL: ACPI MADT IOAPIC entry too short\n");
            if (g_madt.ioapic_count < ACPI_MAX_IOAPICS)
            {
                const madt_io_apic_t *e = (const madt_io_apic_t *)p;
                acpi_ioapic_t *io = &g_madt.ioapics[g_madt.ioapic_count++];
                io->id = e->ioapic_id;
                io->addr = e->ioapic_addr;
                io->gsi_base = e->gsi_base;
            }
            break;

        case MADT_ISO:
            if (eh->length < sizeof(madt_iso_t))
                acpi_halt("FATAL: ACPI MADT ISO entry too short\n");
            if (g_madt.override_count < ACPI_MAX_OVERRIDES)
            {
                const madt_iso_t *e = (const madt_iso_t *)p;
                acpi_iso_t *iso = &g_madt.overrides[g_madt.override_count++];
                iso->bus = e->bus;
                iso->irq = e->irq;
                iso->gsi = e->gsi;
                iso->flags = e->flags;
            }
            break;

        case MADT_LAPIC_ADDR_OVERRIDE:
            if (eh->length < sizeof(madt_lapic_addr_override_t))
                acpi_halt("FATAL: ACPI MADT LAPIC override too short\n");
            {
                const madt_lapic_addr_override_t *e =
                    (const madt_lapic_addr_override_t *)p;
                g_madt.lapic_addr = e->lapic_addr;
            }
            break;

        case MADT_LOCAL_X2APIC:
            /* 本阶段跳过 */
            break;

        default:
            break;
        }

        p += eh->length;
    }
}

static void dump_madt()
{
    uint32_t i;

    fput_string("[ACPI] MADT lapic=0x%llx flags=0x%x cpus=%u ioapics=%u overrides=%u\n",
                g_madt.lapic_addr,
                (unsigned)g_madt.flags,
                (unsigned)g_madt.cpu_count,
                (unsigned)g_madt.ioapic_count,
                (unsigned)g_madt.override_count);

    for (i = 0; i < g_madt.cpu_count; i++)
    {
        fput_string("[ACPI] cpu[%u] acpi_id=%u apic_id=%u flags=0x%x\n",
                    (unsigned)i,
                    (unsigned)g_madt.cpus[i].acpi_processor_id,
                    (unsigned)g_madt.cpus[i].apic_id,
                    (unsigned)g_madt.cpus[i].flags);
    }

    for (i = 0; i < g_madt.ioapic_count; i++)
    {
        fput_string("[ACPI] ioapic[%u] id=%u addr=0x%x gsi_base=%u\n",
                    (unsigned)i,
                    (unsigned)g_madt.ioapics[i].id,
                    (unsigned)g_madt.ioapics[i].addr,
                    (unsigned)g_madt.ioapics[i].gsi_base);
    }

    for (i = 0; i < g_madt.override_count; i++)
    {
        fput_string("[ACPI] override[%u] bus=%u irq=%u gsi=%u flags=0x%x\n",
                    (unsigned)i,
                    (unsigned)g_madt.overrides[i].bus,
                    (unsigned)g_madt.overrides[i].irq,
                    (unsigned)g_madt.overrides[i].gsi,
                    (unsigned)g_madt.overrides[i].flags);
    }
}

void init_acpi(boot_info_t *boot)
{
    const acpi_rsdp_t *rsdp;
    const acpi_sdt_header_t *madt_hdr;

    kernel_memset(&g_madt, 0, (uint32_t)sizeof(g_madt));
    g_madt_ready = 0;

    if (boot == NULL || boot->rsdp == 0)
        acpi_halt("FATAL: ACPI RSDP missing (boot_info.rsdp=0)\n");

    rsdp = (const acpi_rsdp_t *)(uintptr_t)boot->rsdp;
    if (kernel_memcmp((void *)rsdp->signature, (void *)"RSD PTR ", 8) != 0)
        acpi_halt("FATAL: ACPI RSDP signature bad\n");

    if (!acpi_checksum_ok(rsdp, 20))
        acpi_halt("FATAL: ACPI RSDP checksum bad\n");

    if (rsdp->revision >= 2)
    {
        if (rsdp->length < sizeof(acpi_rsdp_t))
            acpi_halt("FATAL: ACPI RSDP length too small\n");
        if (!acpi_checksum_ok(rsdp, rsdp->length))
            acpi_halt("FATAL: ACPI RSDP extended checksum bad\n");
    }

    fput_string("[ACPI] RSDP=0x%llx rev=%u\n",
                (unsigned long long)boot->rsdp,
                (unsigned)rsdp->revision);

    madt_hdr = NULL;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0)
    {
        const acpi_sdt_header_t *xsdt =
            (const acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_address;
        if (!sdt_sig_eq(xsdt, "XSDT"))
            acpi_halt("FATAL: ACPI XSDT signature bad\n");
        madt_hdr = find_madt_in_xsdt(xsdt);
    }

    if (madt_hdr == NULL)
    {
        const acpi_sdt_header_t *rsdt =
            (const acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_address;
        if (rsdt == NULL)
            acpi_halt("FATAL: ACPI RSDT address is 0\n");
        if (!sdt_sig_eq(rsdt, "RSDT"))
            acpi_halt("FATAL: ACPI RSDT signature bad\n");
        madt_hdr = find_madt_in_rsdt(rsdt);
    }

    if (madt_hdr == NULL)
        acpi_halt("FATAL: ACPI MADT not found\n");

    parse_madt_entries((const acpi_madt_t *)madt_hdr);
    g_madt_ready = 1;
    dump_madt();
}