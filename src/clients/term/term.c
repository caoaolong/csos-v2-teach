#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <input.h>
#include <comp_ipc.h>

/* term：compositor 的第一个正式客户端（终端）。
 *
 * 架构红线：只走 pipe 与 compositor 说话，永远不碰 /dev/kbd0、/dev/mouse0、
 * /dev/fb0（连 stdin fd 0 都不读——那是全局 kbd 环，读了会偷走 compositor 的事件）。
 * 渲染走 C→S 命令（compositor 光栅化），输入走 S→C 事件（窗口相对坐标尚未启用，
 * v1 窗口即全屏，坐标即屏坐标）。
 */

#define TERM_BG 0x000F0F12u
#define TERM_FG 0x00E8E8E8u
#define TERM_DIM_FG 0x00888888u

#define TERM_MAX_COLS 160
#define TERM_MAX_ROWS 64

static int g_cmdfd = -1; /* 写：渲染命令 */
static int g_evtfd = -1; /* 读：输入事件 */
static comp_reader_t g_evtr;

static uint32_t g_wx;
static uint32_t g_wy;
static uint32_t g_ww; /* 窗口像素宽 */
static uint32_t g_wh; /* 窗口像素高 */
static uint32_t g_cols;
static uint32_t g_rows; /* 含底行状态行 */
static uint32_t g_cx;
static uint32_t g_cy;
static uint32_t g_cw = COMP_FONT_W; /* 单元格像素宽 = 8*scale */
static uint32_t g_ch = COMP_FONT_H; /* 单元格像素高 = 16*scale */
static uint32_t g_ox;               /* 网格居中偏移 */
static uint32_t g_oy;

static char g_screen[TERM_MAX_ROWS][TERM_MAX_COLS];

/* 事件负载缓冲放 bss（用户栈仅一页） */
static uint8_t g_evt_payload[COMP_MAX_PAYLOAD];
static comp_hdr_t g_evth;

static int g_mx; /* 状态行用的鼠标位置 */
static int g_my;
static uint32_t g_mbuttons;

static int send_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint8_t p[20];

    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
    p[4] = (uint8_t)y;
    p[5] = (uint8_t)(y >> 8);
    p[6] = (uint8_t)(y >> 16);
    p[7] = (uint8_t)(y >> 24);
    p[8] = (uint8_t)w;
    p[9] = (uint8_t)(w >> 8);
    p[10] = (uint8_t)(w >> 16);
    p[11] = (uint8_t)(w >> 24);
    p[12] = (uint8_t)h;
    p[13] = (uint8_t)(h >> 8);
    p[14] = (uint8_t)(h >> 16);
    p[15] = (uint8_t)(h >> 24);
    p[16] = (uint8_t)color;
    p[17] = (uint8_t)(color >> 8);
    p[18] = (uint8_t)(color >> 16);
    p[19] = (uint8_t)(color >> 24);
    return (int)comp_send(g_cmdfd, COMP_MSG_FILL, p, sizeof(p));
}

static int send_text(uint32_t x, uint32_t y, uint32_t color,
                     const char *s, unsigned long n)
{
    uint8_t p[14 + TERM_MAX_COLS];
    unsigned long i;

    if (n > TERM_MAX_COLS)
        n = TERM_MAX_COLS;
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
    p[4] = (uint8_t)y;
    p[5] = (uint8_t)(y >> 8);
    p[6] = (uint8_t)(y >> 16);
    p[7] = (uint8_t)(y >> 24);
    p[8] = (uint8_t)color;
    p[9] = (uint8_t)(color >> 8);
    p[10] = (uint8_t)(color >> 16);
    p[11] = (uint8_t)(color >> 24);
    p[12] = (uint8_t)n;
    p[13] = (uint8_t)(n >> 8);
    for (i = 0; i < n; i++)
        p[14 + i] = (uint8_t)s[i];
    return (int)comp_send(g_cmdfd, COMP_MSG_TEXT, p, 14 + n);
}

/* 网格转窗口像素 */
static uint32_t cell_x(uint32_t cx)
{
    return g_ox + cx * g_cw;
}

static uint32_t cell_y(uint32_t cy)
{
    return g_oy + cy * g_ch;
}

static int draw_cell(uint32_t cx, uint32_t cy)
{
    if (cx >= g_cols || cy + 1 >= g_rows)
        return 0;
    return send_text(cell_x(cx), cell_y(cy), TERM_FG,
                     &g_screen[cy][cx], 1);
}

static int draw_row(uint32_t cy)
{
    if (cy + 1 >= g_rows)
        return 0;
    return send_text(cell_x(0), cell_y(cy), TERM_FG, g_screen[cy], g_cols);
}

