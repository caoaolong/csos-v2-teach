#ifndef CSOS_USER_H
#define CSOS_USER_H

#include <stdint.h>

#define USER_STACK_VADDR 0x40001000ULL
#define USER_STACK_TOP (USER_STACK_VADDR + 0x1000ULL)

/* 内核内部入口：加载并进入用户态（不返回），仅内核侧使用 */
void user_enter_init(const char *init_path);

#endif /* CSOS_USER_H */
