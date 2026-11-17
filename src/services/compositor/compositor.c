#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fbinfo.h>
#include <input.h>
#include <comp_ipc.h>
#include "font.h"
#include <gfx/cursor.h>

#define COMP_BG_RGB 0x000F0F12u
#define COMP_FG_RGB 0x00E8E8E8u

/* v1 单客户端：窗口即全屏，compositor 独占输入并只画光标 */
/* 客户端像素归客户端；光标擦除按底色重填，会盖掉光标下像素 */

static uint32_t *g_px;
static uint32_t g_stride;
static uint32_t g_width;
static uint32_t g_height;
static comp_rect_t g_win;
static uint32_t g_scale = 1; /* 字号整数倍率：DPI>140 用 2x，上限 2 */
static uint32_t g_phys_w_mm; /* 显示器物理宽（EDID，0=未知） */

static uint32_t g_cx;
static uint32_t g_cy;
static uint32_t g_max_x;
static uint32_t g_max_y;

static int g_cmdfd = -1; /* C→S：读 */
static int g_evtfd = -1; /* S→C：写 */
static comp_reader_t g_cmdr;

/* 大缓冲一律放 bss（用户栈仅一页），main 栈帧只留标量 */
static input_event_t g_mevs[32];
static input_event_t g_kevs[32];
static uint8_t g_payload[COMP_MAX_PAYLOAD];
static comp_hdr_t g_cmdh;

static void cursor_paint(uint32_t cx, uint32_t cy, int erase)
{
    uint32_t i;
    uint32_t j;

    for (j = 0; j < CURSOR_HEIGHT; j++)
    {
        if (cy + j >= g_height)
            break;
        for (i = 0; i < CURSOR_WIDTH; i++)
        {
            uint32_t at;
            uint32_t c;

            if (cx + i >= g_width)
                break;
            at = (unsigned long)(cy + j) * g_stride + cx + i;
            if (erase)
            {
                g_px[at] = COMP_BG_RGB;
                continue;
            }
            c = CURSOR[j * CURSOR_WIDTH + i];
            if (c != 0)
                g_px[at] = c;
        }
    }
}

static void clear_screen(void)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < g_height; y++)
    {
        for (x = 0; x < g_width; x++)
            g_px[(unsigned long)y * g_stride + x] = COMP_BG_RGB;
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

/* 窗口内裁剪画实心矩形 */
static void comp_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t i;
    uint32_t j;
    uint32_t x0 = x < g_win.x ? g_win.x : x;
    uint32_t y0 = y < g_win.y ? g_win.y : y;
    uint32_t x1 = x + w > g_win.x + g_win.w ? g_win.x + g_win.w : x + w;
    uint32_t y1 = y + h > g_win.y + g_win.h ? g_win.y + g_win.h : y + h;

    if (x1 <= x0 || y1 <= y0)
        return;
    if (x0 >= g_width || y0 >= g_height)
        return;
    if (x1 > g_width)
        x1 = g_width;
    if (y1 > g_height)
        y1 = g_height;
    for (j = y0; j < y1; j++)
    {
        for (i = x0; i < x1; i++)
            g_px[(unsigned long)j * g_stride + i] = color;
    }
}

/* 在窗口内画一个字形（按 g_scale 放大） */
static void comp_glyph(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *g = comp_font_glyph(ch);
    uint32_t r;
    uint32_t c;
    uint32_t sx;
    uint32_t sy;

    for (r = 0; r < COMP_FONT_H; r++)
    {
        uint8_t bits = g[r];

        for (sy = 0; sy < g_scale; sy++)
        {
            uint32_t py = y + r * g_scale + sy;

            if (py < g_win.y || py >= g_win.y + g_win.h || py >= g_height)
                continue;
            for (c = 0; c < COMP_FONT_W; c++)
            {
                uint32_t color =
                    (bits & (0x80u >> c)) ? fg : bg;

                for (sx = 0; sx < g_scale; sx++)
                {
                    uint32_t px = x + c * g_scale + sx;

                    if (px < g_win.x || px >= g_win.x + g_win.w ||
                        px >= g_width)
                        continue;
                    g_px[(unsigned long)py * g_stride + px] = color;
                }
            }
        }
    }
}

