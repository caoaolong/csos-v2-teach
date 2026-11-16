#include <kernel/dev/fb_dev.h>
#include <fbinfo.h>
#include <kernel/serial.h>
#include <kernel/string.h>
#include <kernel/vdev.h>
#include <kernel/memory/vmm.h>

/* fb0：只读字符设备，一次 read 返回 24 字节 fb_info_t（支持 pos 续读）。
 * 映射显存用 fbmap()（SYS_FBMAP），不在这里做。 */

static int64_t fb_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    fb_info_t info;
    uint64_t avail;
    uint8_t *p = (uint8_t *)buf;

    (void)dev;
    if (f->is_dir)
        return VFS_ERR_ISDIR;
    vmm_framebuffer_info(&info.phys_base, NULL, &info.width,
                         &info.height, &info.pitch);
    info.bpp = 32;
    if (info.phys_base == 0)
        return VFS_ERR_NODEV; /* 无显存的机器 */
    avail = sizeof(info) > f->pos ? sizeof(info) - f->pos : 0;
    if (len > avail)
        len = avail;
    if (len == 0)
        return 0; /* EOF */
    kernel_memcpy(p, (uint8_t *)&info + f->pos, (uint32_t)len);
    f->pos += len;
    return (int64_t)len;
}

static int64_t fb_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP; /* 只读设备：画图走映射后的内存 */
}

static const vdev_char_ops_t g_fb_ops = {
    .read = fb_read,
    .write = fb_write,
};

static vdev_t g_fb0 = {
    .name = "fb0",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_MISC,
    .minor = 0,
    .ops = &g_fb_ops,
};

void init_fb_dev()
{
    if (vdev_register(&g_fb0) != VFS_OK)
        put_string("[FB] register fb0 failed\n");
}
