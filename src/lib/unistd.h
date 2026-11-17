#ifndef CSOS_LIBC_UNISTD_H
#define CSOS_LIBC_UNISTD_H

#include <abi.h>

/* 用户态系统调用封装（POSIX 风格标准头） */

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

/* 阻塞回收已退出子进程：成功返回子 pid，status 回填 exit 码，可传 0 */
/* 无子返回 -1（errno=ECHILD） */
long wait(int *status);

/* 取当前任务 pid；失败返回 -1 */
long getpid(void);

/* 取父任务 pid；无父返回 -1 */
long getppid(void);

/* 堆顶管理：brk 设堆顶为 addr，sbrk 增量调整并返回旧堆顶 */
long brk(void *addr);
void *sbrk(long incr);

/* 匿名管道：fds[0]=读端，fds[1]=写端；成功 0，失败 -1 */
long pipe(int fds[2]);

/* 整块显存映射进本任务：返回像素起点虚地址，失败返回 (void *)-1（幂等） */
void *fbmap(void);

/* 屏幕归属仲裁：FB_ACQUIRE=compositor 接管，FB_RELEASE=交还 console */
/* 成功 0，失败 -1 */
long fbctl(int op);

/* 命名管道：在 /pipe 下建节点（如 "/pipe/log"）；mode 保留暂不用 */
/* 成功 0，失败 -1 并填 errno（EEXIST=撞名，ENOENT=路径非法） */
long mkfifo(const char *path, int mode);

#endif /* CSOS_LIBC_UNISTD_H */