static int draw_status(void)
{
    char buf[TERM_MAX_COLS];
    unsigned long n = 0;
    int v;
    char num[12];
    int ni;
    int i;

    if (g_rows == 0)
        return 0;
    /* 状态行：mouse <x>,<y> btns */
    buf[n++] = 'm';
    buf[n++] = 'o';
    buf[n++] = 'u';
    buf[n++] = 's';
    buf[n++] = 'e';
    buf[n++] = ' ';
    v = g_mx;
    if (v < 0)
    {
        buf[n++] = '-';
        v = -v;
    }
    ni = 0;
    if (v == 0)
        num[ni++] = '0';
    while (v > 0)
    {
        num[ni++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ni > 0)
        buf[n++] = num[--ni];
    buf[n++] = ',';
    v = g_my;
    if (v < 0)
    {
        buf[n++] = '-';
        v = -v;
    }
    ni = 0;
    if (v == 0)
        num[ni++] = '0';
    while (v > 0)
    {
        num[ni++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ni > 0)
        buf[n++] = num[--ni];
    buf[n++] = ' ';
    buf[n++] = 'b';
    buf[n++] = (char)('0' + (g_mbuttons & 7));
    while (n < g_cols)
        buf[n++] = ' ';
    if (send_fill(cell_x(0), cell_y(g_rows - 1), g_cols * g_cw,
                  g_ch, TERM_BG) != 0)
        return -1;
    for (i = 0; (unsigned long)i < n; i += TERM_MAX_COLS)
    {
        unsigned long m = n - (unsigned long)i;

        if (m > TERM_MAX_COLS)
            m = TERM_MAX_COLS;
        if (send_text(cell_x(0) + (uint32_t)((unsigned long)i * g_cw),
                      cell_y(g_rows - 1), TERM_DIM_FG,
                      buf + i, m) != 0)
            return -1;
    }
    return 0;
}

static int full_redraw(void)
{
    uint32_t cy;

    if (send_fill(0, 0, g_ww, g_wh, TERM_BG) != 0)
        return -1;
    for (cy = 0; cy + 1 < g_rows; cy++)
    {
        if (draw_row(cy) != 0)
            return -1;
    }
    return draw_status();
}

static void scroll_up(void)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y + 2 < g_rows; y++)
    {
        for (x = 0; x < g_cols; x++)
            g_screen[y][x] = g_screen[y + 1][x];
    }
    for (x = 0; x < g_cols; x++)
        g_screen[g_rows - 2][x] = ' ';
}

static int term_putc(char ch)
{
    if (ch == '\n')
    {
        g_cx = 0;
        g_cy++;
        if (g_cy + 1 >= g_rows)
        {
            g_cy = g_rows - 2;
            scroll_up();
            return full_redraw();
        }
        return 0;
    }
    if (ch == '\b')
    {
        if (g_cx > 0)
        {
            g_cx--;
            g_screen[g_cy][g_cx] = ' ';
            if (send_fill(cell_x(g_cx), cell_y(g_cy),
                          g_cw, g_ch, TERM_BG) != 0)
                return -1;
        }
        return 0;
    }
    if (ch == 0 || ch < 32)
        return 0;
    g_screen[g_cy][g_cx] = ch;
    if (draw_cell(g_cx, g_cy) != 0)
        return -1;
    g_cx++;
    if (g_cx >= g_cols)
    {
        g_cx = 0;
        g_cy++;
        if (g_cy + 1 >= g_rows)
        {
            g_cy = g_rows - 2;
            scroll_up();
            return full_redraw();
        }
    }
    return 0;
}

static int term_puts(const char *s)
{
    while (*s != '\0')
    {
        if (term_putc(*s++) != 0)
            return -1;
    }
    return 0;
}

