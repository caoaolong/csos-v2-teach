#ifndef CSOS_MOUSE_H
#define CSOS_MOUSE_H

#include <kernel/idt.h>
#include <kernel/apic/ioapic.h>

#define MOUSE_ISA_IRQ 12
#define MOUSE_VECTOR (IOAPIC_IRQ_BASE + MOUSE_ISA_IRQ)

/* 需在 init_kbd 之后调用（复用 8042 初始化，打开第二端口并接鼠标中断） */
void init_mouse();

void handler_mouse(exception_frame_t *frame);

void init_mouse_dev();

#endif /* CSOS_MOUSE_H */