/* 执行客户端命令 */
static void exec_fill(const uint8_t *p)
{
    uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t y = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                 ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    uint32_t w = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                 ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    uint32_t h = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                 ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    uint32_t color = (uint32_t)p[16] | ((uint32_t)p[17] << 8) |
                     ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);

    comp_fill(g_win.x + x, g_win.y + y, w, h, color);
}

static void exec_text(const uint8_t *p, unsigned long len)
{
    uint32_t x;
    uint32_t y;
    uint32_t color;
    unsigned long n;
    unsigned long i;

    if (len < 14)
        return;
    x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    y = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
        ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    color = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
            ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    n = (unsigned long)p[12] | ((unsigned long)p[13] << 8);
    if (14 + n > len)
        return;
    for (i = 0; i < n; i++)
        comp_glyph(g_win.x + x + (uint32_t)(i * COMP_FONT_W * g_scale),
                   g_win.y + y, (char)p[14 + i], color, COMP_BG_RGB);
    cursor_paint(g_cx, g_cy, 0); /* 文本盖住光标时重画 */
}

/* 转发一批输入事件给客户端 */
static int forward_events(const input_event_t *evs, long nev)
{
    return (int)comp_send(g_evtfd, COMP_MSG_EVTS, evs,
                          (unsigned long)(nev * (long)sizeof(input_event_t)));
}

/* 与客户端握手：建 fifo、开两端、下发窗口 */
static int ipc_handshake(void)
{
    comp_win_t w;

    if (g_cmdfd >= 0)
        close(g_cmdfd);
    if (g_evtfd >= 0)
        close(g_evtfd);
    g_cmdfd = -1;
    g_evtfd = -1;
    comp_reader_init(&g_cmdr);
    if (mkfifo(COMP_CMD_FIFO, 0) != 0 && errno != EEXIST)
    {
        printf("compositor: mkfifo cmd failed\n");
        return -1;
    }
    if (mkfifo(COMP_EVT_FIFO, 0) != 0 && errno != EEXIST)
    {
        printf("compositor: mkfifo evt failed\n");
        return -1;
    }
    /* 先开 cmd 读端再开 evt 写端（与客户端对称等待） */
    g_cmdfd = (int)open(COMP_CMD_FIFO, O_RDONLY | O_NONBLOCK);
    if (g_cmdfd < 0)
    {
        printf("compositor: open cmd failed\n");
        return -1;
    }
    g_evtfd = (int)open(COMP_EVT_FIFO, O_WRONLY);
    if (g_evtfd < 0)
    {
        printf("compositor: open evt failed\n");
        close(g_cmdfd);
        g_cmdfd = -1;
        return -1;
    }
    g_win.x = 0;
    g_win.y = 0;
    g_win.w = g_width;
    g_win.h = g_height;
    w.x = g_win.x;
    w.y = g_win.y;
    w.w = g_win.w;
    w.h = g_win.h;
    w.scale = g_scale;
    printf("compositor: win %ux%u scale %u (phys %umm)\n",
           g_width, g_height, g_scale, g_phys_w_mm);
    if (comp_send(g_evtfd, COMP_MSG_WIN, &w, sizeof(w)) != 0)
    {
        printf("compositor: send WIN failed\n");
        close(g_cmdfd);
        close(g_evtfd);
        g_cmdfd = -1;
        g_evtfd = -1;
        return -1;
    }
    printf("compositor: client connected\n");
    return 0;
}

