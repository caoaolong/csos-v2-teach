.section .text.bootstrap
.global _start

.extern boot_enter_higher_half

_start:
    /*
     * BootInfo 固定在物理地址 0x40000（与 EfiBoot 约定）。
     * bootstrap 在物理 0x100000 运行，完成页表后跳入高半核 kernel_main。
     */
    movabs $0x40000, %rdi
    movabs $__boot_stack_top, %rsp
    call boot_enter_higher_half

1:
    hlt
    jmp 1b
