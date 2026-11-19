#ifndef CSOS_DIRENT_H
#define CSOS_DIRENT_H

#include <stdint.h>

/* 目录项（内核↔用户态共享布局）：name 与 VFS_NAME_MAX 同宽 64 */
#define CSOS_NAME_MAX 64

typedef struct csos_dirent
{
    char name[CSOS_NAME_MAX];
    uint32_t is_dir;
    uint64_t size;
} csos_dirent_t;

#endif /* CSOS_DIRENT_H */
