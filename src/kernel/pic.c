#include <kernel/pic.h>
#include <kernel/kernel.h>

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define PIC_EOI 0x20

void init_pic()
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, PIC_IRQ_BASE);     /* 主片向量基址 32 */
    outb(PIC2_DATA, PIC_IRQ_BASE + 8); /* 从片向量基址 40 */

    outb(PIC1_DATA, 1 << 2); /* 主片经 IRQ2 级联 */
    outb(PIC2_DATA, 2);      /* 从片标识为 2 */

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    (void)mask1;
    (void)mask2;
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_disable()
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    value = inb(port) | (uint8_t)(1u << irq);
    outb(port, value);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    value = inb(port) & (uint8_t)~(1u << irq);
    outb(port, value);
}