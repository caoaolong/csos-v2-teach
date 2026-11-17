.global enter_user

/* 与 kernel.h 中 USER_CODE_SEG / USER_DATA_SEG 保持一致 */
#define USER_CS 0x2B /* GDT[5] + RPL=3：64 位用户代码段 */
#define USER_DS 0x23 /* GDT[4] + RPL=3：用户数据段 */

.text
enter_user:
    mov $USER_DS, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* SS：用户数据段 */
    pushq $USER_DS
    /* RSP：用户栈顶 */
    pushq %rsi
    /* RFLAGS：保留位恒 1，IF=1 开中断 */
    pushq $0x202
    /* CS：用户代码段 */
    pushq $USER_CS
    /* RIP：用户入口地址 */
    pushq %rdi
    iretq