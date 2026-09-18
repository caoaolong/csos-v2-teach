.section .text.entry
.global _start
.code64
_start:
    xor %ebp, %ebp
    call main
.Lhalt:
    hlt
    jmp .Lhalt
