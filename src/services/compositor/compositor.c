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

/* v2 多窗口：窗口数组 + z 序（g_z 底→顶存槽位号），rendezvous 对只做分配 */
/* 遮挡裁剪是下一步的事；本步重叠窗口直接覆盖绘制，靠 EXPOSE 让客户端重画 */
struct win
{
    int used;
    int focused;
    comp_rect_t frame; /* 含标题栏/边框 */
    comp_rect_t win;   /* 客户区（裁剪与坐标原点） */
    char title[32];
    int cmdfd; /* C→S 读端（私有对） */
    int evtfd; /* S→C 写端（私有对） */
    comp_reader_t cmdr;
    int established; /* 私有 cmd 上收到过完整帧 */
    int eof_grace;   /* 未建立期的 EOF 计数 */
    int cursor_sent; /* 光标已同步 */
    uint32_t sent_cx;
    uint32_t sent_cy;
};

static struct win g_wins[COMP_MAX_WIN];
static int g_z[COMP_MAX_WIN]; /* z 序，底→顶 */
static int g_nwin;
static int g_focus; /* 聚焦槽位，-1=无 */
static uint32_t g_mbuttons; /* 按键电平：bit0 左 bit1 右 bit2 中 */
static int g_dragwin = -1;  /* 正在拖拽的窗口槽位（单指针） */
static int g_closewin = -1; /* 关闭钮已 arm 的窗口槽位 */
static int g_spawn_armed;   /* 在任务栏启动钮上按下，松开即拉起 */
static int g_spawn_pending; /* 已 fork 等对端 CREATE；防连点起两个 */
static int g_spawn_ttl;     /* pending 超时计数，CREATE 前消费完则清 pending */
static int g_drag_ox; /* 抓取点相对窗左上角偏移 */
static int g_drag_oy;
static unsigned long g_evt_drops;

static uint32_t *g_px;
static uint32_t g_stride;
static uint32_t g_width;
static uint32_t g_height;
static struct win *g_curw; /* 当前服务窗口（exec_* 用它定位客户区） */

/* 后定义，先声明 */
static void draw_frame(struct win *w);
static void paint_taskbar(void);
static void drain_evt_pipe_slot(int idx);
static void drain_fd(int fd);
static void paint_desktop_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static int evt_send_on(struct win *w, uint16_t type, const void *payload,
                       unsigned long len);

/* 按槽位取窗口（须 used） */
static struct win *win_at(int idx)
{
    if (idx < 0 || idx >= COMP_MAX_WIN || !g_wins[idx].used)
        return NULL;
    return &g_wins[idx];
}

/* 分配空槽（返回槽号，满返回 -1）；新窗置顶 */
static int win_alloc(void)
{
    int i;

    for (i = 0; i < COMP_MAX_WIN; i++)
    {
        if (!g_wins[i].used)
        {
            g_wins[i].used = 1;
            g_wins[i].focused = 0;
            g_wins[i].cmdfd = -1;
            g_wins[i].evtfd = -1;
            g_wins[i].established = 0;
            g_wins[i].eof_grace = 0;
            g_wins[i].cursor_sent = 0;
            g_z[g_nwin++] = i;
            return i;
        }
    }
    return -1;
}

/* z 序中摘掉槽位 */
static void win_unlink_z(int idx)
{
    int zi;

    for (zi = 0; zi < g_nwin; zi++)
    {
        if (g_z[zi] == idx)
        {
            for (; zi + 1 < g_nwin; zi++)
                g_z[zi] = g_z[zi + 1];
            g_nwin--;
            return;
        }
    }
}

/* 销毁窗口：排空→关两端→摘槽→焦点/拖拽状态清理 */
static void win_teardown(int idx, const char *why, int drain_evt)
{
    struct win *w = win_at(idx);

    if (w == NULL)
        return;
    printf("compositor: window %d lost (%s, evt drops %lu)\n", idx, why,
           g_evt_drops);
    g_evt_drops = 0;
    drain_fd(w->cmdfd);
    if (w->cmdfd >= 0)
        close(w->cmdfd);
    if (drain_evt) /* CLOSE 已在管道里时跳过，见 do_close */
        drain_evt_pipe_slot(idx);
    if (w->evtfd >= 0)
        close(w->evtfd);
    w->used = 0;
    w->cmdfd = -1;
    w->evtfd = -1;
    win_unlink_z(idx);
    if (g_focus == idx)
        g_focus = -1;
    if (g_dragwin == idx)
        g_dragwin = -1;
    if (g_closewin == idx)
        g_closewin = -1;
}

/* 全量重排：桌面 + 由底到顶画框 + 任务栏（调用方先藏光标） */
static void repaint_all(void)
{
    int zi;

    paint_desktop_rect(0, 0, g_width, g_height);
    for (zi = 0; zi < g_nwin; zi++)
    {
        struct win *w = win_at(g_z[zi]);

        if (w != NULL)
            draw_frame(w);
    }
    paint_taskbar();
}

/* 布局变化后：重排 + 给所有窗发整窗 EXPOSE（客户端重画内容） */
static void layout_changed(void)
{
    comp_rect_t dmg;
    int zi;

    dmg.x = 0;
    dmg.y = 0;
    dmg.w = 0;
    dmg.h = 0;
    repaint_all();
    for (zi = 0; zi < g_nwin; zi++)
    {
        struct win *w = win_at(g_z[zi]);

        if (w != NULL)
            evt_send_on(w, COMP_MSG_EXPOSE, &dmg, sizeof(dmg));
    }
}

/* 聚焦并置顶；已是焦点且置顶则无操作 */
static void win_focus_raise(int idx)
{
    struct win *w = win_at(idx);
    struct win *old;
    uint32_t one = 1;
    uint32_t zero = 0;

    if (w == NULL)
        return;
    old = win_at(g_focus);
    if (old != NULL && old != w)
    {
        old->focused = 0;
        evt_send_on(old, COMP_MSG_FOCUS, &zero, sizeof(zero));
    }
    win_unlink_z(idx);
    g_z[g_nwin++] = idx;
    w->focused = 1;
    g_focus = idx;
    evt_send_on(w, COMP_MSG_FOCUS, &one, sizeof(one));
    layout_changed();
}

/* 焦点空了且还有窗：把焦点给顶窗（teardown 后调用，调用方统一 layout） */
static void refocus_top(void)
{
    struct win *top;
    uint32_t one = 1;

    if (g_focus >= 0 || g_nwin <= 0)
        return;
    top = win_at(g_z[g_nwin - 1]);
    if (top == NULL)
        return;
    top->focused = 1;
    g_focus = g_z[g_nwin - 1];
    evt_send_on(top, COMP_MSG_FOCUS, &one, sizeof(one));
}
static uint32_t g_scale = 1; /* 字号整数倍率：DPI>140 用 2x，上限 2 */
static uint32_t g_phys_w_mm; /* 显示器物理宽（EDID，0=未知） */

static uint32_t g_cx;
static uint32_t g_cy;
static uint32_t g_max_x;
static uint32_t g_max_y;

static int g_cmdfd = -1; /* C→S：读（rendezvous，只收 CREATE） */
static int g_evtfd = -1; /* S→C：写（rendezvous，只发 ASSIGN） */
static comp_reader_t g_cmdr;

static int ipc_handshake(void);
/* 大缓冲一律放 bss（用户栈仅一页），main 栈帧只留标量 */
static input_event_t g_mevs[32];
static input_event_t g_kevs[32];
static uint8_t g_payload[COMP_MAX_PAYLOAD];
static comp_hdr_t g_cmdh;
static uint8_t g_sendb[COMP_MAX_PAYLOAD + sizeof(comp_hdr_t)];

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

/* 窗口内裁剪画实心矩形（当前服务窗口 g_curw） */
static void comp_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (g_curw == NULL)
        return;
    comp_fill_clip(x, y, w, h, color, &g_curw->win);
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

