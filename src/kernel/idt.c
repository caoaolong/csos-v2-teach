#include <idt.h>
#include <serial.h>

idt_entry_t idt[IDT_SIZE];

void set_interrupt_gate(int vector, uint64_t offset, uint16_t selector, uint16_t attr)
{
    idt_entry_t *entry = &idt[vector];
    entry->offset_low = (uint16_t)(offset & 0xFFFF);
    entry->selector = selector;
    /* attr 低 8 位：IST；高 8 位：P|DPL|type */
    entry->ist = (uint8_t)(attr & 0x07);
    entry->type_attr = (uint8_t)((attr >> 8) & 0xFF);
    entry->offset_mid = (uint16_t)((offset >> 16) & 0xFFFF);
    entry->offset_high = (uint32_t)((offset >> 32) & 0xFFFFFFFF);
    entry->reserved = 0;
}

static void load_idt()
{
    idtr_t idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)(uintptr_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void init_idt()
{
    uint64_t handler = (uint64_t)(uintptr_t)interrupt_handler_default;

    for (int i = 0; i < IDT_SIZE; i++)
    {
        set_interrupt_gate(i, handler, KERNEL_CODE_SEG,
                           GATE_ATTR_P | GATE_ATTR_DPL0 | GATE_TYPE_IDT);
    }

    load_idt();
}

void handler_default()
{
    put_string("Unhandled interrupt/exception\n");
}
