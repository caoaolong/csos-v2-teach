#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fbinfo.h>
#include <input.h>
#include <gfx/cursor.h>

#define COMP_BG_RGB 0x000F0F12u
#define COMP_DOT_RGB 0x00FFFFFFu
#define COMP_CLICK_RGB 0x00FF4040u
#define KBD_STEP 8

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
                px[at] = COMP_BG_RGB;
                continue;
            }
            c = CURSOR[j * CURSOR_WIDTH + i];
            if (c != 0)
                px[at] = c;
        }
    }
}

/* 在屏内画实心方块（键盘回显/点击落点用，持久保留，不随光标擦除） */
static void draw_block(uint32_t *px, uint32_t stride, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, uint32_t color,
                       uint32_t width, uint32_t height)
{
    uint32_t i;
    uint32_t j;

    for (j = 0; j < h; j++)
    {
        if (y + j >= height)
            break;
        for (i = 0; i < w; i++)
        {
            if (x + i >= width)
                break;
            px[(unsigned long)(y + j) * stride + x + i] = color;
        }
    }
}

static void clear_screen(uint32_t *px, uint32_t stride,
                         uint32_t width, uint32_t height)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
            px[(unsigned long)y * stride + x] = COMP_BG_RGB;
    }
}

static uint32_t clamp_u32(int v, uint32_t max)
{
    if (v < 0)
        return 0;
    if ((uint32_t)v > max)
        return max;
    return (uint32_t)v;
}