/* 在窗口内画一个字形（按 g_scale 放大，裁剪到当前服务窗口） */
static void comp_glyph(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg)
{
    if (g_curw == NULL)
        return;
    comp_glyph_clip(x, y, ch, fg, bg, &g_curw->win);
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

#define WINBTN_LABEL_MAX 10

/* 第 n 个窗口按钮区（按 z 序排列，跟在启动钮后面） */
static void winbtn_rect(int n, comp_rect_t *r)
{
    comp_rect_t launch;
    uint32_t bw;
    uint32_t bh;

    taskbtn_rect(&launch);
    r->x = 0;
    r->y = 0;
    r->w = 0;
    r->h = 0;
    if (launch.w == 0)
        return;
    bw = (uint32_t)(WINBTN_LABEL_MAX * COMP_FONT_W + 12) * g_scale;
    bh = launch.h;
    r->x = launch.x + launch.w + 4 * g_scale +
            (uint32_t)n * (bw + 4 * g_scale);
    r->y = launch.y;
    r->w = bw;
    r->h = bh;
    if (r->x + r->w > g_width)
    {
        r->w = 0;
        r->h = 0;
    }
}

/* 点落在第几个窗口按钮上（z 序 n），-1=不在任一上 */
static int winbtn_at(uint32_t x, uint32_t y)
{
    int zi;

    for (zi = 0; zi < g_nwin; zi++)
    {
        comp_rect_t b;

        winbtn_rect(zi, &b);
        if (b.w != 0 && x >= b.x && x < b.x + b.w && y >= b.y &&
            y < b.y + b.h)
            return zi;
    }
    return -1;
}

/* 画一个任务栏按钮（描边 + 面 + 文字） */
static void paint_button(const comp_rect_t *btn, const char *label,
                         uint32_t face, uint32_t fg)
{
    comp_rect_t clip;
    uint32_t tyy;
    int k;

    clip = *btn;
    comp_fill_clip(btn->x, btn->y, btn->w, 1, fg, &clip);
    comp_fill_clip(btn->x, btn->y + btn->h - 1, btn->w, 1, fg, &clip);
    comp_fill_clip(btn->x, btn->y, 1, btn->h, fg, &clip);
    comp_fill_clip(btn->x + btn->w - 1, btn->y, 1, btn->h, fg, &clip);
    comp_fill_clip(btn->x + 1, btn->y + 1, btn->w - 2, btn->h - 2, face,
                   &clip);
    tyy = btn->y + (btn->h > 16 * g_scale ? (btn->h - 16 * g_scale) / 2 : 1);
    for (k = 0; k < WINBTN_LABEL_MAX && label[k] != '\0'; k++)
        comp_glyph_clip(btn->x + (6 + k * COMP_FONT_W) * g_scale, tyy,
                        label[k], fg, face, &clip);
}

/* 任务栏按钮行：启动钮 + 每窗一钮（聚焦高亮） */
static void paint_taskbar(void)
{
    comp_rect_t btn;
    int zi;
    static const char tlabel[5] = {'t', 'e', 'r', 'm', '\0'};

    taskbtn_rect(&btn);
    if (btn.w != 0)
    {
        /* 无空槽则灰显，点之不重开 */
        int full = (g_nwin >= COMP_MAX_WIN);
        uint32_t face = full ? TASKBAR_BG : TASKBTN_FACE;
        uint32_t fg = full ? 0x00888888u : COMP_FG_RGB;

        paint_button(&btn, tlabel, face, fg);
    }
    for (zi = 0; zi < g_nwin; zi++)
    {
        struct win *w = win_at(g_z[zi]);
        comp_rect_t b;

        if (w == NULL)
            continue;
        winbtn_rect(zi, &b);
        if (b.w == 0)
            break;
        if (w->focused)
            paint_button(&b, w->title, COMP_TITLE_ACTIVE, COMP_FG_RGB);
        else
            paint_button(&b, w->title, TASKBAR_BG, 0x00888888u);
    }
}

/* 重画桌面区（壁纸条纹 + 任务栏底）：不碰光标，调用方先藏光标 */
static void paint_desktop_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    comp_rect_t scr;
    comp_rect_t clip;
    uint32_t bh;
    uint32_t by;
    uint32_t ry0;
    uint32_t ry1;
    uint32_t j;

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
    if (g_curw != NULL)
        comp_fill(g_curw->win.x + x, g_curw->win.y + y, w, h, color);
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
    if (g_curw == NULL)
        return;
    cursor_paint(g_cx, g_cy, 1); /* 先藏光标，文本可能盖住它 */
    for (i = 0; i < n; i++)
        comp_glyph(g_curw->win.x + x + (uint32_t)(i * COMP_FONT_W * g_scale),
                   g_curw->win.y + y, (char)p[14 + i], color, COMP_BG_RGB);
    cursor_paint(g_cx, g_cy, 0); /* 文本后重存：若已同位显示则无操作 */
}

