.section .text.bootstrap
.global _start

.extern boot_enter_higher_half

_start:
    /* BootInfo 在物理地址 0x40000，bootstrap 在 0x100000 运行 */
    movabs $0x40000, %rdi
    movabs $__boot_stack_top, %rsp
    call boot_enter_higher_half

1:
    hlt
    jmp 1b