/* 键盘按下后的可见反馈：wasd/方向键移光标，c 清屏，其余回显方块 */
static void handle_key_press(char ch, uint16_t code, uint32_t *pcx, uint32_t *pcy,
                             uint32_t max_x, uint32_t max_y, uint32_t *px,
                             uint32_t stride, uint32_t width, uint32_t height,
                             uint32_t *pecho_x, uint32_t echo_y)
{
    uint32_t cx = *pcx;
    uint32_t cy = *pcy;
    uint32_t echo_x = *pecho_x;
    int nx = (int)cx;
    int ny = (int)cy;

    /* 串口回显（接管后 console 只走串口，屏上另用方块回显） */
    printf("compositor: key '%c' (code=%u)\n",
           ch != 0 && ch >= 32 && ch < 127 ? ch : '.', (unsigned)code);
    if (ch == 'w' || code == INPUT_KEY_UP)
        ny -= KBD_STEP;
    else if (ch == 's' || code == INPUT_KEY_DOWN)
        ny += KBD_STEP;
    else if (ch == 'a' || code == INPUT_KEY_LEFT)
        nx -= KBD_STEP;
    else if (ch == 'd' || code == INPUT_KEY_RIGHT)
        nx += KBD_STEP;
    else if (ch == 'c')
    {
        clear_screen(px, stride, width, height);
        echo_x = 8;
    }
    if (nx != (int)cx || ny != (int)cy)
    {
        cursor_paint(px, stride, cx, cy, width, height, 1);
        cx = clamp_u32(nx, max_x);
        cy = clamp_u32(ny, max_y);
        cursor_paint(px, stride, cx, cy, width, height, 0);
    }
    /* 屏上回显：底行画一个 8x8 方块，按键则进一格 */
    if (ch == '\n')
    {
        echo_x = 8;
    }
    else if (ch == '\b')
    {
        if (echo_x >= 8 + 10)
            echo_x -= 10;
        draw_block(px, stride, echo_x, echo_y, 8, 8, COMP_BG_RGB,
                   width, height);
    }
    else if (ch != 0)
    {
        uint32_t color = (ch == ' ') ? COMP_CLICK_RGB : COMP_DOT_RGB;

        if (echo_x + 8 < width)
        {
            draw_block(px, stride, echo_x, echo_y, 8, 8, color,
                       width, height);
            cursor_paint(px, stride, cx, cy, width, height, 0);
            echo_x += 10;
        }
        else
        {
            echo_x = 8;
        }
    }
    *pcx = cx;
    *pcy = cy;
    *pecho_x = echo_x;
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
    int mfd;
    int kfd;
    uint32_t cx;
    uint32_t cy;
    uint32_t max_x;
    uint32_t max_y;
    uint32_t echo_x; /* 键盘回显块横坐标（底行） */
    uint32_t echo_y;

    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
    {
        printf("compositor: open /dev/fb0 failed\n");
        exit(1);
    }
    n = read(fd, &info, sizeof(info));
    if (n != (long)sizeof(info))
    {
        printf("compositor: fb info read failed n=%ld\n", n);
        exit(1);
    }
    close((int)fd);
    if (fbctl(FB_ACQUIRE) != 0)
    {
        printf("compositor: fb acquire failed\n");
        exit(1);
    }
    px = (uint32_t *)fbmap();
    if (px == (void *)0 || px == (void *)-1)
    {
        printf("compositor: fbmap failed\n");
        exit(1);
    }
    stride = info.pitch / 4;
    for (y = 0; y < info.height; y++)
    {
        for (x = 0; x < info.width; x++)
            px[(unsigned long)y * stride + x] = COMP_BG_RGB;
    }

    /* 鼠标 + 键盘双事件接收：两路都输出 input_event_t，以 O_NONBLOCK 轮询。
     * read 无数据时回 -1/EAGAIN（不阻塞），有事件则批量耗尽，避免 starvation。
     * 鼠标按 SYN 分组提交一次光标移动；键盘只响应 value==PRESS 的按下事件。 */
    mfd = (int)open("/dev/mouse0", O_RDONLY | O_NONBLOCK);
    if (mfd < 0)
    {
        printf("compositor: open /dev/mouse0 failed\n");
        exit(1);
    }
    kfd = (int)open("/dev/kbd0", O_RDONLY | O_NONBLOCK);
    if (kfd < 0)
    {
        printf("compositor: open /dev/kbd0 failed\n");
        exit(1);
    }
    cx = info.width / 2;
    cy = info.height / 2;
    max_x = info.width > CURSOR_WIDTH ? info.width - CURSOR_WIDTH : 0;
    max_y = info.height > CURSOR_HEIGHT ? info.height - CURSOR_HEIGHT : 0;
    echo_x = 8;
    echo_y = info.height > 24 ? info.height - 16 : 0;

    printf("compositor: input ready %ux%u\n", info.width, info.height);
    cursor_paint(px, stride, cx, cy, info.width, info.height, 0);
    for (;;)
    {
        int idle = 1;
        input_event_t mevs[32];
        input_event_t kevs[32];
        long r;
        long kr;
        long i;
        int mdx = 0;
        int mdy = 0;
        uint32_t mbuttons = 0;
        int mhave = 0;

        /* --- 鼠标：耗尽 input_event_t，按 SYN 分组移动光标 --- */
        errno = 0;
        r = read(mfd, mevs, sizeof(mevs));
        if (r > 0)
        {
            long nev = r / (long)sizeof(input_event_t);

            idle = 0;
            for (i = 0; i < nev; i++)
            {
                if (mevs[i].type == INPUT_EV_REL)
                {
                    if (mevs[i].code == INPUT_REL_X)
                    {
                        mdx += mevs[i].value;
                        mhave = 1;
                    }
                    else if (mevs[i].code == INPUT_REL_Y)
                    {
                        mdy += mevs[i].value;
                        mhave = 1;
                    }
                }
                else if (mevs[i].type == INPUT_EV_KEY)
                {
                    if (mevs[i].code == INPUT_BTN_LEFT)
                    {
                        if (mevs[i].value == INPUT_KEY_PRESS)
                            mbuttons |= 0x01;
                        else
                            mbuttons &= (uint32_t)~0x01u;
                        mhave = 1;
                    }
                    else if (mevs[i].code == INPUT_BTN_RIGHT)
                    {
                        if (mevs[i].value == INPUT_KEY_PRESS)
                            mbuttons |= 0x02;
                        else
                            mbuttons &= (uint32_t)~0x02u;
                        mhave = 1;
                    }
                    else if (mevs[i].code == INPUT_BTN_MIDDLE)
                    {
                        if (mevs[i].value == INPUT_KEY_PRESS)
                            mbuttons |= 0x04;
                        else
                            mbuttons &= (uint32_t)~0x04u;
                        mhave = 1;
                    }
                }
                else if (mevs[i].type == INPUT_EV_SYN)
                {
                    if (mhave)
                    {
                        int nx = (int)cx + mdx;
                        int ny = (int)cy + mdy;

                        cursor_paint(px, stride, cx, cy,
                                     info.width, info.height, 1);
                        cx = clamp_u32(nx, max_x);
                        cy = clamp_u32(ny, max_y);
                        cursor_paint(px, stride, cx, cy,
                                     info.width, info.height, 0);
                        if (mbuttons & 0x01)
                        {
                            /* 左键按住：热点落笔（持久白点） */
                            draw_block(px, stride, cx, cy, 4, 4,
                                       COMP_DOT_RGB,
                                       info.width, info.height);
                            cursor_paint(px, stride, cx, cy,
                                         info.width, info.height, 0);
                        }
                        if (mbuttons & 0x02)
                        {
                            /* 右键按住：清屏并重画光标 */
                            clear_screen(px, stride,
                                         info.width, info.height);
                            cursor_paint(px, stride, cx, cy,
                                         info.width, info.height, 0);
                            echo_x = 8;
                        }
                        mdx = 0;
                        mdy = 0;
                        mhave = 0;
                    }
                }
            }
        }

        /* --- 键盘：耗尽 input_event_t，只响应按下事件 --- */
        errno = 0;
        kr = read(kfd, kevs, sizeof(kevs));
        if (kr > 0)
        {
            long nev = kr / (long)sizeof(input_event_t);

            idle = 0;
            for (i = 0; i < nev; i++)
            {
                char ch;

                if (kevs[i].type != INPUT_EV_KEY ||
                    kevs[i].value != INPUT_KEY_PRESS)
                    continue;
                ch = input_key_to_ascii(kevs[i].code);
                handle_key_press(ch, kevs[i].code, &cx, &cy,
                                 max_x, max_y, px, stride,
                                 info.width, info.height,
                                 &echo_x, echo_y);
            }
        }

        if (idle)
            sleep(5); /* 两路都无事件：让出 CPU，避免空转 */
    }
    return 0;
}
