#ifndef CSOS_KBD_H
#define CSOS_KBD_H

#include <kernel/idt.h>
#include <kernel/apic/ioapic.h>

#define KBD_ISA_IRQ 1
#define KBD_VECTOR (IOAPIC_IRQ_BASE + KBD_ISA_IRQ)

/* 需在 init_ioapic 与 IDT 安装之后、sti 之前调用 */
void init_kbd();

void handler_kbd(exception_frame_t *frame);

void init_kbd_dev();

int64_t kbd_read_blocking(void *buf, uint64_t len);

#endif /* CSOS_KBD_H */
