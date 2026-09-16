#ifndef CSOS_USER_H
#define CSOS_USER_H

#include <stdint.h>

#define USER_CODE_VADDR 0x40000000ULL
#define USER_STACK_VADDR 0x40001000ULL
#define USER_STACK_TOP (USER_STACK_VADDR + 0x1000ULL)

#define SYS_WRITE 1
#define SYS_EXIT 2

/* 映射用户页、设置 TSS.RSP0，iretq 进入 ring3（不返回） */
void user_enter_demo();

#endif /* CSOS_USER_H */
