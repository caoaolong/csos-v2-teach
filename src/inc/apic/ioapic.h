#ifndef CSOS_IOAPIC_H
#define CSOS_IOAPIC_H

#include <stdint.h>

/* ISA IRQ 映射到的 IDT 向量基址（与历史 PIC 一致：IRQ0 → 32） */
#define IOAPIC_IRQ_BASE 32

/* 映射 MADT 中的 IOAPIC，默认 mask 全部 RTE；需在 init_lapic 之后调用 */
void init_ioapic();

/*
 * 将 ISA IRQ 路由到指定 IDT 向量，投递到 dest_lapic_id（物理模式）。
 * 自动查 MADT Interrupt Source Override；失败则 FATAL。
 */
void ioapic_route_isa_irq(uint8_t isa_irq, uint8_t vector, uint32_t dest_lapic_id);

#endif /* CSOS_IOAPIC_H */