int main()
{
    fb_info_t info;
    long fd;
    long n;
    uint32_t x;
    uint32_t y;
    int mfd;
    int kfd;

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
    g_px = (uint32_t *)fbmap();
    if (g_px == (void *)0 || g_px == (void *)-1)
    {
        printf("compositor: fbmap failed\n");
        exit(1);
    }
    g_stride = info.pitch / 4;
    g_width = info.width;
    g_height = info.height;
    g_phys_w_mm = info.phys_w_mm;
    /* DPI 定字号：超 140 用 2x；无物理尺寸时按高度保底 */
    g_scale = 1;
    if (g_phys_w_mm != 0 && g_width != 0)
    {
        if ((uint64_t)254 * (uint64_t)g_width >
            (uint64_t)1400 * (uint64_t)g_phys_w_mm)
            g_scale = 2;
    }
    else if (g_height >= 1600)
    {
        g_scale = 2;
    }
    for (y = 0; y < g_height; y++)
    {
        for (x = 0; x < g_width; x++)
            g_px[(unsigned long)y * g_stride + x] = COMP_BG_RGB;
    }

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
    g_cx = g_width / 2;
    g_cy = g_height / 2;
    g_max_x = g_width > CURSOR_WIDTH ? g_width - CURSOR_WIDTH : 0;
    g_max_y = g_height > CURSOR_HEIGHT ? g_height - CURSOR_HEIGHT : 0;

    printf("compositor: input ready %ux%u, waiting client\n",
           g_width, g_height);
    if (ipc_handshake() != 0)
        exit(1);
    clear_screen();
    cursor_paint(g_cx, g_cy, 0);
    for (;;)
    {
        int idle = 1;
        long r;
        long kr;
        long i;
        int mdx = 0;
        int mdy = 0;
        uint32_t mbuttons = 0;
        int mhave = 0;
        int fr;
        input_event_t *mevs = g_mevs;
        input_event_t *kevs = g_kevs;
        uint8_t *payload = g_payload;
        comp_hdr_t *h = &g_cmdh;

        /* 鼠标：SYN 分组移动光标，整批转发客户端 */
        errno = 0;
        r = read(mfd, mevs, sizeof(g_mevs));
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
                        mbuttons = mevs[i].value ? mbuttons | 0x01u
                                                 : mbuttons & ~0x01u;
                    else if (mevs[i].code == INPUT_BTN_RIGHT)
                        mbuttons = mevs[i].value ? mbuttons | 0x02u
                                                 : mbuttons & ~0x02u;
                    else if (mevs[i].code == INPUT_BTN_MIDDLE)
                        mbuttons = mevs[i].value ? mbuttons | 0x04u
                                                 : mbuttons & ~0x04u;
                    mhave = 1;
                }
                else if (mevs[i].type == INPUT_EV_SYN && mhave)
                {
                    int nx = (int)g_cx + mdx;
                    int ny = (int)g_cy + mdy;

                    cursor_paint(g_cx, g_cy, 1);
                    g_cx = clamp_u32(nx, g_max_x);
                    g_cy = clamp_u32(ny, g_max_y);
                    cursor_paint(g_cx, g_cy, 0);
                    mdx = 0;
                    mdy = 0;
                    mhave = 0;
                }
            }
            if (g_evtfd >= 0 && forward_events(mevs, nev) != 0)
            {
                printf("compositor: client lost, re-handshake\n");
                if (ipc_handshake() != 0)
                    exit(1);
                clear_screen();
                cursor_paint(g_cx, g_cy, 0);
                continue;
            }
        }

        /* 键盘：按下打日志，其余原样转发 */
        errno = 0;
        kr = read(kfd, kevs, sizeof(g_kevs));
        if (kr > 0)
        {
            long nev = kr / (long)sizeof(input_event_t);

            idle = 0;
            for (i = 0; i < nev; i++)
            {
                if (kevs[i].type == INPUT_EV_KEY &&
                    kevs[i].value == INPUT_KEY_PRESS)
                {
                    char ch = input_key_to_ascii(kevs[i].code);

                    printf("compositor: key '%c' (code=%u)\n",
                           ch != 0 && ch >= 32 && ch < 127 ? ch : '.',
                           (unsigned)kevs[i].code);
                }
            }
            if (g_evtfd >= 0 && forward_events(kevs, nev) != 0)
            {
                printf("compositor: client lost, re-handshake\n");
                if (ipc_handshake() != 0)
                    exit(1);
                clear_screen();
                cursor_paint(g_cx, g_cy, 0);
                continue;
            }
        }

        /* 客户端命令：耗尽并执行 */
        if (g_cmdfd >= 0)
        {
            for (;;)
            {
                errno = 0;
                fr = comp_reader_feed(&g_cmdr, g_cmdfd, h, payload,
                                      sizeof(g_payload));
                if (fr == 0)
                    break;
                if (fr < 0)
                {
                    printf("compositor: client lost, re-handshake\n");
                    if (ipc_handshake() != 0)
                        exit(1);
                    clear_screen();
                    cursor_paint(g_cx, g_cy, 0);
                    break;
                }
                idle = 0;
                if (h->type == COMP_MSG_FILL)
                    exec_fill(payload);
                else if (h->type == COMP_MSG_TEXT)
                    exec_text(payload, h->len);
            }
        }

        if (idle)
            sleep(5);
    }
    return 0;
}
