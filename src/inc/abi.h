#ifndef CSOS_ABI_H
#define CSOS_ABI_H

/*
 * 内核↔用户态 ABI 契约：两边都可以 include 本文件。
 * 约束：只放整数常量（系统调用号、fd 编号、错误码等），
 * 不放函数声明、结构体定义和任何一方的私有内容。
 * 改值即改契约，需内核与 libc 同步。
 */

/* 系统调用号：rdi/rsi/rdx 传参，rax 传调用号兼收返回值 */
#define SYS_WRITE 1 /* rdi=fd, rsi=buf, rdx=len -> 写入字节数/-1 */
#define SYS_EXIT 2  /* rdi=code -> 不返回 */
#define SYS_SLEEP 3 /* rdi=ms -> 0 */
#define SYS_READ 4  /* rdi=fd, rsi=buf, rdx=len -> 读到字节数(阻塞)/-1 */
#define SYS_OPEN 5  /* rdi=path, rsi=flags(O_*), rdx=mode(保留) -> fd/-1 */
#define SYS_CLOSE 6 /* rdi=fd -> 0/-1 */
#define SYS_DUP2 7  /* rdi=oldfd, rsi=newfd -> newfd/-1 */
#define SYS_LSEEK 8 /* rdi=fd, rsi=offset, rdx=whence(SEEK_*) -> 新位置/-1 */
#define SYS_EXECVE 9 /* rdi=path, rsi=argv, rdx=envp -> 成功不返回/失败-1 */
#define SYS_FORK 10 /* -> 子 pid/子 0/失败-1 */
#define SYS_WAIT 11 /* rdi=status(out, 可 0) -> 子 pid/失败-1（阻塞到有子可收） */

/* 标准文件描述符编号 */
#define STDIN_FILENO 0  /* 键盘输入 */
#define STDOUT_FILENO 1 /* 屏幕输出 */
#define STDERR_FILENO 2 /* 屏幕输出（暂与 stdout 同设备，可独立重定向） */

/* open flags：与 VFS_O_* 同值，内核直接透传 vfs_open */
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR 0x02
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_DIRECTORY 0x1000

/* lseek whence：与 VFS_SEEK_* 同值 */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* CSOS_ABI_H */
