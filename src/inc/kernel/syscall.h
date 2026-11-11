#ifndef CSOS_SYSCALL_H
#define CSOS_SYSCALL_H

#include <idt.h>

void init_syscall();

void handler_syscall(exception_frame_t *frame);

void syscall_entry();

extern uint64_t syscall_kstack_top;
extern uint64_t syscall_user_rsp;

#endif /* CSOS_SYSCALL_H */