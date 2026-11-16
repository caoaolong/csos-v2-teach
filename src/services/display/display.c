#include <unistd.h>
#include <stdio.h>
#include <fbinfo.h>

/* 屏幕底色：与控制台文字背景一致（LOGO_SPLASH_BG_RGB，见 layout.h），
 * 柔和不刺眼，内核后续打印的文字无缝衔接。 */
#define DISP_BG_RGB 0x000F0F12u

int main()
{
    fb_info_t info;
    long fd;
    long n;
    uint32_t *px;
    uint32_t x;
    uint32_t y;
    uint32_t stride;

    printf("display: starting fb test\n");
    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
    {
        printf("display: open /dev/fb0 failed\n");
        exit(1);
    }
    n = read(fd, &info, sizeof(info));
    if (n != (long)sizeof(info))
    {
        printf("display: fb info read failed n=%ld\n", n);
        exit(1);
    }
    printf("display: fb phys=0x%lx %ux%u pitch=%u bpp=%u\n",
           (unsigned long)info.phys_base, info.width, info.height,
           info.pitch, info.bpp);
    close((int)fd);
    if (fbctl(FB_ACQUIRE) != 0)
    {
        printf("display: fb acquire failed\n");
        exit(1);
    }
    px = (uint32_t *)fbmap();
    if (px == (void *)0 || px == (void *)-1)
    {
        printf("display: fbmap failed\n");
        exit(1);
    }
    printf("display: fb mapped at %p, painting background\n", px);
    stride = info.pitch / 4;
    for (y = 0; y < info.height; y++)
    {
        for (x = 0; x < info.width; x++)
            px[(unsigned long)y * stride + x] = DISP_BG_RGB;
    }
    printf("display: paint done\n");
    for (;;)
        sleep(1000); /* 常驻：不再退出，init 无需重启 */
    return 0;
}
