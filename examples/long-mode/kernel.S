/*
 * kernel.S — Long Mode 下的最小内核
 * 加载/运行地址：0xA000
 * 用 64 位寻址写 VGA，证明已处于 Long Mode。
 */
.code64
.global _start

.set VGA_BASE, 0xB8000
.set ATTR,     0x0A          /* 黑底绿字 */

_start:
    /* 清屏：80x25，空格 + 属性 */
    movabs  $VGA_BASE, %rdi
    mov     $0x0F20, %ax          /* 空格，白字（随后整屏再写绿字消息） */
    mov     $(80 * 25), %ecx
    rep     stosw

    /* 写入 "LONG MODE" —— 使用 64 位绝对地址，体现 Long Mode */
    movabs  $VGA_BASE, %rdi
    lea     msg(%rip), %rsi
1:
    movzbq  (%rsi), %rax
    test    %rax, %rax
    jz      hang
    movb    %al, (%rdi)
    movb    $ATTR, 1(%rdi)
    add     $2, %rdi
    inc     %rsi
    jmp     1b

hang:
    hlt
    jmp     hang

msg:
    .asciz  "LONG MODE"
