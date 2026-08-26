#ifndef CSOS_PIC_H
#define CSOS_PIC_H

#include <stdint.h>

#define PIC_IRQ_BASE 32

void init_pic(void);
void pic_eoi(uint8_t irq);
void pic_disable();
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

#endif /* CSOS_PIC_H */