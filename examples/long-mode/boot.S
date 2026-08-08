/*
 * boot.S — MBR 引导扇区（实模式）
 * BIOS 加载到 0x7C00，再把 bootloader+kernel 读到 0x8000 并跳转。
 */
.code16
.global _start

_start:
    cli
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     $0x7C00, %sp
    sti

    /* BIOS 传入的启动盘号保存在 DL */
    mov     %dl, boot_drive

    /* INT 13h 扩展读：从 LBA 1 起读 LOAD_SECTORS 扇区到 0x8000 */
    mov     $dap, %si
    mov     boot_drive, %dl
    mov     $0x42, %ah
    int     $0x13
    jc      disk_error

    /* 跳转到 bootloader（0x0000:0x8000） */
    ljmp    $0x0000, $0x8000

disk_error:
    mov     $err_msg, %si
print_loop:
    lodsb
    test    %al, %al
    jz      halt
    mov     $0x0E, %ah
    mov     $0x00, %bh
    int     $0x10
    jmp     print_loop

halt:
    hlt
    jmp     halt

err_msg:
    .asciz  "disk read error"

boot_drive:
    .byte   0

/* Disk Address Packet */
.align 4
dap:
    .byte   0x10            /* 包大小 */
    .byte   0x00            /* 保留 */
    .word   LOAD_SECTORS    /* 扇区数 */
    .word   0x8000          /* 偏移 */
    .word   0x0000          /* 段 */
    .quad   1               /* 起始 LBA（跳过 MBR） */

/* 读入扇区数：bootloader(16) + kernel 预留(16) = 32 */
.set LOAD_SECTORS, 32

/* 填充至 510 字节并写入引导签名 */
    .space  510 - (. - _start)
    .word   0xAA55