/* 与 compositor 握手：开两端收 WIN */
/* 对端未就绪则重试等待；超时退出由 init 重拉 */
static int term_handshake(void)
{
    comp_hdr_t h;
    comp_win_t w;
    int tries;
    uint32_t scale;

    if (g_cmdfd >= 0)
        close(g_cmdfd);
    if (g_evtfd >= 0)
        close(g_evtfd);
    g_cmdfd = -1;
    g_evtfd = -1;
    for (tries = 0; tries < 600; tries++)
    {
        g_cmdfd = (int)open(COMP_CMD_FIFO, O_WRONLY);
        if (g_cmdfd >= 0)
            break;
        sleep(50);
    }
    if (g_cmdfd < 0)
    {
        printf("term: open cmd timeout\n");
        return -1;
    }
    for (tries = 0; tries < 600; tries++)
    {
        g_evtfd = (int)open(COMP_EVT_FIFO, O_RDONLY | O_NONBLOCK);
        if (g_evtfd >= 0)
            break;
        sleep(50);
    }
    if (g_evtfd < 0)
    {
        printf("term: open evt timeout\n");
        close(g_cmdfd);
        g_cmdfd = -1;
        return -1;
    }
    comp_reader_init(&g_evtr);
    /* WIN 是阻塞首帧 */
    if (comp_read_all(g_evtfd, &h, sizeof(h)) != 0)
        return -1;
    if (h.magic != COMP_IPC_MAGIC || h.type != COMP_MSG_WIN ||
        h.len != sizeof(w))
        return -1;
    if (comp_read_all(g_evtfd, &w, sizeof(w)) != 0)
        return -1;
    g_wx = w.x;
    g_wy = w.y;
    g_ww = w.w;
    g_wh = w.h;
    scale = w.scale < 1 ? 1 : w.scale;
    if (scale > 4)
        scale = 4;
    g_cw = COMP_FONT_W * scale;
    g_ch = COMP_FONT_H * scale;
    g_cols = w.w / g_cw;
    g_rows = w.h / g_ch;
    if (g_cols == 0 || g_rows < 2)
        return -1;
    if (g_cols > TERM_MAX_COLS)
        g_cols = TERM_MAX_COLS;
    if (g_rows > TERM_MAX_ROWS)
        g_rows = TERM_MAX_ROWS;
    /* 网格小于窗口时居中留边 */
    g_ox = (w.w > g_cols * g_cw) ? (w.w - g_cols * g_cw) / 2 : 0;
    g_oy = (w.h > g_rows * g_ch) ? (w.h - g_rows * g_ch) / 2 : 0;
    printf("term: win %ux%u scale %u grid %ux%u\n",
           w.w, w.h, scale, g_cols, g_rows);
    return 0;
}

static void term_reset(void)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y < TERM_MAX_ROWS; y++)
    {
        for (x = 0; x < TERM_MAX_COLS; x++)
            g_screen[y][x] = ' ';
    }
    g_cx = 0;
    g_cy = 0;
    g_mx = 0;
    g_my = 0;
    g_mbuttons = 0;
}

int main()
{
    int mdx = 0;
    int mdy = 0;
    int mhave = 0;

    printf("term: connecting compositor\n");
    if (term_handshake() != 0)
    {
        printf("term: handshake failed\n");
        exit(1);
    }
    term_reset();
    if (full_redraw() != 0)
        exit(1);
    if (term_puts("csos term v1 -- pipe client of compositor\n> ") != 0)
        exit(1);
    for (;;)
    {
        int idle = 1;
        int fr;
        long i;
        comp_hdr_t *h = &g_evth;
        uint8_t *payload = g_evt_payload;

        for (;;)
        {
            errno = 0;
            fr = comp_reader_feed(&g_evtr, g_evtfd, h, payload, sizeof(g_evt_payload));
            if (fr == 0)
                break;
            if (fr < 0)
                goto reconnect; /* compositor 挂了，重连 */
            idle = 0;
            if (h->type != COMP_MSG_EVTS)
                continue;
            {
                long nev = (long)(h->len / sizeof(input_event_t));
                input_event_t *evs = (input_event_t *)payload;

                for (i = 0; i < nev; i++)
                {
                    if (evs[i].type == INPUT_EV_KEY)
                    {
                        uint32_t code = evs[i].code;

                        if (code == INPUT_BTN_LEFT ||
                            code == INPUT_BTN_RIGHT ||
                            code == INPUT_BTN_MIDDLE)
                        {
                            uint32_t bit = code == INPUT_BTN_LEFT ? 1u
                                           : code == INPUT_BTN_RIGHT ? 2u : 4u;

                            if (evs[i].value)
                                g_mbuttons |= bit;
                            else
                                g_mbuttons &= ~bit;
                            continue;
                        }
                        if (evs[i].value != INPUT_KEY_PRESS)
                            continue;
                        if (term_putc(input_key_to_ascii((uint16_t)code)) != 0)
                            goto reconnect;
                    }
                    else if (evs[i].type == INPUT_EV_REL)
                    {
                        if (evs[i].code == INPUT_REL_X)
                        {
                            mdx += evs[i].value;
                            mhave = 1;
                        }
                        else if (evs[i].code == INPUT_REL_Y)
                        {
                            mdy += evs[i].value;
                            mhave = 1;
                        }
                    }
                    else if (evs[i].type == INPUT_EV_SYN && mhave)
                    {
                        g_mx += mdx;
                        g_my += mdy;
                        mdx = 0;
                        mdy = 0;
                        mhave = 0;
                        if (draw_status() != 0)
                            goto reconnect;
                    }
                }
            }
        }
        if (idle)
            sleep(5);
        continue;
    reconnect:
        printf("term: compositor lost, reconnecting\n");
        if (term_handshake() != 0)
        {
            printf("term: re-handshake failed\n");
            exit(1);
        }
        term_reset();
        mdx = 0;
        mdy = 0;
        mhave = 0;
        if (full_redraw() != 0)
            exit(1);
        if (term_puts("csos term v1 -- reconnected\n> ") != 0)
            exit(1);
    }
    return 0;
}
