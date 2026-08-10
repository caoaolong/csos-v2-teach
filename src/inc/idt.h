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

#endif /* CSOS_IDT_H */
