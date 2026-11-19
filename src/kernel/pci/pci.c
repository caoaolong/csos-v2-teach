#include <kernel/pci.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static pci_dev_t g_pci_devs[PCI_MAX_DEVICES];
static int g_pci_count;
static uint8_t g_bus_seen[32]; /* 256 总线访问位图，防桥环路 */

/* 配置地址：enable(31) bus(23:16) slot(15:11) func(10:8) offset(7:2) */
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFCu);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(PCI_CFG_ADDR, pci_addr(bus, slot, func, off));
    return inl(PCI_CFG_DATA);
}

void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                     uint32_t v)
{
    outl(PCI_CFG_ADDR, pci_addr(bus, slot, func, off));
    outl(PCI_CFG_DATA, v);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, slot, func, off);

    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, slot, func, off);

    return (uint8_t)(v >> ((off & 3) * 8));
}

/* 探测一条 BAR 的地址与大小（写全 1 回读掩码，完事立即恢复原值） */
static void pci_probe_bar(pci_dev_t *d, int idx, uint8_t off)
{
    uint32_t orig = pci_cfg_read32(d->bus, d->slot, d->func, off);
    uint32_t szmask;
    pci_bar_t *b = &d->bars[idx];

    b->present = 0;
    pci_cfg_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFFu);
    szmask = pci_cfg_read32(d->bus, d->slot, d->func, off);
    pci_cfg_write32(d->bus, d->slot, d->func, off, orig);
    if (szmask == 0)
        return; /* 该槽未实现 */
    if (szmask & 1)
    {
        /* I/O 端口：bit1:0 保留 */
        b->is_io = 1;
        b->is64 = 0;
        b->addr = orig & ~0x3u;
        b->size = (~(szmask & ~0x3u)) + 1;
        b->present = 1;
        return;
    }
    if (((szmask >> 1) & 3) == 2)
    {
        /* 64 位 MMIO：连吃两槽，高 32 位同样掩码探测 */
        uint32_t hi_orig = pci_cfg_read32(d->bus, d->slot, d->func, off + 4);
        uint32_t hi_mask;
        uint64_t mask;

        pci_cfg_write32(d->bus, d->slot, d->func, off + 4, 0xFFFFFFFFu);
        hi_mask = pci_cfg_read32(d->bus, d->slot, d->func, off + 4);
        pci_cfg_write32(d->bus, d->slot, d->func, off + 4, hi_orig);
        mask = ((uint64_t)hi_mask << 32) | (szmask & ~0xFu);
        b->is_io = 0;
        b->is64 = 1;
        b->addr = ((uint64_t)hi_orig << 32) | (orig & ~0xFu);
        b->size = ~mask + 1;
        b->present = 1;
        return;
    }
    /* 32 位 MMIO：bit3:0 为标志位 */
    b->is_io = 0;
    b->is64 = 0;
    b->addr = orig & ~0xFu;
    b->size = (~(szmask & ~0xFu)) + 1;
    b->present = 1;
}

/* 记录一个功能号：读头部 + 逐条探测 BAR */
static void pci_add_func(uint8_t bus, uint8_t slot, uint8_t func)
{
    pci_dev_t *d;
    uint32_t ccs;
    int i;
    uint8_t off;

    if (g_pci_count >= PCI_MAX_DEVICES)
        return;
    d = &g_pci_devs[g_pci_count++];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = pci_cfg_read16(bus, slot, func, 0x00);
    d->device = pci_cfg_read16(bus, slot, func, 0x02);
    ccs = pci_cfg_read32(bus, slot, func, 0x08);
    d->class_code = (uint8_t)(ccs >> 24);
    d->subclass = (uint8_t)(ccs >> 16);
    d->prog_if = (uint8_t)(ccs >> 8);
    d->header = pci_cfg_read8(bus, slot, func, 0x0E) & 0x7F;
    d->irq = pci_cfg_read8(bus, slot, func, 0x3C);
    for (i = 0; i < 6; i++)
    {
        d->bars[i].present = 0;
        d->bars[i].is64 = 0;
    }
    if (d->header == 0)
    {
        off = 0x10;
        for (i = 0; i < 6; i++, off += 4)
        {
            pci_probe_bar(d, i, off);
            if (d->bars[i].is64)
            {
                i++; /* 64 位占两槽 */
                off += 4;
            }
        }
    }
    else if (d->header == 1)
    {
        /* PCI 桥只看前两条 BAR */
        for (i = 0; i < 2; i++)
            pci_probe_bar(d, i, (uint8_t)(0x10 + i * 4));
    }
}

/* 递归扫总线：普通设备记录，桥跟进次级总线 */
static void pci_scan_bus(uint8_t bus)
{
    uint8_t slot;
    uint8_t func;

    if (g_bus_seen[bus / 8] & (1u << (bus % 8)))
        return;
    g_bus_seen[bus / 8] |= (uint8_t)(1u << (bus % 8));
    for (slot = 0; slot < 32; slot++)
    {
        uint8_t hdr;
        int multi;
        int nf;

        if (pci_cfg_read16(bus, slot, 0, 0x00) == 0xFFFF)
            continue; /* 无设备 */
        hdr = pci_cfg_read8(bus, slot, 0, 0x0E);
        multi = (hdr & 0x80) != 0;
        nf = multi ? 8 : 1;
        for (func = 0; func < nf; func++)
        {
            if (pci_cfg_read16(bus, slot, (uint8_t)func, 0x00) == 0xFFFF)
                continue;
            pci_add_func(bus, slot, (uint8_t)func);
            if ((pci_cfg_read8(bus, slot, (uint8_t)func, 0x0E) & 0x7F) == 1)
            {
                uint8_t secondary =
                    pci_cfg_read8(bus, slot, (uint8_t)func, 0x19);

                pci_scan_bus(secondary); /* 桥下总线 */
            }
        }
    }
}

int pci_count(void)
{
    return g_pci_count;
}

const pci_dev_t *pci_get(int idx)
{
    if (idx < 0 || idx >= g_pci_count)
        return NULL;
    return &g_pci_devs[idx];
}

void init_pci(void)
{
    int i;
    int b;

    for (i = 0; i < PCI_MAX_DEVICES; i++)
    {
        int k;

        g_pci_devs[i].vendor = 0xFFFF;
        for (k = 0; k < 6; k++)
            g_pci_devs[i].bars[k].present = 0;
    }
    for (b = 0; b < 32; b++)
        g_bus_seen[b] = 0;
    g_pci_count = 0;
    pci_scan_bus(0);
    fput_string("[PCI] %d device(s)\n", g_pci_count);
    for (i = 0; i < g_pci_count; i++)
    {
        const pci_dev_t *d = &g_pci_devs[i];
        int k;

        fput_string("[PCI] %02x:%02x.%x %04x:%04x class %02x%02x%02x irq %u",
                    d->bus, d->slot, d->func, d->vendor, d->device,
                    d->class_code, d->subclass, d->prog_if, d->irq);
        for (k = 0; k < 6; k++)
        {
            if (!d->bars[k].present)
                continue;
            if (d->bars[k].is_io)
                fput_string(" io%x:[%x+%x]", k,
                            (unsigned int)d->bars[k].addr,
                            (unsigned int)d->bars[k].size);
            else
                fput_string(" mem%x:[%llx+%llx]%s", k,
                            (unsigned long long)d->bars[k].addr,
                            (unsigned long long)d->bars[k].size,
                            d->bars[k].is64 ? "64" : "");
        }
        fput_string("\n");
    }
}
