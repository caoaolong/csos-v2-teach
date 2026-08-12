.section .text
.global _start

_start:
    /* 设置栈 */
    movabs $stack_top, %rsp

    /* 清 BSS */
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub    %rdi, %rcx
    xor    %rax, %rax
    rep stosb

    /* 调用 C 入口 */
    mov $0x40000, %rdi
    call kernel_main

1:
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom:
    .skip 8192
stack_top: