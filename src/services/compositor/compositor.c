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
#define COMP_TITLE_ACTIVE 0x000078D7u
#define COMP_TITLE_IDLE 0x00606060u
#define COMP_TITLE_H 28 /* 未缩放标题栏高，实际乘 g_scale */
#define COMP_BORDER 1
#define MOUSE_SPEED 2            /* 指针加速：宿主走 1 像素，guest 走 2 像素，免得够不着边 */
#define TASKBAR_H 28             /* 未缩放任务栏高，实际乘 g_scale */
#define DESK_STRIPE 0x00161B21u  /* 壁纸暗纹 */
#define TASKBAR_BG 0x002A2A30u   /* 任务栏底 */
#define TASKBTN_FACE 0x00383840u /* 任务栏按钮面 */

/* v1 窗口机制：单客户端独占一个窗口 */
/* g_frame 含标题栏/边框；g_win 为客户区（裁剪与坐标原点） */
/* 失焦窗口标题栏变灰；多客户端（共享 fifo 交错）暂不支持 */

static uint32_t *g_px;
static uint32_t g_stride;
static uint32_t g_width;
static uint32_t g_height;
static comp_rect_t g_frame;
static comp_rect_t g_win;
static int g_win_valid; /* 0=等 CREATE */
static char g_win_title[32];
static uint32_t g_mbuttons; /* 按键电平：bit0 左 bit1 右 bit2 中 */
static int g_dragging;      /* 标题栏拖拽中 */
static int g_close_armed;   /* 在关闭钮上按下，松开即关窗 */
static int g_spawn_armed;   /* 在任务栏 term 钮上按下，松开即拉起 */
static int g_spawn_pending; /* 已 fork 等对端 CREATE；防连点起两个 */
static int g_spawn_ttl;     /* pending 超时计数，CREATE 前消费完则清 pending */
/* 握手完成：两端 fifo 均已打开并配对；断线后置 0，下个 CREATE 重配 evt 端 */
static int g_hs_ok;
static int g_drag_ox; /* 抓取点相对窗左上角偏移 */
static int g_drag_oy;
static uint32_t g_scale = 1; /* 字号整数倍率：DPI>140 用 2x，上限 2 */
static uint32_t g_phys_w_mm; /* 显示器物理宽（EDID，0=未知） */

static uint32_t g_cx;
static uint32_t g_cy;
static uint32_t g_max_x;
static uint32_t g_max_y;

static int g_cmdfd = -1; /* C→S：读 */
static int g_evtfd = -1; /* S→C：写 */
static comp_reader_t g_cmdr;

static int ipc_handshake(void);
/* 大缓冲一律放 bss（用户栈仅一页），main 栈帧只留标量 */
static input_event_t g_mevs[32];
static input_event_t g_kevs[32];
static uint8_t g_payload[COMP_MAX_PAYLOAD];
static comp_hdr_t g_cmdh;
static uint8_t g_sendb[COMP_MAX_PAYLOAD + sizeof(comp_hdr_t)];
static unsigned long g_evt_drops;

/* 光标下像素保存区：erase 时恢复原像素，不涂底色 */
/* g_cursor_px/py 记录绘制位置：paint 同位重复调用直接返回，保证幂等 */
static uint32_t g_cursor_save[CURSOR_WIDTH * CURSOR_HEIGHT];
static int g_cursor_visible;
static uint32_t g_cursor_px;
static uint32_t g_cursor_py;

static void cursor_paint(uint32_t cx, uint32_t cy, int erase)
{
    uint32_t i;
    uint32_t j;

    if (erase)
    {
        if (!g_cursor_visible)
            return;
        cx = g_cursor_px; /* 在绘制处恢复，与传入坐标无关 */
        cy = g_cursor_py;
        for (j = 0; j < CURSOR_HEIGHT; j++)
        {
            uint32_t at;

            if (cy + j >= g_height)
                break;
            for (i = 0; i < CURSOR_WIDTH; i++)
            {
                if (cx + i >= g_width)
                    break;
                at = (unsigned long)(cy + j) * g_stride + cx + i;
                g_px[at] = g_cursor_save[j * CURSOR_WIDTH + i];
            }
        }
        g_cursor_visible = 0;
        return;
    }
    if (g_cursor_visible)
    {
        if (cx == g_cursor_px && cy == g_cursor_py)
            return;                                /* 已画在此处，保存区仍有效 */
        cursor_paint(g_cursor_px, g_cursor_py, 1); /* 先清旧处再画新处 */
    }
    for (j = 0; j < CURSOR_HEIGHT; j++)
    {
        uint32_t at;
        uint32_t c;

        if (cy + j >= g_height)
            break;
        for (i = 0; i < CURSOR_WIDTH; i++)
        {
            if (cx + i >= g_width)
                break;
            at = (unsigned long)(cy + j) * g_stride + cx + i;
            g_cursor_save[j * CURSOR_WIDTH + i] = g_px[at];
            c = CURSOR[j * CURSOR_WIDTH + i];
            if (c != 0)
                g_px[at] = c;
        }
    }
    g_cursor_px = cx;
    g_cursor_py = cy;
    g_cursor_visible = 1;
}

static void clear_screen(void); /* 定义在后：重画整屏桌面 */
static void paint_desktop_rect(uint32_t x, uint32_t y, uint32_t w,
                               uint32_t h); /* 定义在后：壁纸 + 任务栏 */

/* 任务栏高（像素） */
static uint32_t taskbar_h(void)
{
    return TASKBAR_H * g_scale;
}

static void clear_screen(void)
{
    g_cursor_visible = 0; /* 整屏重写，保存区失效 */
    paint_desktop_rect(0, 0, g_width, g_height);
}

static uint32_t clamp_u32(int v, uint32_t max)
{
    if (v < 0)
        return 0;
    if ((uint32_t)v > max)
        return max;
    return (uint32_t)v;
}

/* 指定裁剪区画实心矩形（坐标为屏坐标） */
static void comp_fill_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color, const comp_rect_t *clip)
{
    uint32_t i;
    uint32_t j;
    uint32_t x0 = x < clip->x ? clip->x : x;
    uint32_t y0 = y < clip->y ? clip->y : y;
    uint32_t x1 = x + w > clip->x + clip->w ? clip->x + clip->w : x + w;
    uint32_t y1 = y + h > clip->y + clip->h ? clip->y + clip->h : y + h;

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

/* 窗口内裁剪画实心矩形 */
static void comp_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    comp_fill_clip(x, y, w, h, color, &g_win);
}

/* 在指定裁剪区画一个字形（按 g_scale 放大） */
static void comp_glyph_clip(uint32_t x, uint32_t y, char ch, uint32_t fg,
                            uint32_t bg, const comp_rect_t *clip)
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

            if (py < clip->y || py >= clip->y + clip->h || py >= g_height)
                continue;
            for (c = 0; c < COMP_FONT_W; c++)
            {
                uint32_t color =
                    (bits & (0x80u >> c)) ? fg : bg;

                for (sx = 0; sx < g_scale; sx++)
                {
                    uint32_t px = x + c * g_scale + sx;

                    if (px < clip->x || px >= clip->x + clip->w ||
                        px >= g_width)
                        continue;
                    g_px[(unsigned long)py * g_stride + px] = color;
                }
            }
        }
    }
}

/* 在窗口内画一个字形（按 g_scale 放大） */
static void comp_glyph(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg)
{
    comp_glyph_clip(x, y, ch, fg, bg, &g_win);
}

/* 任务栏 term 按钮区（屏坐标）；矮屏无任务栏时 w=h=0 */
static void taskbtn_rect(comp_rect_t *r)
{
    uint32_t bh = taskbar_h();
    uint32_t bw;
    uint32_t bh2;

    r->x = 0;
    r->y = 0;
    r->w = 0;
    r->h = 0;
    if (g_height <= bh)
        return;
    bw = (uint32_t)(4 * COMP_FONT_W + 12) * g_scale;
    bh2 = bh > 8 * g_scale ? bh - 8 * g_scale : bh;
    r->x = 4 * g_scale;
    r->y = g_height - bh + (bh > bh2 ? (bh - bh2) / 2 : 0);
    r->w = bw;
    r->h = bh2;
}

