#ifndef CSOS_LIBC_UNISTD_H
#define CSOS_LIBC_UNISTD_H

#include <abi.h>

/* 用户态系统调用封装（POSIX 风格标准头）*/

long write(int fd, const void *buf, unsigned long len);
long read(int fd, void *buf, unsigned long len);
void sleep(unsigned long ms);
void exit(int code);

/* 文件描述符：成功返回 fd/新位置，失败返回 -1 */
long open(const char *path, int flags);
long close(int fd);
long dup2(int oldfd, int newfd);
long lseek(int fd, long offset, int whence);

/* 装载并替换当前程序：成功不返回，失败返回 -1；argv/envp 可为 0 */
long execve(const char *path, char *const argv[], char *const envp[]);

/* 复制当前任务：父返回子 pid，子返回 0，失败返回 -1 */
long fork(void);

/* 阻塞回收一个已退出子进程：>0=子 pid（status 回填 exit 码，可传 0），-1=无子 */
long wait(int *status);

/* 回收子进程（仅支持 pid=-1，即任意子进程）：options 仅支持 WNOHANG */
long waitpid(int pid, int *status, int options);

/* 取当前任务 pid；失败返回 -1 */
long getpid(void);

/* 取父任务 pid；无父返回 -1 */
long getppid(void);

/* 堆顶管理：brk 设堆顶为 addr（成功 0/失败 -1），sbrk 增量调整并返回旧堆顶 */
long brk(void *addr);
void *sbrk(long incr);

#endif /* CSOS_LIBC_UNISTD_H */
