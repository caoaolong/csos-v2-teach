.section .text
.global interrupt_handler_default

interrupt_handler_default:
    cld
    call handler_default
1:
    hlt
    jmp 1b
