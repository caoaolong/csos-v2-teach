#ifndef CSOS_GDT_H
#define CSOS_GDT_H

#include <kernel.h>

#define GDT_SIZE 0x100

#define SEG_ATTR_G (1 << 15)
#define SEG_ATTR_D (1 << 14)
#define SEG_ATTR_L (1 << 13) /* Long Mode：代码段 L=1 且 D=0 */
#define SEG_ATTR_P (1 << 7)

#define SEG_ATTR_DPL0 (0 << 5)
#define SEG_ATTR_DPL3 (3 << 5)

#define SEG_ATTR_CPL0 (0 << 0)
#define SEG_ATTR_CPL3 (3 << 0)

#define SEG_SYSTEM (0 << 4)
#define SEG_NORMAL (1 << 4)
#define SEG_TYPE_DATA (0 << 3)
#define SEG_TYPE_CODE (1 << 3)
#define SEG_TYPE_RW (1 << 1)
#define SEG_TYPE_TSS (9 << 0)

typedef struct gdt_entry
{
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdtr
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

void init_gdt();
void set_gdt_entry(int selector, uint32_t base, uint32_t limit, uint16_t attr);

#endif /* CSOS_GDT_H */
