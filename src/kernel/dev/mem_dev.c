#include <dev/mem_dev.h>
#include <serial.h>
#include <string.h>
#include <vdev.h>

/* Linux 兼容次设备号：null=3，zero=5 */
#define MEM_MINOR_NULL 3
#define MEM_MINOR_ZERO 5

static int64_t null_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return 0; /* EOF */
}

static int64_t null_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    return (int64_t)len; /* 丢弃 */
}

static int64_t zero_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    if (len == 0)
        return 0;
    kernel_memset(buf, 0, (uint32_t)len);
    return (int64_t)len;
}

static int64_t zero_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    return (int64_t)len; /* 丢弃 */
}

static const vdev_char_ops_t g_null_ops = {
    .read = null_read,
    .write = null_write,
};

static const vdev_char_ops_t g_zero_ops = {
    .read = zero_read,
    .write = zero_write,
};

static vdev_t g_null = {
    .name = "null",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_MEM,
    .minor = MEM_MINOR_NULL,
    .ops = &g_null_ops,
};

static vdev_t g_zero = {
    .name = "zero",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_MEM,
    .minor = MEM_MINOR_ZERO,
    .ops = &g_zero_ops,
};

void init_mem_dev()
{
    if (vdev_register(&g_null) != VFS_OK)
        put_string("[MEM] register null failed\n");
    if (vdev_register(&g_zero) != VFS_OK)
        put_string("[MEM] register zero failed\n");
}
