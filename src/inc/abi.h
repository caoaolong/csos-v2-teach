#ifndef CSOS_ABI_H
#define CSOS_ABI_H

/*
 * 内核↔用户态 ABI 契约：两边都可以 include 本文件。
 * 约束：只放整数常量（系统调用号、fd 编号、错误码等），
 * 不放函数声明、结构体定义和任何一方的私有内容。
 * 改值即改契约，需内核与 libc 同步。
 */

/* 系统调用号：rdi/rsi/rdx 传参，rax 传调用号兼收返回值 */
#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_SLEEP 3
#define SYS_READ 4

/* 标准文件描述符编号 */
#define STDIN_FILENO 0  /* 键盘输入 */
#define STDOUT_FILENO 1 /* 屏幕输出 */
#define STDERR_FILENO 2 /* 屏幕输出（暂与 stdout 同设备，可独立重定向） */

#endif /* CSOS_ABI_H */
