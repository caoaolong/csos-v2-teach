#ifndef CSOS_IDT_H
#define CSOS_IDT_H

#include <kernel.h>

#define IDT_SIZE 0x100

#define GATE_TYPE_IDT (0xE << 8)     // 中断门描述符（64 位 IDT type=0xE）
#define GATE_TYPE_TRAP (0xF << 8)    // 陷阱门描述符（type=0xF）
#define GATE_TYPE_SYSCALL (0xE << 8) // 用户态可触发的中断门（配合 DPL3）
#define GATE_ATTR_P (1 << 15)        // 是否存在
#define GATE_ATTR_DPL0 (0 << 13)     // 特权级0，最高特权级
#define GATE_ATTR_DPL3 (3 << 13)     // 特权级3，最低权限

/* 与 interrupt.S 压栈顺序一致（低地址 → 高地址） */
typedef struct exception_frame
{
    // 手动压栈
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    // 自动压栈
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} exception_frame_t;

/* Long Mode：每个 IDT 项 16 字节 */
typedef struct idt_entry
{
    uint16_t offset_low;  /* handler[15:0] */
    uint16_t selector;    /* 代码段选择子 */
    uint8_t ist;          /* IST 索引（低 3 位），其余为 0 */
    uint8_t type_attr;    /* P | DPL | type */
    uint16_t offset_mid;  /* handler[31:16] */
    uint32_t offset_high; /* handler[63:32] */
    uint32_t reserved;    /* 必须为 0 */
} __attribute__((packed)) idt_entry_t;

typedef struct idtr
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

void init_idt();
void set_interrupt_gate(int vector, uint64_t offset, uint16_t selector, uint16_t attr);

/* 中断向量及处理函数 */
extern void interrupt_handler_default();
void handler_default();

void handler_default(exception_frame_t *frame);
void handler_division(exception_frame_t *frame);
void handler_debug(exception_frame_t *frame);
void handler_nmi(exception_frame_t *frame);
void handler_breakpoint(exception_frame_t *frame);
void handler_overflow(exception_frame_t *frame);
void handler_range(exception_frame_t *frame);
void handler_opcode(exception_frame_t *frame);
void handler_device(exception_frame_t *frame);
void handler_double(exception_frame_t *frame);
void handler_tss(exception_frame_t *frame);
void handler_segment(exception_frame_t *frame);
void handler_stack(exception_frame_t *frame);
void handler_protection(exception_frame_t *frame);
void handler_page(exception_frame_t *frame);
void handler_fpu(exception_frame_t *frame);
void handler_align(exception_frame_t *frame);
void handler_machine(exception_frame_t *frame);
void handler_simd(exception_frame_t *frame);
void handler_virtual(exception_frame_t *frame);
void handler_control(exception_frame_t *frame);
void handler_spurious(exception_frame_t *frame);

/* 汇编入口 */
extern void interrupt_handler_default(void);
extern void interrupt_handler_division(void);
extern void interrupt_handler_debug(void);
extern void interrupt_handler_nmi(void);
extern void interrupt_handler_breakpoint(void);
extern void interrupt_handler_overflow(void);
extern void interrupt_handler_range(void);
extern void interrupt_handler_opcode(void);
extern void interrupt_handler_device(void);
extern void interrupt_handler_double(void);
extern void interrupt_handler_tss(void);
extern void interrupt_handler_segment(void);
extern void interrupt_handler_stack(void);
extern void interrupt_handler_protection(void);
extern void interrupt_handler_page(void);
extern void interrupt_handler_fpu(void);
extern void interrupt_handler_align(void);
extern void interrupt_handler_machine(void);
extern void interrupt_handler_simd(void);
extern void interrupt_handler_virtual(void);
extern void interrupt_handler_control(void);
extern void interrupt_handler_timer(void);
extern void interrupt_handler_kbd(void);
extern void interrupt_handler_spurious(void);

#endif /* CSOS_IDT_H */
