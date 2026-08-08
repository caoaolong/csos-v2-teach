/*
 * bootloader.S — 从实模式进入 Long Mode，再跳转到内核
 * 加载地址：0x8000
 */
.code16
.global _start

.set KERNEL_ENTRY, 0xA000

_start:
    cli
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     $0x7C00, %sp

    /* ---- 开启 A20 ---- */
    call    enable_a20

    /* ---- 加载 GDT ---- */
    lgdt    gdt_desc

    /* ---- 进入 32 位保护模式 ---- */
    mov     %cr0, %eax
    or      $0x1, %eax
    mov     %eax, %cr0
    ljmp    $0x08, $pm_entry

.code32
pm_entry:
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs
    mov     $0x7C00, %esp

    /* ---- 清零并建立 identity-map 页表（2MB 页，覆盖低 1GB） ---- */
    /* PML4 @ 0x1000, PDPT @ 0x2000, PD @ 0x3000 */
    mov     $0x1000, %edi
    xor     %eax, %eax
    mov     $0xC00, %ecx          /* 3 * 0x400 dwords = 3 页 */
    rep     stosl

    /* PML4[0] = PDPT | PRESENT | RW */
    movl    $0x2003, 0x1000
    /* PDPT[0] = PD   | PRESENT | RW */
    movl    $0x3003, 0x2000

    /* PD[i] = (i << 21) | PRESENT | RW | PS(2MB) */
    mov     $0x3000, %edi
    xor     %eax, %eax
    or      $0x83, %eax           /* P | RW | PS */
    mov     $512, %ecx
1:
    mov     %eax, (%edi)
    add     $0x200000, %eax
    add     $8, %edi
    loop    1b

    /* ---- 开启 PAE（Physical Address Extension） ---- */
    mov     %cr4, %eax
    or      $(1 << 5), %eax
    mov     %eax, %cr4

    /* ---- 设置 CR3 = PML4 ---- */
    mov     $0x1000, %eax
    mov     %eax, %cr3

    /* ---- 开启 EFER.LME ---- */
    mov     $0xC0000080, %ecx
    rdmsr
    or      $(1 << 8), %eax
    wrmsr

    /* ---- 开启分页 → 进入 Long Mode（兼容子模式） ---- */
    mov     %cr0, %eax
    or      $(1 << 31), %eax
    mov     %eax, %cr0

    /* 远跳转到 64 位代码段 */
    ljmp    $0x18, $lm_entry

.code64
lm_entry:
    mov     $0x20, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     %ax, %fs
    mov     %ax, %gs
    mov     $0x7C00, %rsp

    /* 跳转到内核入口 */
    movabs  $KERNEL_ENTRY, %rax
    jmp     *%rax

/* ---- A20：键盘控制器 Fast A20 ---- */
.code16
enable_a20:
    in      $0x92, %al
    or      $0x02, %al
    and     $0xFE, %al
    out     %al, $0x92
    ret

/* ---- GDT ---- */
.align 8
gdt:
    /* 0x00: null */
    .quad   0x0000000000000000
    /* 0x08: 32-bit code  base=0 limit=4GB */
    .quad   0x00CF9A000000FFFF
    /* 0x10: 32-bit data  base=0 limit=4GB */
    .quad   0x00CF92000000FFFF
    /* 0x18: 64-bit code  L=1 D=0 */
    .quad   0x00AF9A000000FFFF
    /* 0x20: 64-bit data */
    .quad   0x00AF92000000FFFF

gdt_desc:
    .word   gdt_desc - gdt - 1
    .long   gdt
