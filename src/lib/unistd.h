#ifndef CSOS_LIBC_UNISTD_H
#define CSOS_LIBC_UNISTD_H

/* 用户态系统调用封装（POSIX 风格标准头）*/

long write(int fd, const void *buf, unsigned long len);
void sleep(unsigned long ms);
void exit(int code);

#endif /* CSOS_LIBC_UNISTD_H */
