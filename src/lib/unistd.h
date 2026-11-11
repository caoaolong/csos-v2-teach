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

#endif /* CSOS_LIBC_UNISTD_H */