/* 点是否落在任务栏 term 钮内 */
static int in_taskbtn(uint32_t x, uint32_t y)
{
    comp_rect_t b;

    taskbtn_rect(&b);
    if (b.w == 0)
        return 0;
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* 重画桌面区（壁纸条纹 + 任务栏 + term 按钮）：不碰光标，调用方先藏光标 */
static void paint_desktop_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    comp_rect_t scr;
    comp_rect_t clip;
    uint32_t bh;
    uint32_t by;
    uint32_t ry0;
    uint32_t ry1;
    uint32_t j;
    uint32_t k;
    comp_rect_t btn;
    uint32_t tyy;
    static const char tlabel[4] = {'t', 'e', 'r', 'm'};

    scr.x = 0;
    scr.y = 0;
    scr.w = g_width;
    scr.h = g_height;
    /* 壁纸：底色 + 每 8 像素一条暗纹 */
    comp_fill_clip(x, y, w, h, COMP_BG_RGB, &scr);
    for (j = (y + 7) / 8 * 8; j < y + h && j < g_height; j += 8)
        comp_fill_clip(x, j, w, 1, DESK_STRIPE, &scr);
    /* 任务栏（屏底）；矮屏跳过 */
    bh = taskbar_h();
    if (g_height <= bh || y >= g_height)
        return;
    by = g_height - bh;
    ry0 = y > by ? y : by;
    ry1 = y + h < g_height ? y + h : g_height;
    if (ry1 <= ry0)
        return;
    clip.x = x;
    clip.y = ry0;
    clip.w = w;
    clip.h = ry1 - ry0;
    comp_fill_clip(x, ry0, w, ry1 - ry0, TASKBAR_BG, &clip);
    /* term 按钮：描边 + 面 + 文字；运行中（有窗）则灰显，点之不重开 */
    taskbtn_rect(&btn);
    if (btn.w == 0)
        return;
    {
        uint32_t bface = g_win_valid ? TASKBAR_BG : TASKBTN_FACE;
        uint32_t bfg = g_win_valid ? 0x00888888u : COMP_FG_RGB;

        comp_fill_clip(btn.x, btn.y, btn.w, 1, bfg, &clip);
        comp_fill_clip(btn.x, btn.y + btn.h - 1, btn.w, 1, bfg, &clip);
        comp_fill_clip(btn.x, btn.y, 1, btn.h, bfg, &clip);
        comp_fill_clip(btn.x + btn.w - 1, btn.y, 1, btn.h, bfg, &clip);
        comp_fill_clip(btn.x + 1, btn.y + 1, btn.w - 2, btn.h - 2,
                       bface, &clip);
        tyy = btn.y + (btn.h > 16 * g_scale ? (btn.h - 16 * g_scale) / 2 : 1);
        for (k = 0; k < 4; k++)
            comp_glyph_clip(btn.x + (6 + k * COMP_FONT_W) * g_scale, tyy,
                            tlabel[k], bfg, bface, &clip);
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

    cursor_paint(g_cx, g_cy, 1); /* 先藏光标，FILL 可能盖住它 */
    comp_fill(g_win.x + x, g_win.y + y, w, h, color);
    cursor_paint(g_cx, g_cy, 0); /* FILL 后重存：若已同位显示则无操作 */
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
    cursor_paint(g_cx, g_cy, 1); /* 先藏光标，文本可能盖住它 */
    for (i = 0; i < n; i++)
        comp_glyph(g_win.x + x + (uint32_t)(i * COMP_FONT_W * g_scale),
                   g_win.y + y, (char)p[14 + i], color, COMP_BG_RGB);
    cursor_paint(g_cx, g_cy, 0); /* 文本后重存：若已同位显示则无操作 */
}

static int evt_send(uint16_t type, const void *payload, unsigned long len)
{
    int retries;
    int r;

    if (type == COMP_MSG_EVTS || type == COMP_MSG_CURSOR)
        retries = 0;
    else
        retries = 500;
    for (;;)
    {
        r = comp_try_send(g_evtfd, type, payload, len, g_sendb);
        if (r == 0)
            return 0;
        if (r < 0)
            return -1; /* 无读端等硬错误 */
        if (retries-- <= 0)
        {
            g_evt_drops++;
            return (type == COMP_MSG_EVTS || type == COMP_MSG_CURSOR) ? 0 : -1;
        }
        sleep(2);
    }
}

/* 转发一批输入事件给客户端 */
static int forward_events(const input_event_t *evs, long nev)
{
    return evt_send(COMP_MSG_EVTS, evs,
                    (unsigned long)(nev * (long)sizeof(input_event_t)));
}

/* 标题栏高（像素，未缩放部分乘 g_scale 使用） */
static uint32_t title_h(void)
{
    return COMP_TITLE_H * g_scale;
}

/* 标题栏颜色：聚焦亮蓝，失焦灰 */
static uint32_t title_color(void)
{
    return g_win_valid ? COMP_TITLE_ACTIVE : COMP_TITLE_IDLE;
}

/* 关闭按钮：标题栏右侧方块；边长 th-8*scale，边距 4*scale */
static void close_rect(comp_rect_t *r)
{
    uint32_t th = title_h();
    uint32_t side = th > 8 * g_scale ? th - 8 * g_scale : th;
    uint32_t m = 4 * g_scale;

    r->w = side;
    r->h = side;
    r->x = g_frame.x + g_frame.w - COMP_BORDER - m - side;
    r->y = g_frame.y + COMP_BORDER + m;
}

/* 画窗框：1px 边框 + 标题栏 + 标题文本 + 关闭按钮 */
static void draw_frame(void)
{
    uint32_t th = title_h();
    uint32_t i;
    uint32_t color = title_color();
    comp_rect_t clip;
    comp_rect_t cb;

    if (!g_win_valid)
        return;
    cursor_paint(g_cx, g_cy, 1); /* 边框重画前先藏光标 */
    /* 边框（整框描边） */
    clip.x = 0;
    clip.y = 0;
    clip.w = g_width;
    clip.h = g_height;
    comp_fill_clip(g_frame.x, g_frame.y, g_frame.w, 1, color, &clip);
    comp_fill_clip(g_frame.x, g_frame.y + g_frame.h - 1, g_frame.w, 1, color, &clip);
    comp_fill_clip(g_frame.x, g_frame.y, 1, g_frame.h, color, &clip);
    comp_fill_clip(g_frame.x + g_frame.w - 1, g_frame.y, 1, g_frame.h, color, &clip);
    /* 标题栏 */
    clip.x = g_frame.x;
    clip.y = g_frame.y;
    clip.w = g_frame.w;
    clip.h = g_frame.h;
    comp_fill_clip(g_frame.x + 1, g_frame.y + 1, g_frame.w - 2, th, color, &clip);
    close_rect(&cb);
    for (i = 0; i < 31 && g_win_title[i] != '\0'; i++)
    {
        uint32_t tx;

        tx = g_frame.x + 2 + 8 * g_scale + i * COMP_FONT_W * g_scale;
        if (tx + COMP_FONT_W * g_scale > cb.x)
            break; /* 给关闭按钮留位 */
        comp_glyph_clip(tx, g_frame.y + 1 + 6 * g_scale, g_win_title[i],
                        COMP_FG_RGB, color, &clip);
    }
    /* 关闭按钮：描边方块 + X */
    comp_fill_clip(cb.x, cb.y, cb.w, 1, COMP_FG_RGB, &clip);
    comp_fill_clip(cb.x, cb.y + cb.h - 1, cb.w, 1, COMP_FG_RGB, &clip);
    comp_fill_clip(cb.x, cb.y, 1, cb.h, COMP_FG_RGB, &clip);
    comp_fill_clip(cb.x + cb.w - 1, cb.y, 1, cb.h, COMP_FG_RGB, &clip);
    comp_glyph_clip(cb.x + (cb.w > 8 * g_scale ? (cb.w - 8 * g_scale) / 2 : 0),
                    cb.y + (cb.h > 16 * g_scale ? (cb.h - 16 * g_scale) / 2 : 0),
                    'X', COMP_FG_RGB, color, &clip);
    /* 有窗时任务栏按钮灰显（光标已藏，直接重画） */
    paint_desktop_rect(0, g_height - taskbar_h(), g_width, taskbar_h());
    cursor_paint(g_cx, g_cy, 0);
}

/* 下发窗口几何 + 焦点 + 整窗 EXPOSE */
static int send_win_geom(void); /* 前置声明，send_cursor 后定义 */

/* 下发光标绝对屏坐标（钳制后）：仅变化时发送 */
static int g_cursor_sent;
static uint32_t g_sent_cx;
static uint32_t g_sent_cy;

static int send_cursor(void)
{
    comp_cursor_t c;

    if (!g_win_valid)
        return 0;
    if (g_cursor_sent && g_sent_cx == g_cx && g_sent_cy == g_cy)
        return 0;
    c.x = g_cx;
    c.y = g_cy;
    {
        int r = comp_try_send(g_evtfd, COMP_MSG_CURSOR, &c, sizeof(c),
                              g_sendb);

        if (r == 0)
        {
            g_sent_cx = g_cx;
            g_sent_cy = g_cy;
            g_cursor_sent = 1;
            return 0;
        }
        if (r > 0)
        {
            g_evt_drops++; /* 不记已发送，下个主循环同位重发 */
            return 0;
        }
        return -1;
    }
}

/* 下发窗口几何 + 焦点 + 整窗 EXPOSE，末尾补发光标绝对位置 */
static int send_win_geom(void)
{
    comp_win_t w;
    comp_rect_t dmg;
    uint32_t focused = 1;

    w.x = g_win.x;
    w.y = g_win.y;
    w.w = g_win.w;
    w.h = g_win.h;
    w.scale = g_scale;
    if (evt_send(COMP_MSG_WIN_GEOM, &w, sizeof(w)) != 0)
        return -1;
    if (evt_send(COMP_MSG_FOCUS, &focused, sizeof(focused)) != 0)
        return -1;
    dmg.x = 0;
    dmg.y = 0;
    dmg.w = 0;
    dmg.h = 0;
    if (evt_send(COMP_MSG_EXPOSE, &dmg, sizeof(dmg)) != 0)
        return -1;
    return send_cursor(); /* CREATE 后客户端立即拿到初始光标位置 */
}

/* 移动窗口：清旧框、裁剪定位、画新框、下发几何与重画 */
static int move_window(int nx, int ny)
{
    /* 窗框不得压住任务栏：活动区为屏高减任务栏高 */
    uint32_t dh = g_height > taskbar_h() ? g_height - taskbar_h() : g_height;
    int max_x = (int)g_width - (int)g_frame.w;
    int max_y = (int)dh - (int)g_frame.h;

    if (!g_win_valid)
        return -1;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (max_x < 0 || max_y < 0)
        return -1;
    if (nx > max_x)
        nx = max_x;
    if (ny > max_y)
        ny = max_y;
    if ((uint32_t)nx == g_frame.x && (uint32_t)ny == g_frame.y)
        return 0;
    cursor_paint(g_cx, g_cy, 1); /* 先恢复光标下像素，再清旧框 */
    paint_desktop_rect(g_frame.x, g_frame.y, g_frame.w, g_frame.h);
    g_frame.x = (uint32_t)nx;
    g_frame.y = (uint32_t)ny;
    g_win.x = g_frame.x + COMP_BORDER;
    g_win.y = g_frame.y + COMP_BORDER + title_h();
    draw_frame();
    printf("compositor: window moved to %d,%d\n", nx, ny);
    return send_win_geom();
}

/* 命中测试：关闭按钮返回 3，标题栏返回 1，客户区返回 2，框外返回 0 */
static int hit_frame(uint32_t x, uint32_t y)
{
    uint32_t th = title_h();
    comp_rect_t cb;

    if (!g_win_valid)
        return 0;
    if (x < g_frame.x || x >= g_frame.x + g_frame.w ||
        y < g_frame.y || y >= g_frame.y + g_frame.h)
        return 0;
    close_rect(&cb);
    if (x >= cb.x && x < cb.x + cb.w && y >= cb.y && y < cb.y + cb.h)
        return 3;
    if (y < g_frame.y + COMP_BORDER + th)
        return 1;
    return 2;
}

/* CREATE 建窗：分配框体（0=最大化并居中）、画框、下发几何与焦点 */
static int on_create(const uint8_t *p, unsigned long len)
{
    uint32_t want_w;
    uint32_t want_h;
    uint32_t cw;
    uint32_t ch;
    uint32_t i;

    if (g_win_valid)
    {
        printf("compositor: CREATE ignored (single window)\n");
        return 0;
    }
    if (!g_hs_ok)
    {
        /* 重连的客户端：重开 evt 写端（非阻塞，永不卡主循环）；
           对端正持读端等 WIN_GEOM，立即配对 */
        if (g_evtfd >= 0)
            close(g_evtfd);
        g_evtfd = (int)open(COMP_EVT_FIFO, O_WRONLY | O_NONBLOCK);
        if (g_evtfd < 0)
            return -1;
        g_hs_ok = 1;
        printf("compositor: client reconnected\n");
    }
    g_spawn_pending = 0; /* 等到的 CREATE 消费掉 pending */
    g_spawn_ttl = 0;
    if (len < 8)
        return -1;
    want_w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    want_h = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
             ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    for (i = 0; i < 31 && i + 8 < len; i++)
    {
        char c = (char)p[8 + i];

        if (c == '\0')
            break;
        g_win_title[i] = c;
    }
    g_win_title[i < 31 ? i : 31] = '\0';
    if (g_win_title[0] == '\0')
    {
        g_win_title[0] = 'w';
        g_win_title[1] = 'i';
        g_win_title[2] = 'n';
        g_win_title[3] = '\0';
    }
    /* 客户区：0=占满桌面（任务栏以上）；否则按需居中 */
    {
        uint32_t dh = g_height > taskbar_h() ? g_height - taskbar_h()
                                             : g_height;

        cw = want_w != 0 ? want_w : g_width;
        ch = want_h != 0 ? want_h : dh;
        if (cw > g_width)
            cw = g_width;
        if (ch > dh)
            ch = dh;
        g_frame.w = cw + 2 * COMP_BORDER;
        g_frame.h = ch + 2 * COMP_BORDER + title_h();
        if (g_frame.w > g_width)
            g_frame.w = g_width;
        if (g_frame.h > dh)
            g_frame.h = dh;
        g_frame.x = (g_width - g_frame.w) / 2;
        g_frame.y = (dh - g_frame.h) / 2;
    }
    g_win.x = g_frame.x + COMP_BORDER;
    g_win.y = g_frame.y + COMP_BORDER + title_h();
    g_win.w = g_frame.w - 2 * COMP_BORDER;
    g_win.h = g_frame.h - 2 * COMP_BORDER - title_h();
    g_win_valid = 1;
    draw_frame();
    printf("compositor: window '%s' %ux%u scale %u (phys %umm)\n",
           g_win_title, g_win.w, g_win.h, g_scale, g_phys_w_mm);
    return send_win_geom();
}

/* 客户端断开/DESTROY/关窗：回无窗状态；不断 fd（新客户端直接复用）， */
/* 已是无窗无连接态则静默返回，避免 EOF 轮询刷屏 */
static void client_reset(const char *why)
{
    if (!g_hs_ok && !g_win_valid)
        return;
    printf("compositor: client lost (%s, evt drops %lu)\n", why, g_evt_drops);
    g_win_valid = 0;
    g_dragging = 0;
    g_close_armed = 0;
    g_spawn_armed = 0;
    g_cursor_sent = 0;
    g_hs_ok = 0;
    g_evt_drops = 0; /* 新纪元重新计数 */
    clear_screen();
    cursor_paint(g_cx, g_cy, 0);
}

/* 点任务栏 term 钮：双 fork 拉起 term；孙进程由 init 接管并回收 */
static void spawn_term(void)
{
    long pid = fork();

    if (pid < 0)
    {
        printf("compositor: fork term failed\n");
        return;
    }
    if (pid == 0)
    {
        long pid2 = fork();

        if (pid2 < 0)
            exit(1);
        if (pid2 == 0)
        {
            char *const argv[] = {"term", NULL};

            execve("/bin/term", argv, NULL);
            printf("compositor: execve term failed\n");
            exit(1);
        }
        exit(0); /* 中间进程退出，断开与 compositor 的父子关系 */
    }
    {
        int st;

        wait(&st); /* 回收中间进程（转瞬即逝，不久等） */
    }
    g_spawn_pending = 1;
    g_spawn_ttl = 400; /* 约 2s 内等 CREATE，超时则允许再点 */
    printf("compositor: spawn term requested\n");
}

/* 用户点关闭钮：擦窗、下发 CLOSE，客户端回 DESTROY 后退出 */
static void do_close(void)
{
    if (!g_win_valid)
        return;
    printf("compositor: window close requested\n");
    g_close_armed = 0;
    g_dragging = 0;
    cursor_paint(g_cx, g_cy, 1); /* 先藏光标再清框 */
    paint_desktop_rect(g_frame.x, g_frame.y, g_frame.w, g_frame.h);
    g_win_valid = 0;
    /* 窗口已关，任务栏切回空闲态（亮按钮） */
    paint_desktop_rect(0, g_height - taskbar_h(), g_width, taskbar_h());
    cursor_paint(g_cx, g_cy, 0);
    if (g_evtfd >= 0)
        evt_send(COMP_MSG_CLOSE, NULL, 0); /* 尽力通知，失败走 EOF */
    client_reset("closed");
}

/* 与客户端握手：建 fifo、开两端（阻塞等首个对端）；窗口等 CREATE 到达 */
static int ipc_handshake(void)
{

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
    g_cmdfd = (int)open(COMP_CMD_FIFO, O_RDONLY | O_NONBLOCK);
    if (g_cmdfd < 0)
    {
        printf("compositor: open cmd failed\n");
        return -1;
    }
    g_evtfd = (int)open(COMP_EVT_FIFO, O_WRONLY | O_NONBLOCK);
    if (g_evtfd < 0)
    {
        printf("compositor: open evt failed\n");
        close(g_cmdfd);
        g_cmdfd = -1;
        return -1;
    }
    g_win_valid = 0;
    g_dragging = 0;
    g_close_armed = 0;
    g_spawn_armed = 0;
    g_cursor_sent = 0; /* 新客户端尚未同步过光标位置 */
    g_evt_drops = 0;
    g_hs_ok = 0; /* 等首个 CREATE 到达才算连接，对端未到前 EOF 静默 */
    printf("compositor: waiting for client...\n");
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
    /* 热点钳制：光标左上点在屏内即可，保证屏上每点都可点到 */
    g_max_x = g_width > 0 ? g_width - 1 : 0;
    g_max_y = g_height > 0 ? g_height - 1 : 0;

    printf("compositor: input ready %ux%u, waiting client\n",
           g_width, g_height);
    if (ipc_handshake() != 0) /* 打开即返（皆非阻塞），等首个 CREATE 才算连接 */
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
        int mhave = 0;
        int fr;
        input_event_t *mevs = g_mevs;
        input_event_t *kevs = g_kevs;
        uint8_t *payload = g_payload;
        comp_hdr_t *h = &g_cmdh;

        if (g_spawn_pending && g_spawn_ttl > 0 && --g_spawn_ttl == 0)
            g_spawn_pending = 0; /* 超时还没 CREATE，放行重点 */
        /* 鼠标：SYN 分组移动光标，标题栏拖拽移窗，整批转发客户端 */
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
                    {
                        if (mevs[i].value)
                        {
                            int hit;

                            g_mbuttons |= 0x01u;
                            hit = hit_frame(g_cx, g_cy);
                            printf("compositor: left down @%u,%u hit=%d\n",
                                   g_cx, g_cy, hit);
                            if (hit == 1)
                            {
                                g_dragging = 1;
                                g_drag_ox = (int)g_cx - (int)g_frame.x;
                                g_drag_oy = (int)g_cy - (int)g_frame.y;
                                printf("compositor: drag start off=%d,%d\n",
                                       g_drag_ox, g_drag_oy);
                            }
                            else if (hit == 3)
                            {
                                g_close_armed = 1;
                                printf("compositor: close armed @%u,%u\n",
                                       g_cx, g_cy);
                            }
                            else if (hit == 0 && in_taskbtn(g_cx, g_cy))
                            {
                                g_spawn_armed = 1;
                                printf("compositor: spawn armed @%u,%u\n",
                                       g_cx, g_cy);
                            }
                        }
                        else
                        {
                            g_mbuttons &= (uint32_t)~0x01u;
                            if (g_close_armed)
                            {
                                g_close_armed = 0;
                                if (hit_frame(g_cx, g_cy) == 3)
                                    do_close();
                            }
                            else if (g_spawn_armed)
                            {
                                g_spawn_armed = 0;
                                /* 已运行则不重开（暂时规则）：亮钮点之无反应 */
                                if (in_taskbtn(g_cx, g_cy) && !g_win_valid &&
                                    !g_hs_ok && !g_spawn_pending)
                                    spawn_term();
                                else
                                    printf("compositor: spawn ignored\n");
                            }
                            else if (g_dragging)
                            {
                                printf("compositor: drag end\n");
                                g_dragging = 0;
                            }
                        }
                        mhave = 1;
                    }
                    else if (mevs[i].code == INPUT_BTN_RIGHT)
                    {
                        g_mbuttons = mevs[i].value ? g_mbuttons | 0x02u
                                                   : g_mbuttons & (uint32_t)~0x02u;
                        mhave = 1;
                    }
                    else if (mevs[i].code == INPUT_BTN_MIDDLE)
                    {
                        g_mbuttons = mevs[i].value ? g_mbuttons | 0x04u
                                                   : g_mbuttons & (uint32_t)~0x04u;
                        mhave = 1;
                    }
                }
                else if (mevs[i].type == INPUT_EV_SYN && mhave)
                {
                    int nx = (int)g_cx + mdx * MOUSE_SPEED;
                    int ny = (int)g_cy + mdy * MOUSE_SPEED;

                    cursor_paint(g_cx, g_cy, 1);
                    g_cx = clamp_u32(nx, g_max_x);
                    g_cy = clamp_u32(ny, g_max_y);
                    cursor_paint(g_cx, g_cy, 0);
                    if (g_close_armed && hit_frame(g_cx, g_cy) != 3)
                        g_close_armed = 0; /* 移出关闭钮，取消待关闭 */
                    if (g_spawn_armed && !in_taskbtn(g_cx, g_cy))
                        g_spawn_armed = 0; /* 移出 term 钮，取消待拉起 */
                    if (g_dragging)
                        move_window((int)g_cx - g_drag_ox, (int)g_cy - g_drag_oy);
                    mdx = 0;
                    mdy = 0;
                    mhave = 0;
                }
            }
            if (g_evtfd >= 0 && g_win_valid && forward_events(mevs, nev) != 0)
            {
                client_reset("evt write");
                continue;
            }
            if (send_cursor() != 0) /* 光标动过则下发绝对位置 */
            {
                client_reset("cursor write");
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
            if (g_evtfd >= 0 && g_win_valid && forward_events(kevs, nev) != 0)
            {
                client_reset("evt write");
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
                    client_reset("cmd EOF");
                    break;
                }
                idle = 0;
                if (h->type == COMP_MSG_FILL && g_win_valid)
                    exec_fill(payload);
                else if (h->type == COMP_MSG_TEXT && g_win_valid)
                    exec_text(payload, h->len);
                else if (h->type == COMP_MSG_CREATE)
                {
                    if (on_create(payload, h->len) != 0)
                    {
                        client_reset("CREATE failed");
                        break;
                    }
                }
                else if (h->type == COMP_MSG_DESTROY)
                {
                    client_reset("DESTROY");
                    break;
                }
                else if (h->type == COMP_MSG_MOVE_REQ && g_win_valid)
                {
                    if (h->len >= 8)
                    {
                        int mx = (int)((uint32_t)payload[0] |
                                       ((uint32_t)payload[1] << 8) |
                                       ((uint32_t)payload[2] << 16) |
                                       ((uint32_t)payload[3] << 24));
                        int my = (int)((uint32_t)payload[4] |
                                       ((uint32_t)payload[5] << 8) |
                                       ((uint32_t)payload[6] << 16) |
                                       ((uint32_t)payload[7] << 24));

                        move_window(mx, my);
                    }
                }
            }
        }

        if (idle)
            sleep(5);
    }
    return 0;
}
