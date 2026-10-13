#ifndef CSOS_VDEV_H
#define CSOS_VDEV_H

#include <stdint.h>
#include <stddef.h>
#include <fs/vfs.h>

#define VDEV_NAME_MAX 64

typedef enum
{
    VDEV_CHAR = 1, /* 字符设备 */
} vdev_type_t;

#define VDEV_MAJOR_MEM 1
#define VDEV_MAJOR_TTY 4
#define VDEV_MAJOR_INPUT 13

typedef struct vdev vdev_t;

typedef struct vdev_char_ops
{
    int (*open)(vdev_t *dev, vfs_file_t *f);
    int (*close)(vdev_t *dev, vfs_file_t *f);
    int64_t (*read)(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len);
    int64_t (*write)(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len);
} vdev_char_ops_t;

struct vdev
{
    char name[VDEV_NAME_MAX];
    vdev_type_t type;
    uint32_t major;
    uint32_t minor;
    const vdev_char_ops_t *ops;
    void *priv;
    struct vdev *next;
};

/* 注册表 */
void init_vdev(void);
int vdev_register(vdev_t *dev);
int vdev_unregister(vdev_t *dev);
vdev_t *vdev_find(const char *name);
uint32_t vdev_count(void);

typedef struct vdev_listener
{
    int (*added)(void *ctx, vdev_t *dev);
    int (*removed)(void *ctx, vdev_t *dev);
    void *ctx;
    struct vdev_listener *next;
} vdev_listener_t;

void vdev_listen(vdev_listener_t *l);
void vdev_unlisten(vdev_listener_t *l);

#endif /* CSOS_VDEV_H */
