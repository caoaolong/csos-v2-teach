#include <dev/timer_dev.h>
#include <serial.h>
#include <string.h>
#include <timer.h>
#include <vdev.h>

static int64_t uptime_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    char tmp[24];
    uint32_t slen;
    uint64_t v;
    int i = (int)sizeof(tmp) - 1;
    uint64_t remain;
    uint64_t n;

    (void)dev;
    if (len == 0)
        return 0;

    v = uptime_ms();
    tmp[sizeof(tmp) - 1] = '\n';
    if (v == 0)
        tmp[--i] = '0';
    else
    {
        while (v > 0 && i > 0)
        {
            tmp[--i] = (char)('0' + v % 10);
            v /= 10;
        }
    }
    slen = (uint32_t)(sizeof(tmp) - (uint32_t)i);
    if (f->pos >= slen)
        return 0;
    remain = slen - f->pos;
    n = len < remain ? len : remain;
    kernel_memcpy(buf, tmp + i + f->pos, (uint32_t)n);
    f->pos += n;
    return (int64_t)n;
}

static int64_t uptime_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP; /* 只读时钟 */
}

static const vdev_char_ops_t g_uptime_ops = {
    .read = uptime_read,
    .write = uptime_write,
};

static vdev_t g_uptime = {
    .name = "uptime",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_MISC,
    .minor = 0,
    .ops = &g_uptime_ops,
};

void init_timer_dev()
{
    if (vdev_register(&g_uptime) != VFS_OK)
        put_string("[TIMER] register uptime failed\n");
}
