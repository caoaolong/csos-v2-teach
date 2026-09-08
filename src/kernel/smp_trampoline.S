/*
 * AP 启动桩：复制到物理 0x8000 后由 SIPI 进入（实模式 CS=0x800, IP=0）。
 * 所有绝对地址按「运行时基址 0x8000」计算：0x8000 + (label - start)。
 */
.globl smp_trampoline_start
.globl smp_trampoline_end
.globl tramp_bootinfo

.set TRAMP_BASE, 0x8000

.section .rodata
.align 16
smp_trampoline_start:
.code16
    cli
    cld
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs

    /* GDTR 在 trampoline 页内（含 32 位与 64 位段） */
    lgdtl   TRAMP_BASE + (tramp_gdtr - smp_trampoline_start)

    /* 进保护模式 */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    ljmp    $0x08, $(TRAMP_BASE + (tramp_pm32 - smp_trampoline_start))

.code32
tramp_pm32:
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs

    /* PAE */
    mov     %cr4, %eax
    or      $(1 << 5), %eax
    mov     %eax, %cr4

    /* CR3 = BSP 页表（低 32 位；QEMU 下 PML4 在 4GB 内） */
    mov     TRAMP_BASE + (tramp_bootinfo - smp_trampoline_start) + 8, %eax
    mov     %eax, %cr3

    /* EFER.LME */
    mov     $0xC0000080, %ecx
    rdmsr
    or      $(1 << 8), %eax
    wrmsr

    /* 开分页 → IA-32e 兼容模式 */
    mov     %cr0, %eax
    or      $(1 << 31), %eax
    mov     %eax, %cr0

    ljmp    $0x18, $(TRAMP_BASE + (tramp_lm64 - smp_trampoline_start))

.code64
tramp_lm64:
    mov     $0x20, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs

    movabs  $(TRAMP_BASE + (tramp_bootinfo - smp_trampoline_start)), %rbx
    mov     16(%rbx), %rsp          /* stack */
    mov     24(%rbx), %rax          /* entry = ap_main */
    jmp     *%rax

/* ---- bootinfo：由 BSP 在拷贝后填写 ---- */
.align 8
tramp_bootinfo:
    .long   0       /* 0: started */
    .long   0       /* 4: pad */
    .quad   0       /* 8: cr3 */
    .quad   0       /* 16: stack */
    .quad   0       /* 24: entry */

.align 16
tramp_gdt:
    .quad   0x0000000000000000          /* 0x00 null */
    .quad   0x00CF9A000000FFFF          /* 0x08 32-bit code */
    .quad   0x00CF92000000FFFF          /* 0x10 32-bit data */
    .quad   0x00AF9A000000FFFF          /* 0x18 64-bit code L=1 */
    .quad   0x00AF92000000FFFF          /* 0x20 64-bit data */

tramp_gdt_end:

.align 16
tramp_gdtr:
    .word   tramp_gdt_end - tramp_gdt - 1
    .long   TRAMP_BASE + (tramp_gdt - smp_trampoline_start)

smp_trampoline_end:
