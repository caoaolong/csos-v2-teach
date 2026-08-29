#ifndef CSOS_IOAPIC_H
#define CSOS_IOAPIC_H

#include <stdint.h>

/* ISA IRQ 映射到的 IDT 向量基址（与历史 PIC 一致：IRQ0 → 32） */
#define IOAPIC_IRQ_BASE 32

/* 映射 MADT 中的 IOAPIC，默认 mask 全部 RTE；需在 init_lapic 之后调用 */
void init_ioapic();

#endif /* CSOS_IOAPIC_H */