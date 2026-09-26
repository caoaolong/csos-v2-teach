#include <idt.h>
#include <serial.h>
#include <timer.h>
#include <user.h>

void handler_syscall(exception_frame_t *frame)
{
    switch (frame->rax)
    {
    case SYS_SLEEP:
        msleep(frame->rdi);
        break;

    default:
        fput_string("[SYSCALL] unknown num=%llu\n",
                    (unsigned long long)frame->rax);
        break;
    }
}
