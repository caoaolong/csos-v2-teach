#ifndef CSOS_FD_H
#define CSOS_FD_H

#include <kernel/fs/vfs.h>

#define FD_MAX 16

struct task;

/* 打开文件描述：多 fd 可共享（dup2），refs 做引用计数 */
struct fd_entry
{
    vfs_file_t *vf;
    int refs;
};

typedef struct fd_entry fd_entry_t;

/* 为任务预开 0/1/2 标准流（独立 open，1/2 可分别重定向） */
void fd_init_std(struct task *t);

/* 当前任务 fd -> vfs 句柄；无效返回 NULL（调用期间持有者不得 close，多任务后需 get/put） */
vfs_file_t *fd_get(int fd);

/* 安装已 open 的 vfs 句柄到当前任务最小空闲 fd；失败返回 -1（EMFILE/NOMEM） */
int fd_install(vfs_file_t *vf);

/* 关闭当前任务的 fd；成功返回 VFS_OK */
int fd_close(int fd);

/* fork：子表指针复用父描述（refs++，offset 共享，语义同 dup2 全表版） */
void fd_clone_table(struct task *dst, struct task *src);

/* 任务回收：逐项 close（引用归零才真正 vfs_close） */
void fd_table_cleanup(struct task *t);

/* oldfd 共享到 newfd（同描述、共享 offset）；成功返回 newfd，失败返回 -1 */
int fd_dup2(int oldfd, int newfd);

#endif /* CSOS_FD_H */
