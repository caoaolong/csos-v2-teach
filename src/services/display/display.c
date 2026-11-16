#include <unistd.h>
#include <stdio.h>
#include <fbinfo.h>
#include <minput.h>
#include <gfx/cursor.h>

#define DISP_BG_RGB 0x000F0F12u

static void cursor_paint(uint32_t *px, uint32_t stride, uint32_t cx, uint32_t cy,
                         uint32_t width, uint32_t height, int erase)
{
    uint32_t i;
    uint32_t j;

    for (j = 0; j < CURSOR_HEIGHT; j++)
    {
        if (cy + j >= height)
            break;
        for (i = 0; i < CURSOR_WIDTH; i++)
        {
            uint32_t at;
            uint32_t c;

            if (cx + i >= width)
                break;
            at = (unsigned long)(cy + j) * stride + cx + i;
            if (erase)
            {
                px[at] = DISP_BG_RGB;
                continue;
            }
            c = CURSOR[j * CURSOR_WIDTH + i];
            if (c != 0)
                px[at] = c;
        }
    }
}

int main()
{
    fb_info_t info;
    long fd;
    long n;
    uint32_t *px;
    uint32_t x;
    uint32_t y;
    uint32_t stride;

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
    stride = info.pitch / 4;
    for (y = 0; y < info.height; y++)
    {
        for (x = 0; x < info.width; x++)
            px[(unsigned long)y * stride + x] = DISP_BG_RGB;
    }
    {
        /* 光标事件循环：读 /dev/mouse0，擦旧画新（钳制在屏内） */
        int mfd = (int)open("/dev/mouse0", O_RDONLY);
        uint32_t cx = info.width / 2;
        uint32_t cy = info.height / 2;
        uint32_t max_x = info.width > CURSOR_WIDTH ? info.width - CURSOR_WIDTH : 0;
        uint32_t max_y = info.height > CURSOR_HEIGHT ? info.height - CURSOR_HEIGHT : 0;

        if (mfd < 0)
        {
            printf("display: open /dev/mouse0 failed\n");
            exit(1);
        }
        cursor_paint(px, stride, cx, cy, info.width, info.height, 0);
        for (;;)
        {
            mouse_event_t ev;
            long r = read(mfd, &ev, sizeof(ev));
            int nx;
            int ny;

            if (r != (long)sizeof(ev))
                continue;
            cursor_paint(px, stride, cx, cy, info.width, info.height, 1);
            nx = (int)cx + ev.dx;
            ny = (int)cy + ev.dy;
            if (nx < 0)
                nx = 0;
            if (ny < 0)
                ny = 0;
            if ((uint32_t)nx > max_x)
                nx = (int)max_x;
            if ((uint32_t)ny > max_y)
                ny = (int)max_y;
            cx = (uint32_t)nx;
            cy = (uint32_t)ny;
            cursor_paint(px, stride, cx, cy, info.width, info.height, 0);
        }
    }
    return 0;
}
