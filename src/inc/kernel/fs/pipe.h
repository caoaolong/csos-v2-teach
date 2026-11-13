#ifndef CSOS_FS_PIPE_H
#define CSOS_FS_PIPE_H

#include <kernel/fs/vfs.h>

/* 匿名管道：由 SYS_PIPE 直接建两个端点 */
int pipe_create(vfs_file_t **r_out, vfs_file_t **w_out);

#endif /* CSOS_FS_PIPE_H */
