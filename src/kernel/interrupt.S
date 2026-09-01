.macro interrupt_handler func, vector, code
.text
.global interrupt_handler_\func
.extern handler_\func
interrupt_handler_\func:
  .if \code == 0
  push $0
  .endif
  push $\vector

  push %rax
  push %rbx
  push %rcx
  push %rdx
  push %rbp
  push %rsi
  push %rdi
  push %r8
  push %r9
  push %r10
  push %r11
  push %r12
  push %r13
  push %r14
  push %r15

  cld
  /* 根据x86‑64 System V ABI：第一个参数存储在RDI寄存器中 */
  mov %rsp, %rdi
  call handler_\func
  
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

  add $(2 * 8), %rsp
  iretq
.endm

interrupt_handler default,          -1,     0
interrupt_handler division,         0,      0
interrupt_handler debug,            1,      0
interrupt_handler nmi,              2,      0
interrupt_handler breakpoint,       3,      0
interrupt_handler overflow,         4,      0
interrupt_handler range,            5,      0
interrupt_handler opcode,           6,      0
interrupt_handler device,           7,      0
interrupt_handler double,           8,      1
interrupt_handler tss,              10,     1
interrupt_handler segment,          11,     1
interrupt_handler stack,            12,     1
interrupt_handler protection,       13,     1
interrupt_handler page,             14,     1
interrupt_handler fpu,              16,     0
interrupt_handler align,            17,     1
interrupt_handler machine,          18,     0
interrupt_handler simd,             19,     0
interrupt_handler virtual,          20,     0
interrupt_handler control,          21,     1

/* timer: 0x20 */
interrupt_handler timer,            32,     0
interrupt_handler kbd,              33,     0
interrupt_handler spurious,         255,    0