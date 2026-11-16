#ifndef CSOS_FS_PIPE_H
#define CSOS_FS_PIPE_H

#include <kernel/fs/vfs.h>

/* 匿名管道 + 命名管道（pipefs）共享的环形缓冲原语。
 * pipe_shared_t 对使用者不透明，只传指针。 */

typedef struct pipe_shared pipe_shared_t;

/* 新建缓冲（refs=0）；调用方负责首个 hold（端点/节点归属）。 */
pipe_shared_t *pipe_shared_new(void);

/* 增/减持有；减到 0 即释放。 */
void pipe_shared_hold(pipe_shared_t *sh);
void pipe_shared_release(pipe_shared_t *sh);

/* 建一个端点句柄：mnt 为端点所在挂载点（匿名 pipe 用内部假位，
 * fifo 用 pipefs 真位），open_count +1 与 vfs_close 的 unpin 配对。
 * flags 原样存入句柄（vfs 层按低 2 位做方向校验）。失败返回 NULL。 */
vfs_file_t *pipe_make_file(pipe_shared_t *sh, int is_write,
                           vfs_mount_t *mnt, int flags);

/* 建一对匿名端点（读/写各一）：成功 0，失败 <0。 */
int pipe_create(vfs_file_t **r_out, vfs_file_t **w_out);

/* 阻塞等对端 open（见上）；仅 pipefs 用，匿名 pipe 不需要。 */
void pipe_wait_peer(pipe_shared_t *sh, int is_write);

/* 端点读写驱动：pipe_make_file 建出的句柄把 drv 指向它，
 * pipefs 直接复用，无需自己的 read/write/close。 */
extern const fs_driver_t pipe_driver;

#endif /* CSOS_FS_PIPE_H */
