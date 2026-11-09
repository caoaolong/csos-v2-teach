#include <dev/console_dev.h>
#include <gfx/console.h>
#include <serial.h>
#include <vdev.h>

/* console：framebuffer + 串口双输出，只写设备 */
static int64_t console_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP;
}

static int64_t console_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    if (len == 0)
        return 0;
    fb_console_write(buf, len);
    serial_write_raw(buf, len);
    return (int64_t)len;
}

static const vdev_char_ops_t g_console_ops = {
    .read = console_read,
    .write = console_write,
};

static vdev_t g_console = {
    .name = "console",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_TTY,
    .minor = 1,
    .ops = &g_console_ops,
};

void init_console_dev()
{
    if (vdev_register(&g_console) != VFS_OK)
        put_string("[CONSOLE] register console failed\n");
}
