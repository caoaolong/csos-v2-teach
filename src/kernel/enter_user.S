.global enter_user

#define USER_CS 0x1B
#define USER_DS 0x23

.text
enter_user:
    mov $USER_DS, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* SS: 用户数据段,0x23 = GDT[4] + RPL=3 */
    pushq $USER_DS
    /* RSP: 用户栈顶,对应 enter_user(rip[rdi]，rsp[rsi]) */
    pushq %rsi
    /* RFLAGS: 仅 bit1置1(保留位恒1)，IF=0即关中断进用户态 */
    pushq $0x2
    /* CS: 用户代码段,0x1B = GDT[3] + RPL=3 */
    pushq $USER_CS
    /* RIP: 用户入口地址 */
    pushq %rdi
    iretq