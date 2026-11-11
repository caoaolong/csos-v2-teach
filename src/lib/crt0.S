.section .text.entry
.global _start
.code64
_start:
    xor %ebp, %ebp
    pop %rdi                    /* argc */
    mov %rsp, %rsi              /* argv */
    call main
.Lhalt:
    hlt
    jmp .Lhalt