/* S→C 发送（fd 版）：永不阻塞主循环。内核把写错误（无读端/满）统一
 * 收敛为 -1/EAGAIN，用户态无法区分“对端还在路上”与真硬错误，因此
 * 控制帧一律有限重试 ~1s（每次 yield），耗尽仍失败才判对端已走；
 * 流式帧发一次，满即丢（计数）。返回 0=已发送或已丢弃，-1=对端已走。 */
static int evt_send_fd(int fd, uint16_t type, const void *payload,
                       unsigned long len)
{
    int retries;
    int r;

    if (type == COMP_MSG_EVTS || type == COMP_MSG_CURSOR)
        retries = 0;
    else
        retries = 500;
    for (;;)
    {
        r = comp_try_send(fd, type, payload, len, g_sendb);
        if (r == 0)
            return 0;
        if (retries-- <= 0)
        {
            g_evt_drops++;
            return (type == COMP_MSG_EVTS || type == COMP_MSG_CURSOR) ? 0 : -1;
        }
        sleep(2);
    }
}

/* rendezvous 发送：只用于 WIN_ASSIGN */
static int evt_send(uint16_t type, const void *payload, unsigned long len)
{
    return evt_send_fd(g_evtfd, type, payload, len);
}

/* 窗口私有对发送 */
static int evt_send_on(struct win *w, uint16_t type, const void *payload,
                       unsigned long len)
{
    if (w == NULL || w->evtfd < 0)
        return -1;
    return evt_send_fd(w->evtfd, type, payload, len);
}

/* 私有对路径（slot 为窗口槽位） */
static void priv_pipe_path(char *cmd, char *evt, int idx)
{
    comp_pipe_path(cmd, 64, COMP_CMD_PAT, (unsigned)idx);
    comp_pipe_path(evt, 64, COMP_EVT_PAT, (unsigned)idx);
}

static void drain_fd(int fd)
{
    uint8_t tmp[256];
    int k;

    if (fd < 0)
        return;
    for (k = 0; k < 256; k++)
    {
        errno = 0;
        if (read(fd, tmp, sizeof(tmp)) <= 0)
            break;
    }
}

/* 转发一批输入事件给指定窗口 */
static int forward_events(struct win *w, const input_event_t *evs, long nev)
{
    return evt_send_on(w, COMP_MSG_EVTS, evs,
                       (unsigned long)(nev * (long)sizeof(input_event_t)));
}

/* 标题栏高（像素，未缩放部分乘 g_scale 使用） */
static uint32_t title_h(void)
{
    return COMP_TITLE_H * g_scale;
}

/* 标题栏颜色：聚焦亮蓝，失焦灰 */
static uint32_t title_color(int focused)
{
    return focused ? COMP_TITLE_ACTIVE : COMP_TITLE_IDLE;
}

/* 关闭按钮：标题栏右侧方块；边长 th-8*scale，边距 4*scale */
static void close_rect(struct win *w, comp_rect_t *r)
{
    uint32_t th = title_h();
    uint32_t side = th > 8 * g_scale ? th - 8 * g_scale : th;
    uint32_t m = 4 * g_scale;

    r->w = side;
    r->h = side;
    r->x = w->frame.x + w->frame.w - COMP_BORDER - m - side;
    r->y = w->frame.y + COMP_BORDER + m;
}

/* 画窗框：1px 边框 + 标题栏 + 标题文本 + 关闭按钮 */
static void draw_frame(struct win *w)
{
    uint32_t th = title_h();
    uint32_t i;
    uint32_t color = title_color(w->focused);
    comp_rect_t clip;
    comp_rect_t cb;

    if (w == NULL || !w->used)
        return;
    cursor_paint(g_cx, g_cy, 1); /* 边框重画前先藏光标 */
    /* 边框（整框描边） */
    clip.x = 0;
    clip.y = 0;
    clip.w = g_width;
    clip.h = g_height;
    comp_fill_clip(w->frame.x, w->frame.y, w->frame.w, 1, color, &clip);
    comp_fill_clip(w->frame.x, w->frame.y + w->frame.h - 1, w->frame.w, 1, color, &clip);
    comp_fill_clip(w->frame.x, w->frame.y, 1, w->frame.h, color, &clip);
    comp_fill_clip(w->frame.x + w->frame.w - 1, w->frame.y, 1, w->frame.h, color, &clip);
    /* 标题栏 */
    clip.x = w->frame.x;
    clip.y = w->frame.y;
    clip.w = w->frame.w;
    clip.h = w->frame.h;
    comp_fill_clip(w->frame.x + 1, w->frame.y + 1, w->frame.w - 2, th, color, &clip);
    close_rect(w, &cb);
    for (i = 0; i < 31 && w->title[i] != '\0'; i++)
    {
        uint32_t tx;

        tx = w->frame.x + 2 + 8 * g_scale + i * COMP_FONT_W * g_scale;
        if (tx + COMP_FONT_W * g_scale > cb.x)
            break; /* 给关闭按钮留位 */
        comp_glyph_clip(tx, w->frame.y + 1 + 6 * g_scale, w->title[i],
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
    cursor_paint(g_cx, g_cy, 0);
}

/* 下发窗口几何 + 焦点 + 整窗 EXPOSE */
static int send_win_geom(struct win *w); /* 前置声明，send_cursor 后定义 */

static int send_cursor(struct win *w)
{
    comp_cursor_t c;

    if (w == NULL || !w->used)
        return 0;
    if (w->cursor_sent && w->sent_cx == g_cx && w->sent_cy == g_cy)
        return 0;
    c.x = g_cx;
    c.y = g_cy;
    {
        int r = comp_try_send(w->evtfd, COMP_MSG_CURSOR, &c, sizeof(c),
                              g_sendb);

        if (r == 0)
        {
            w->sent_cx = g_cx;
            w->sent_cy = g_cy;
            w->cursor_sent = 1;
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
static int send_win_geom(struct win *w)
{
    comp_win_t gw;
    comp_rect_t dmg;
    uint32_t focused;

    if (w == NULL || !w->used)
        return -1;
    focused = w->focused ? 1u : 0u;
    gw.x = w->win.x;
    gw.y = w->win.y;
    gw.w = w->win.w;
    gw.h = w->win.h;
    gw.scale = g_scale;
    if (evt_send_on(w, COMP_MSG_WIN_GEOM, &gw, sizeof(gw)) != 0)
        return -1;
    if (evt_send_on(w, COMP_MSG_FOCUS, &focused, sizeof(focused)) != 0)
        return -1;
    dmg.x = 0;
    dmg.y = 0;
    dmg.w = 0;
    dmg.h = 0;
    if (evt_send_on(w, COMP_MSG_EXPOSE, &dmg, sizeof(dmg)) != 0)
        return -1;
    return send_cursor(w); /* CREATE 后客户端立即拿到初始光标位置 */
}

/* 移动窗口：重排重画，下发新几何 */
static int move_window(struct win *w, int nx, int ny)
{
    /* 窗框不得压住任务栏：活动区为屏高减任务栏高 */
    uint32_t dh = g_height > taskbar_h() ? g_height - taskbar_h() : g_height;
    int max_x;
    int max_y;

    if (w == NULL || !w->used)
        return -1;
    max_x = (int)g_width - (int)w->frame.w;
    max_y = (int)dh - (int)w->frame.h;
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
    if ((uint32_t)nx == w->frame.x && (uint32_t)ny == w->frame.y)
        return 0;
    w->frame.x = (uint32_t)nx;
    w->frame.y = (uint32_t)ny;
    w->win.x = w->frame.x + COMP_BORDER;
    w->win.y = w->frame.y + COMP_BORDER + title_h();
    printf("compositor: window moved to %d,%d\n", nx, ny);
    layout_changed();
    return send_win_geom(w);
}

/* 命中测试：关闭按钮返回 3，标题栏返回 1，客户区返回 2，框外返回 0 */
static int hit_frame(struct win *w, uint32_t x, uint32_t y)
{
    uint32_t th = title_h();
    comp_rect_t cb;

    if (w == NULL || !w->used)
        return 0;
    if (x < w->frame.x || x >= w->frame.x + w->frame.w ||
        y < w->frame.y || y >= w->frame.y + w->frame.h)
        return 0;
    close_rect(w, &cb);
    if (x >= cb.x && x < cb.x + cb.w && y >= cb.y && y < cb.y + cb.h)
        return 3;
    if (y < w->frame.y + COMP_BORDER + th)
        return 1;
    return 2;
}

/* 顶层命中：z 序从顶到底，返回槽位号，-1=点空 */
static int hit_top(uint32_t x, uint32_t y)
{
    int zi;

    for (zi = g_nwin - 1; zi >= 0; zi--)
    {
        if (hit_frame(&g_wins[g_z[zi]], x, y) != 0)
            return g_z[zi];
    }
    return -1;
}

/* CREATE 建窗：分配空槽 + 私有对，回 ASSIGN；客户端切私有对后收几何 */
static int on_create(const uint8_t *p, unsigned long len)
{
    uint32_t want_w;
    uint32_t want_h;
    uint32_t cw;
    uint32_t ch;
    uint32_t i;
    int idx;
    struct win *w;
    char cpath[64];
    char epath[64];
    uint32_t wid;

    idx = win_alloc();
    if (idx < 0)
    {
        printf("compositor: CREATE ignored (no free slot)\n");
        return 0;
    }
    w = &g_wins[idx];
    priv_pipe_path(cpath, epath, idx);
    if (mkfifo(cpath, 0) != 0 && errno != EEXIST)
    {
        printf("compositor: mkfifo priv cmd failed\n");
        w->used = 0;
        win_unlink_z(idx);
        return -1;
    }
    if (mkfifo(epath, 0) != 0 && errno != EEXIST)
    {
        printf("compositor: mkfifo priv evt failed\n");
        w->used = 0;
        win_unlink_z(idx);
        return -1;
    }
    /* 先排空复用残留，再开两端（皆非阻塞，打开即返） */
    drain_fd(w->cmdfd);
    if (w->cmdfd >= 0)
        close(w->cmdfd);
    if (w->evtfd >= 0)
        close(w->evtfd);
    w->cmdfd = (int)open(cpath, O_RDONLY | O_NONBLOCK);
    w->evtfd = (int)open(epath, O_WRONLY | O_NONBLOCK);
    if (w->cmdfd < 0 || w->evtfd < 0)
    {
        printf("compositor: priv fifo open failed\n");
        if (w->cmdfd >= 0)
            close(w->cmdfd);
        if (w->evtfd >= 0)
            close(w->evtfd);
        w->cmdfd = -1;
        w->evtfd = -1;
        w->used = 0;
        win_unlink_z(idx);
        return -1;
    }
    comp_reader_init(&w->cmdr);
    w->established = 0;
    w->eof_grace = 0;
    /* rendezvous 上回 ASSIGN，客户端随即切私有对 */
    wid = (uint32_t)idx;
    if (evt_send_fd(g_evtfd, COMP_MSG_WIN_ASSIGN, &wid, sizeof(wid)) != 0)
    {
        printf("compositor: ASSIGN failed\n");
        close(w->cmdfd);
        close(w->evtfd);
        w->cmdfd = -1;
        w->evtfd = -1;
        w->used = 0;
        win_unlink_z(idx);
        return -1;
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
        w->title[i] = c;
    }
    w->title[i < 31 ? i : 31] = '\0';
    if (w->title[0] == '\0')
    {
        w->title[0] = 'w';
        w->title[1] = 'i';
        w->title[2] = 'n';
        w->title[3] = '\0';
    }
    /* 客户区：0=占满桌面（任务栏以上）；所有窗口最大化居中层叠，靠任务栏钮切换 */
    {
        uint32_t dh = g_height > taskbar_h() ? g_height - taskbar_h()
                                             : g_height;

        cw = want_w != 0 ? want_w : g_width;
        ch = want_h != 0 ? want_h : dh;
        if (cw > g_width)
            cw = g_width;
        if (ch > dh)
            ch = dh;
        w->frame.w = cw + 2 * COMP_BORDER;
        w->frame.h = ch + 2 * COMP_BORDER + title_h();
        if (w->frame.w > g_width)
            w->frame.w = g_width;
        if (w->frame.h > dh)
            w->frame.h = dh;
        w->frame.x = (g_width - w->frame.w) / 2;
        w->frame.y = (dh - w->frame.h) / 2;
    }
    w->win.x = w->frame.x + COMP_BORDER;
    w->win.y = w->frame.y + COMP_BORDER + title_h();
    w->win.w = w->frame.w - 2 * COMP_BORDER;
    w->win.h = w->frame.h - 2 * COMP_BORDER - title_h();
    win_focus_raise(idx); /* 置顶 + 重排 + 通知各窗重画 */
    printf("compositor: window %d '%s' %ux%u scale %u (phys %umm)\n",
           idx, w->title, w->win.w, w->win.h, g_scale, g_phys_w_mm);
    if (send_win_geom(w) != 0)
    {
        win_teardown(idx, "GEOM failed", 1);
        return -1;
    }
    return 0;
}

/* 排空指定槽位的 evt 管道残留（临时开读端抽干；调用方保证之后不再写） */
static void drain_evt_pipe_slot(int idx)
{
    char cpath[64];
    char epath[64];
    int t;

    priv_pipe_path(cpath, epath, idx);
    t = (int)open(epath, O_RDONLY | O_NONBLOCK);
    if (t >= 0)
    {
        drain_fd(t);
        close(t);
    }
}

/* 点任务栏启动钮：双 fork 拉起 term；孙进程由 init 接管并回收 */
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

/* 用户点关闭钮：下发 CLOSE（尽力），撕掉窗口并重排，其余窗重画 */
static void do_close(struct win *w)
{
    int idx;

    if (w == NULL || !w->used)
        return;
    idx = (int)(w - g_wins);
    printf("compositor: window %d close requested\n", idx);
    g_closewin = -1;
    g_dragwin = -1;
    if (w->evtfd >= 0)
    {
        /* 先排空残留流式帧，再下发 CLOSE；teardown 时不再排空，
           否则 CLOSE 还没被对端读走就被自己抽掉，对端只见 EOF 会重连 */
        drain_evt_pipe_slot(idx);
        evt_send_fd(w->evtfd, COMP_MSG_CLOSE, NULL, 0); /* 尽力通知 */
    }
    win_teardown(idx, "closed", 0);
    /* 关掉的是焦点：把焦点给新的顶窗，再统一重排 */
    if (g_focus < 0 && g_nwin > 0)
    {
        struct win *top = win_at(g_z[g_nwin - 1]);
        uint32_t one = 1;

        if (top != NULL)
        {
            top->focused = 1;
            g_focus = g_z[g_nwin - 1];
            evt_send_on(top, COMP_MSG_FOCUS, &one, sizeof(one));
        }
    }
    layout_changed();
}

/* 与客户端握手：建 rendezvous 对（皆非阻塞，打开即返）；窗口等 CREATE 到达 */
static int ipc_handshake(void)
{

    if (g_cmdfd >= 0)
        close(g_cmdfd);
    if (g_evtfd >= 0)
        close(g_evtfd);
    g_cmdfd = -1;
    g_evtfd = -1;
    g_nwin = 0;
    g_focus = -1;
    g_dragwin = -1;
    g_closewin = -1;
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
    g_dragwin = -1;
    g_closewin = -1;
    g_spawn_armed = 0;
    g_evt_drops = 0;
    g_focus = -1; /* 等首个 CREATE 到达才算有焦点 */
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
                            int wi;

                            g_mbuttons |= 0x01u;
                            wi = hit_top(g_cx, g_cy);
                            if (wi >= 0)
                            {
                                struct win *w = &g_wins[wi];
                                int hit = hit_frame(w, g_cx, g_cy);

                                printf("compositor: left down @%u,%u win=%d hit=%d\n",
                                       g_cx, g_cy, wi, hit);
                                win_focus_raise(wi);
                                if (hit == 1)
                                {
                                    g_dragwin = wi;
                                    g_drag_ox = (int)g_cx - (int)w->frame.x;
                                    g_drag_oy = (int)g_cy - (int)w->frame.y;
                                    printf("compositor: drag start off=%d,%d\n",
                                           g_drag_ox, g_drag_oy);
                                }
                                else if (hit == 3)
                                {
                                    g_closewin = wi;
                                    printf("compositor: close armed @%u,%u\n",
                                           g_cx, g_cy);
                                }
                            }
                            else if (in_taskbtn(g_cx, g_cy))
                            {
                                g_spawn_armed = 1;
                                printf("compositor: spawn armed @%u,%u\n",
                                       g_cx, g_cy);
                            }
                            else
                            {
                                int zbi = winbtn_at(g_cx, g_cy);

                                if (zbi >= 0)
                                    win_focus_raise(g_z[zbi]);
                            }
                        }
                        else
                        {
                            g_mbuttons &= (uint32_t)~0x01u;
                            if (g_closewin >= 0)
                            {
                                int cw = g_closewin;

                                g_closewin = -1;
                                if (hit_frame(&g_wins[cw], g_cx, g_cy) == 3)
                                    do_close(&g_wins[cw]);
                            }
                            else if (g_spawn_armed)
                            {
                                g_spawn_armed = 0;
                                if (in_taskbtn(g_cx, g_cy) &&
                                    g_nwin < COMP_MAX_WIN && !g_spawn_pending)
                                    spawn_term();
                                else
                                    printf("compositor: spawn ignored\n");
                            }
                            else if (g_dragwin >= 0)
                            {
                                printf("compositor: drag end\n");
                                g_dragwin = -1;
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
                    if (g_closewin >= 0 &&
                        hit_frame(&g_wins[g_closewin], g_cx, g_cy) != 3)
                        g_closewin = -1; /* 移出关闭钮，取消待关闭 */
                    if (g_spawn_armed && !in_taskbtn(g_cx, g_cy))
                        g_spawn_armed = 0; /* 移出启动钮，取消待拉起 */
                    if (g_dragwin >= 0)
                    {
                        struct win *dw = win_at(g_dragwin);

                        if (dw != NULL)
                            move_window(dw, (int)g_cx - g_drag_ox,
                                        (int)g_cy - g_drag_oy);
                        else
                            g_dragwin = -1;
                    }
                    mdx = 0;
                    mdy = 0;
                    mhave = 0;
                }
            }
            /* 鼠标事件发给悬停的顶层窗；光标位置同步给它 */
            {
                int hov = hit_top(g_cx, g_cy);
                struct win *hw = win_at(hov);

                if (hw != NULL && forward_events(hw, mevs, nev) != 0)
                {
                    win_teardown(hov, "evt write", 1);
                    refocus_top();
                    layout_changed();
                    continue;
                }
                if (hw != NULL && send_cursor(hw) != 0)
                {
                    win_teardown(hov, "cursor write", 1);
                    refocus_top();
                    layout_changed();
                    continue;
                }
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
            /* 键盘发给焦点窗 */
            {
                struct win *fw = win_at(g_focus);

                if (fw != NULL && forward_events(fw, kevs, nev) != 0)
                {
                    int f = g_focus;

                    win_teardown(f, "evt write", 1);
                    refocus_top();
                    layout_changed();
                    continue;
                }
            }
        }

        /* 客户端命令：rendezvous 只收 CREATE（其余忽略，EOF 常态静默） */
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
                    break; /* 无写端常态：rendezvous 空闲，保持静默 */
                idle = 0;
                if (h->type == COMP_MSG_CREATE)
                {
                    if (on_create(payload, h->len) != 0)
                        printf("compositor: CREATE failed\n");
                }
                /* 其余类型在 rendezvous 上忽略 */
            }
        }
        /* 各窗口私有对：FILL/TEXT/DESTROY/MOVE_REQ；新窗 EOF 宽限 */
        {
            int wi;

            for (wi = 0; wi < COMP_MAX_WIN; wi++)
            {
                struct win *cw = win_at(wi);

                if (cw == NULL || cw->cmdfd < 0)
                    continue;
                g_curw = cw;
                for (;;)
                {
                    errno = 0;
                    fr = comp_reader_feed(&cw->cmdr, cw->cmdfd, h, payload,
                                          sizeof(g_payload));
                    if (fr == 0)
                    {
                        cw->eof_grace = 0;
                        break;
                    }
                    if (fr < 0)
                    {
                        /* 无写端：刚分配的窗对端可能还在 ASSIGN→open 路上，
                           宽限 ~200 轮再判死刑；已建立连接的直接 teardown */
                        if (cw->established || ++cw->eof_grace > 200)
                        {
                            win_teardown(wi, "cmd EOF", 1);
                            refocus_top();
                            layout_changed();
                        }
                        break;
                    }
                    idle = 0;
                    cw->established = 1;
                    cw->eof_grace = 0;
                    if (h->type == COMP_MSG_FILL)
                        exec_fill(payload);
                    else if (h->type == COMP_MSG_TEXT)
                        exec_text(payload, h->len);
                    else if (h->type == COMP_MSG_DESTROY)
                    {
                        win_teardown(wi, "DESTROY", 1);
                        refocus_top();
                        layout_changed();
                        break;
                    }
                    else if (h->type == COMP_MSG_MOVE_REQ)
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

                            move_window(cw, mx, my);
                        }
                    }
                    /* CREATE 在私有对上非法：忽略 */
                }
                g_curw = NULL;
            }
        }

        if (idle)
            sleep(5);
    }
    return 0;
}
