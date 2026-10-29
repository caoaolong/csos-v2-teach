#ifndef CSOS_USER_H
#define CSOS_USER_H

#include <stdint.h>

#define USER_STACK_VADDR 0x40001000ULL
#define USER_STACK_TOP (USER_STACK_VADDR + 0x1000ULL)

#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_SLEEP 3 /* int 0x80: rdi = 睡眠毫秒数 */

void user_enter_init(const char *init_path);

#endif /* CSOS_USER_H */
