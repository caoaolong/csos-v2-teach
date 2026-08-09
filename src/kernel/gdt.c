#include <gdt.h>

static gdt_entry_t gdt[GDT_SIZE] = {
    {0, 0, 0, 0}};

void set_gdt_entry(int selector, uint32_t base, uint32_t limit, uint16_t attr)
{
    if (limit > 0xFFFFF)
    {
        attr |= SEG_ATTR_G;
        limit >>= 12;
    }
    gdt_entry_t *entry = &gdt[selector >> 3];
    entry->limit_low = limit & 0xFFFF;
    entry->base_low = base & 0xFFFF;
    entry->base_middle = (base >> 16) & 0xFF;
    entry->access = attr & 0xFF;
    entry->granularity = ((attr >> 8) & 0xF0) | ((limit >> 16) & 0x0F);
    entry->base_high = (base >> 24) & 0xFF;
}

static void load_gdt()
{
    gdtr_t gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)(uintptr_t)gdt,
    };

    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");

    /* 远返回刷新 CS，再重载数据段寄存器 */
    __asm__ volatile(
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : [cs] "i"((uint64_t)KERNEL_CODE_SEG), [ds] "r"((uint16_t)KERNEL_DATA_SEG)
        : "rax", "memory");
}

void init_gdt()
{
    for (int i = 0; i < GDT_SIZE; i++)
    {
        set_gdt_entry(i << 3, 0, 0, 0);
    }

    /* 0x08: 64-bit 代码段 L=1 D=0 */
    set_gdt_entry(KERNEL_CODE_SEG, 0, 0xFFFFFFFF,
                  SEG_ATTR_P | SEG_ATTR_DPL0 | SEG_NORMAL | SEG_TYPE_CODE | SEG_TYPE_RW |
                      SEG_ATTR_L | SEG_ATTR_G);

    /* 0x10: 数据段（Long Mode 下 base/limit 被忽略） */
    set_gdt_entry(KERNEL_DATA_SEG, 0, 0xFFFFFFFF,
                  SEG_ATTR_P | SEG_ATTR_DPL0 | SEG_NORMAL | SEG_TYPE_DATA | SEG_TYPE_RW |
                      SEG_ATTR_G);

    load_gdt();
}
