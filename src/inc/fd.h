#ifndef CSOS_FD_H
#define CSOS_FD_H

#include <fs/vfs.h>

#define FD_MAX 16

/* 全局 fd 表（单用户任务过渡；多任务后再搬进 task_t） */
void init_fd();

/* 取 fd 对应的打开文件描述；无效返回 NULL */
vfs_file_t *fd_get(int fd);

#endif /* CSOS_FD_H */
