.text
.global syscall_entry
.extern handler_syscall
.extern syscall_kstack_top
.extern syscall_user_rsp

/* 与 kernel.h 中 USER_CODE_SEG / USER_DATA_SEG 保持一致 */
#define USER_CS 0x2B /* GDT[5] + RPL=3 */
#define USER_DS 0x23 /* GDT[4] + RPL=3 */

syscall_entry:
  /* SYSCALL 不换栈：暂存用户 RSP 再切内核栈 */
  mov %rsp, syscall_user_rsp(%rip)
  mov syscall_kstack_top(%rip), %rsp

  /* 构造 exception_frame_t（自高地址向低地址压） */
  push $USER_DS                 /* ss */
  pushq syscall_user_rsp(%rip)  /* rsp：用户栈指针 */
  push %r11                     /* rflags（SYSCALL 存于 r11） */
  push $USER_CS                 /* cs */
  push %rcx                     /* rip（SYSCALL 存于 rcx） */
  push $0                       /* error_code */
  push $0x80                    /* vector */
  push %rax                     /* rax：系统调用号 */
  push %rbx
  push $0                       /* rcx：占位 0（原值不可恢复） */
  push %rdx
  push %rbp
  push %rsi
  push %rdi
  push %r8
  push %r9
  push %r10
  push $0                       /* r11：同 rcx */
  push %r12
  push %r13
  push %r14
  push %r15

  cld
  /* 首参经 RDI 传入 exception_frame_t* */
  mov %rsp, %rdi
  call handler_syscall

  /* 按压栈逆序恢复 GPR */
  pop %r15
  pop %r14
  pop %r13
  pop %r12
  pop %r11
  pop %r10
  pop %r9
  pop %r8
  pop %rdi
  pop %rsi
  pop %rbp
  pop %rdx
  pop %rcx
  pop %rbx
  pop %rax

  /* 帧内偏移：rip=+16, rflags=+32, rsp=+40 */
  mov 16(%rsp), %rcx            /* sysretq 从 RCX 恢复 RIP */
  mov 32(%rsp), %r11            /* sysretq 从 R11 恢复 RFLAGS */
  mov 40(%rsp), %rsp            /* 恢复用户 RSP，同时跳过整个帧 */
  sysretq