#ifndef CSOS_FS_VFS_H
#define CSOS_FS_VFS_H

#include <stdint.h>
#include <stddef.h>

/* ---- 通用错误码（负数，与 ATA rc 风格兼容） ---- */
#define VFS_OK 0
#define VFS_ERR_INVAL -3
#define VFS_ERR_NOMEM -4
#define VFS_ERR_NOENT -5
#define VFS_ERR_EXIST -6
#define VFS_ERR_NODEV -7
#define VFS_ERR_NOTDIR -8
#define VFS_ERR_ISDIR -9
#define VFS_ERR_NOSPC -10
#define VFS_ERR_NOTSUP -11
#define VFS_ERR_BUSY -12

/* ---- open flags（兼容 POSIX 常用子集） ---- */
#define VFS_O_RDONLY 0x00
#define VFS_O_WRONLY 0x01
#define VFS_O_RDWR 0x02
#define VFS_O_CREAT 0x40
#define VFS_O_TRUNC 0x200
#define VFS_O_APPEND 0x400

/* ---- seek whence ---- */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#define VFS_PATH_MAX 256
#define VFS_NAME_MAX 64
#define VFS_MAX_DRIVERS 8
#define VFS_MAX_MOUNTS 8

typedef struct vfs_mount vfs_mount_t;
typedef struct vfs_file vfs_file_t;

/*
 * Strategy 接口
 */
typedef struct fs_driver
{
    const char *name;
    int (*mount)(vfs_mount_t *m, const char *dev);
    int (*unmount)(vfs_mount_t *m);
    int (*open)(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f);
    int (*close)(vfs_file_t *f);
    int64_t (*read)(vfs_file_t *f, void *buf, uint64_t len);
    int64_t (*write)(vfs_file_t *f, const void *buf, uint64_t len);
    int64_t (*seek)(vfs_file_t *f, int64_t off, int whence);
    int (*stat)(vfs_mount_t *m, const char *rel, uint64_t *size_out, uint32_t *is_dir_out);
} fs_driver_t;

/* Facade 句柄 */
struct vfs_file
{
    vfs_mount_t *mnt;
    const fs_driver_t *drv;
    uint64_t pos;
    int flags;
    uint32_t is_dir;
    uint64_t size; /* open 时快照，seek END 用；读写后由驱动回写 */
    void *priv;    /* 各 FS 的 inode/簇号等，vfs.c 不解释 */
};

/* 挂载点 */
struct vfs_mount
{
    int used;
    char mountpoint[VFS_PATH_MAX];
    const fs_driver_t *drv;
    void *priv; /* 各 FS 的 superblock，vfs.c 不解释 */
    char dev[VFS_PATH_MAX];
};

/* Facade：全局唯一入口，具体 FS 可插拔 */
void init_vfs();
int vfs_register_driver(const fs_driver_t *drv);
int vfs_mount(const char *fs_name, const char *mountpoint, const char *dev);
int vfs_unmount(const char *mountpoint);

int vfs_open(const char *path, int flags, vfs_file_t **out);
int vfs_close(vfs_file_t *f);
int64_t vfs_read(vfs_file_t *f, void *buf, uint64_t len);
int64_t vfs_write(vfs_file_t *f, const void *buf, uint64_t len);
int64_t vfs_seek(vfs_file_t *f, int64_t off, int whence);
int vfs_stat(const char *path, uint64_t *size_out, uint32_t *is_dir_out);

#endif /* CSOS_FS_VFS_H */
