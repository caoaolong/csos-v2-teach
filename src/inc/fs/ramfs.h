#ifndef CSOS_FS_RAMFS_H
#define CSOS_FS_RAMFS_H

#include <fs/vfs.h>
#include <spinlock.h>

#define RAMFS_DEF_CAP 512

/* 树形节点：子项按名字升序挂在 child 链表上 */
typedef struct ramfs_node
{
    char name[VFS_NAME_MAX];
    uint8_t *data;
    uint64_t size;
    uint64_t cap;
    uint32_t is_dir;
    uint32_t refs; /* 打开句柄 + 目录游标；>0 时禁止 rmdir */
    struct ramfs_node *parent;
    struct ramfs_node *child;
    struct ramfs_node *sibling;
} ramfs_node_t;

typedef struct ramfs_sb
{
    spinlock_t lock;
    ramfs_node_t *root;
} ramfs_sb_t;

extern const fs_driver_t ramfs_driver;

#endif /* CSOS_FS_RAMFS_H */
