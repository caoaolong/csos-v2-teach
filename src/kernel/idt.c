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

static void install_interrupt_handler(int vector, void (*handler)(void))
{
    set_interrupt_gate(vector, (uint64_t)(uintptr_t)handler, KERNEL_CODE_SEG,
                       GATE_ATTR_P | GATE_ATTR_DPL0 | GATE_TYPE_IDT);
}

static void exception_halt(exception_frame_t *frame, const char *name)
{
    fput_string("Exception %s vector=%d error=0x%x rip=0x%x\n",
                name,
                (int)frame->vector,
                (unsigned)frame->error_code,
                (unsigned)frame->rip);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

void init_idt()
{
    uint64_t handler = (uint64_t)(uintptr_t)interrupt_handler_default;

    for (int i = 0; i < IDT_SIZE; i++)
    {
        set_interrupt_gate(i, handler, KERNEL_CODE_SEG,
                           GATE_ATTR_P | GATE_ATTR_DPL0 | GATE_TYPE_IDT);
    }

    install_interrupt_handler(0, interrupt_handler_division);
    install_interrupt_handler(1, interrupt_handler_debug);
    install_interrupt_handler(2, interrupt_handler_nmi);
    install_interrupt_handler(3, interrupt_handler_breakpoint);
    install_interrupt_handler(4, interrupt_handler_overflow);
    install_interrupt_handler(5, interrupt_handler_range);
    install_interrupt_handler(6, interrupt_handler_opcode);
    install_interrupt_handler(7, interrupt_handler_device);
    install_interrupt_handler(8, interrupt_handler_double);
    install_interrupt_handler(10, interrupt_handler_tss);
    install_interrupt_handler(11, interrupt_handler_segment);
    install_interrupt_handler(12, interrupt_handler_stack);
    install_interrupt_handler(13, interrupt_handler_protection);
    install_interrupt_handler(14, interrupt_handler_page);
    install_interrupt_handler(16, interrupt_handler_fpu);
    install_interrupt_handler(17, interrupt_handler_align);
    install_interrupt_handler(18, interrupt_handler_machine);
    install_interrupt_handler(19, interrupt_handler_simd);
    install_interrupt_handler(20, interrupt_handler_virtual);
    install_interrupt_handler(21, interrupt_handler_control);

    load_idt();
}

void handler_default(exception_frame_t *frame)
{
    exception_halt(frame, "default");
}

void handler_division(exception_frame_t *frame)
{
    exception_halt(frame, "division");
}

void handler_debug(exception_frame_t *frame)
{
    exception_halt(frame, "debug");
}

void handler_nmi(exception_frame_t *frame)
{
    exception_halt(frame, "nmi");
}

void handler_breakpoint(exception_frame_t *frame)
{
    exception_halt(frame, "breakpoint");
}

void handler_overflow(exception_frame_t *frame)
{
    exception_halt(frame, "overflow");
}

void handler_range(exception_frame_t *frame)
{
    exception_halt(frame, "range");
}

void handler_opcode(exception_frame_t *frame)
{
    exception_halt(frame, "opcode");
}

void handler_device(exception_frame_t *frame)
{
    exception_halt(frame, "device");
}

void handler_double(exception_frame_t *frame)
{
    exception_halt(frame, "double");
}

void handler_tss(exception_frame_t *frame)
{
    exception_halt(frame, "tss");
}

void handler_segment(exception_frame_t *frame)
{
    exception_halt(frame, "segment");
}

void handler_stack(exception_frame_t *frame)
{
    exception_halt(frame, "stack");
}

void handler_protection(exception_frame_t *frame)
{
    exception_halt(frame, "protection");
}

static uint64_t read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void handler_page(exception_frame_t *frame)
{
    fput_string("Exception page vector=%d error=0x%x rip=0x%llx cr2=0x%llx\n",
                (int)frame->vector,
                (unsigned)frame->error_code,
                frame->rip,
                read_cr2());
    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

void handler_fpu(exception_frame_t *frame)
{
    exception_halt(frame, "fpu");
}

void handler_align(exception_frame_t *frame)
{
    exception_halt(frame, "align");
}

void handler_machine(exception_frame_t *frame)
{
    exception_halt(frame, "machine");
}

void handler_simd(exception_frame_t *frame)
{
    exception_halt(frame, "simd");
}

void handler_virtual(exception_frame_t *frame)
{
    exception_halt(frame, "virtual");
}

void handler_control(exception_frame_t *frame)
{
    exception_halt(frame, "control");
}