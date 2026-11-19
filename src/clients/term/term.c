#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <input.h>
#include <comp_ipc.h>
#include <dirent.h>

/* term：compositor 的第一个正式客户端（终端） */

#define TERM_BG 0x000F0F12u
#define TERM_FG 0x00E8E8E8u
#define TERM_DIM_FG 0x00888888u

/* 网格 backing 上限（纯防爆 sanity 值，不参与布局）：
 * 网格 = 窗口尺寸 ÷ 字号，上限只兜住 Bochs 候选最大档以上；
 * 内容永远左上起排（现代终端铁律），留白只出现在右/下 */
#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 96

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
static uint32_t g_ox;               /* 恒 0：左上对齐，不居中 */
static uint32_t g_oy;

static char g_screen[TERM_MAX_ROWS][TERM_MAX_COLS];

/* 事件负载缓冲放 bss（用户栈仅一页） */
static uint8_t g_evt_payload[COMP_MAX_PAYLOAD];
static comp_hdr_t g_evth;

static int g_mx; /* 状态行用的鼠标位置 */
static int g_my;
static uint32_t g_mbuttons;
static uint32_t g_focused; /* compositor 告知的焦点状态 */

/* shell 行缓冲与外跑捕获缓冲放 bss（用户栈仅一页） */
#define SHELL_MAX_LINE 256
#define SHELL_MAX_ARGS 16
static char g_line[SHELL_MAX_LINE + 1];
static unsigned long g_line_len;
static char g_argbuf[SHELL_MAX_LINE + 1];
static char *g_argv[SHELL_MAX_ARGS + 1];
static uint8_t g_cap[256];

/* 文本光标（下划线式）：主循环空闲时按节拍闪烁 */
#define CURSOR_BLINK_TICKS 100 /* idle 循环约 5ms/次 -> 约 500ms */
static int g_cursor_on;
static unsigned long g_tick;
static unsigned long g_last_toggle;

/* shell 杂项缓冲放 bss（用户栈仅一页）：cwd 显示 + 裸命令的 /bin/ 拼接 */
static char g_cwd_buf[256];
static char g_exec_path[256 + 6];

/* Shift 电平：左右 Shift 任一按住即置位，term 自跟踪（compositor 只透传原始事件） */
static int g_shift;
/* Ctrl 电平：同上，用于 Ctrl+A/E/U 等行编辑组合键 */
static int g_ctrl;

/* 行编辑状态：行内光标 + 输入起始格 + 历史 */
static unsigned long g_cur;   /* 行内光标 0..g_line_len */
static uint32_t g_lsx;        /* 本行输入起始格（prompt 末尾） */
static uint32_t g_lsy;
static unsigned long g_vis_len; /* 当前已渲染的行字符数 */
#define HIST_N 32
static char g_hist[HIST_N][SHELL_MAX_LINE + 1];
static unsigned long g_hist_len[HIST_N];
static int g_hist_count;
static int g_hist_idx; /* -1=正在输的行，否则历史下标（0=最旧） */
static char g_draft[SHELL_MAX_LINE + 1];
static unsigned long g_draft_len;

/* TAB 补全缓冲放 bss（用户栈仅一页） */
static char g_comp_dir[256];
static csos_dirent_t g_comp_ents[64];

static void term_reset(void); /* 后定义，先声明：ANSI 清屏用 */

/* 最小 ANSI（SGR 前景色）：输出经 term_putc 过状态机，颜色逐格存入 g_attr */
static const uint32_t g_ansi_pal[8] = {
    0x00000000u, /* 30 黑 */
    0x00E05555u, /* 31 红 */
    0x0055DD55u, /* 32 绿 */
    0x00DDBB33u, /* 33 黄 */
    0x006666EEu, /* 34 蓝 */
    0x00CC66CCu, /* 35 品红 */
    0x0055CCCCu, /* 36 青 */
    0x00E8E8E8u, /* 37 白（=TERM_FG） */
};
static uint32_t g_attr[TERM_MAX_ROWS][TERM_MAX_COLS];
static uint32_t g_ansi_fg;
static int g_ansi_st; /* 0 普通 1 见ESC 2 CSI 参数中 */
static int g_ansi_params[8];
static int g_ansi_n;
static int g_last_exit; /* 上条外部命令 exit 码，圆点颜色据此而定 */

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
    return send_text(cell_x(cx), cell_y(cy), g_attr[cy][cx],
                     &g_screen[cy][cx], 1);
}

/* 同色连段合并为一次 TEXT：全默认色时与原来一样一行一次 */
static int draw_row(uint32_t cy)
{
    uint32_t x = 0;

    if (cy + 1 >= g_rows)
        return 0;
    while (x < g_cols)
    {
        uint32_t c = g_attr[cy][x];
        uint32_t n = 1;

        while (x + n < g_cols && g_attr[cy][x + n] == c)
            n++;
        if (send_text(cell_x(x), cell_y(cy), c,
                      &g_screen[cy][x], n) != 0)
            return -1;
        x += n;
    }
    return 0;
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

static int cursor_hide(void)
{
    if (!g_cursor_on)
        return 0;
    g_cursor_on = 0;
    return draw_cell(g_cx, g_cy);
}

static int cursor_show(void)
{
    uint32_t h;
    uint32_t y;

    if (g_cursor_on)
        return 0;
    if (g_cx >= g_cols || g_cy + 1 >= g_rows)
        return 0;
    h = g_cw >= 8 ? 2 * (g_cw / 8) : 2;
    if (h < 2)
        h = 2;
    if (h > g_ch)
        h = g_ch;
    y = cell_y(g_cy) + g_ch - h;
    if (send_fill(cell_x(g_cx), y, g_cw, h, TERM_FG) != 0)
        return -1;
    g_cursor_on = 1;
    return 0;
}

static int full_redraw(void)
{
    uint32_t cy;

    g_cursor_on = 0; /* 整屏重写，光标条已失效 */
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
        for (x = 0; x < TERM_MAX_COLS; x++)
            g_attr[y][x] = g_attr[y + 1][x];
    }
    for (x = 0; x < g_cols; x++)
        g_screen[g_rows - 2][x] = ' ';
    for (x = 0; x < TERM_MAX_COLS; x++)
        g_attr[g_rows - 2][x] = TERM_FG;
}

/* SGR 应用：0 复位，30-37 置前景；1 加粗等多余属性暂忽略 */
static void ansi_apply(void)
{
    int i;

    if (g_ansi_n == 0)
        g_ansi_fg = TERM_FG;
    for (i = 0; i <= g_ansi_n && i < 8; i++)
    {
        int p = g_ansi_params[i];

        if (p == 0)
            g_ansi_fg = TERM_FG;
        else if (p >= 30 && p <= 37)
            g_ansi_fg = g_ansi_pal[p - 30];
    }
}

/* ESC 序列消费：恒吞掉（不落屏不占光标），未知序列直接丢弃 */
static int ansi_csi_final(char ch);

static int ansi_feed(char ch)
{
    unsigned char c = (unsigned char)ch;

    if (g_ansi_st == 1)
    {
        if (c == '[')
        {
            g_ansi_st = 2;
            g_ansi_n = 0;
            g_ansi_params[0] = 0;
        }
        else
            g_ansi_st = 0;
        return 0;
    }
    if (c >= '0' && c <= '9')
    {
        if (g_ansi_n < 8)
            g_ansi_params[g_ansi_n] =
                g_ansi_params[g_ansi_n] * 10 + (int)(c - '0');
        return 0;
    }
    if (c == ';')
    {
        if (g_ansi_n < 7)
        {
            g_ansi_n++;
            g_ansi_params[g_ansi_n] = 0;
        }
        return 0;
    }
    if (c == 'm')
        ansi_apply();
    else if (ansi_csi_final(c) != 0)
    {
        g_ansi_st = 0;
        return -1;
    }
    g_ansi_st = 0;
    return 0;
}

/* CSI 收尾（SGR 之外）：2J 整屏清、H 定位、K 行清、A/B/C/D 移动 */
static int ansi_csi_final(char ch)
{
    unsigned char c = (unsigned char)ch;
    uint32_t y;
    uint32_t x;
    int p0;
    int k;

    if (c == 'J' && g_ansi_n == 0 && g_ansi_params[0] == 2)
    {
        /* 2J：整屏清 + home；行编辑状态当时必为空（仅外部程序能发出 ESC） */
        for (y = 0; y < TERM_MAX_ROWS; y++)
        {
            for (x = 0; x < TERM_MAX_COLS; x++)
            {
                g_screen[y][x] = ' ';
                g_attr[y][x] = TERM_FG;
            }
        }
        g_cx = 0;
        g_cy = 0;
        return full_redraw();
    }
    if (c == 'H')
    {
        int r = g_ansi_n >= 1 ? g_ansi_params[0] : 1;
        int col = g_ansi_n >= 2 ? g_ansi_params[1] : 1;

        if (r < 1)
            r = 1;
        if (col < 1)
            col = 1;
        if (cursor_hide() != 0)
            return -1;
        g_cy = (uint32_t)(r - 1);
        g_cx = (uint32_t)(col - 1);
        if (g_cy + 1 >= g_rows && g_rows >= 2)
            g_cy = g_rows - 2;
        if (g_cx >= g_cols && g_cols > 0)
            g_cx = g_cols - 1;
        return 0;
    }
    if (c == 'K')
    {
        /* EL：清自光标至行尾（含光标格） */
        if (g_cy + 1 >= g_rows)
            return 0;
        for (x = g_cx; x < g_cols; x++)
        {
            g_screen[g_cy][x] = ' ';
            g_attr[g_cy][x] = TERM_FG;
        }
        if (g_cx >= g_cols)
            return 0;
        return send_fill(cell_x(g_cx), cell_y(g_cy),
                         (g_cols - g_cx) * g_cw, g_ch, TERM_BG);
    }
    if (c == 'A' || c == 'B' || c == 'C' || c == 'D')
    {
        p0 = g_ansi_n == 0 ? 0 : g_ansi_params[0];
        k = p0 <= 0 ? 1 : p0;
        if (cursor_hide() != 0)
            return -1;
        if (c == 'A')
            g_cy = g_cy > (uint32_t)k ? g_cy - (uint32_t)k : 0;
        else if (c == 'B')
        {
            g_cy += (uint32_t)k;
            if (g_cy + 1 >= g_rows && g_rows >= 2)
                g_cy = g_rows - 2;
        }
        else if (c == 'C')
        {
            g_cx += (uint32_t)k;
            if (g_cx >= g_cols && g_cols > 0)
                g_cx = g_cols - 1;
        }
        else
            g_cx = g_cx > (uint32_t)k ? g_cx - (uint32_t)k : 0;
        return 0;
    }
    return 0;
}

static int term_putc(char ch)
{
    if (g_ansi_st != 0)
        return ansi_feed(ch);
    if (ch == 0x1B)
    {
        g_ansi_st = 1;
        return 0;
    }
    if (g_cursor_on)
    {
        g_cursor_on = 0;
        if (draw_cell(g_cx, g_cy) != 0)
            return -1;
    }
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
            g_attr[g_cy][g_cx] = TERM_FG;
            if (send_fill(cell_x(g_cx), cell_y(g_cy),
                          g_cw, g_ch, TERM_BG) != 0)
                return -1;
        }
        return 0;
    }
    if (ch == 0 || ch < 32)
        return 0;
    g_screen[g_cy][g_cx] = ch;
    g_attr[g_cy][g_cx] = g_ansi_fg;
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

/* 建窗请求：0,0=最大化；name 进标题栏 */
static int send_create(uint32_t w, uint32_t h, const char *name)
{
    uint8_t p[8 + 32];
    unsigned long n = 0;

    p[0] = (uint8_t)w;
    p[1] = (uint8_t)(w >> 8);
    p[2] = (uint8_t)(w >> 16);
    p[3] = (uint8_t)(w >> 24);
    p[4] = (uint8_t)h;
    p[5] = (uint8_t)(h >> 8);
    p[6] = (uint8_t)(h >> 16);
    p[7] = (uint8_t)(h >> 24);
    while (n < 31 && name[n] != '\0')
    {
        p[8 + n] = (uint8_t)name[n];
        n++;
    }
    p[8 + n] = 0;
    return (int)comp_send(g_cmdfd, COMP_MSG_CREATE, p, 8 + n + 1);
}

/* 与 compositor 握手：开两端，发 CREATE，等 WIN_GEOM */
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
    /* 先发 CREATE，再阻塞等 WIN_GEOM 首帧 */
    if (send_create(0, 0, "term") != 0)
        return -1;
    if (comp_read_all(g_evtfd, &h, sizeof(h)) != 0)
        return -1;
    if (h.magic != COMP_IPC_MAGIC || h.type != COMP_MSG_WIN_GEOM ||
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
    /* 左上锚定：网格从 (0,0) 起排，多余像素只留右/下（现代终端铁律） */
    g_ox = 0;
    g_oy = 0;
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
        {
            g_screen[y][x] = ' ';
            g_attr[y][x] = TERM_FG;
        }
    }
    g_cx = 0;
    g_cy = 0;
    g_mx = 0;
    g_my = 0;
    g_mbuttons = 0;
    g_line_len = 0;
    g_cur = 0;
    g_vis_len = 0;
    g_lsx = 0;
    g_lsy = 0;
    g_hist_idx = -1;
    g_cursor_on = 0;
    g_last_toggle = g_tick;
    g_shift = 0;
    g_ctrl = 0;
    g_ansi_st = 0;
    g_ansi_fg = TERM_FG;
}

static int shell_prompt()
{
    /* 最左侧圆点：上条外部命令 exit 0 绿、非零红（内建命令不改）；字体只到
     * ASCII，用字母 o 充当圆点，颜色即语义 */
    if (term_puts(g_last_exit == 0 ? "\x1b[32mo\x1b[0m "
                                   : "\x1b[31mo\x1b[0m ") != 0)
        return -1;
    /* cwd 提示符：拿不到则退化成裸 > */
    if (getcwd(g_cwd_buf, sizeof(g_cwd_buf)) == 0)
    {
        if (term_puts(g_cwd_buf) != 0)
            return -1;
    }
    if (term_puts("> ") != 0)
        return -1;
    /* 输入起始锚点：其后所有行编辑重画都以此为基准 */
    g_lsx = g_cx;
    g_lsy = g_cy;
    g_cur = 0;
    g_vis_len = 0;
    return 0;
}

/* 锚点有效性：行首须在内容区内（resize/滚动后可能失效） */
static int shell_redraw(void);

static int shell_anchor_ok(void)
{
    if (g_cols == 0 || g_rows < 2)
        return 0;
    if (g_lsy + 1 >= g_rows || g_lsx >= g_cols)
        return 0;
    return 1;
}

/* 显示光标直达锚点后 n 格（纯变量移动，不发任何渲染） */
static void disp_goto(unsigned long n)
{
    unsigned long t;

    if (g_cols == 0)
        return;
    t = (unsigned long)g_lsx + n;
    g_cy = g_lsy + (uint32_t)(t / g_cols);
    g_cx = (uint32_t)(t % g_cols);
    if (g_cy + 1 >= g_rows && g_rows >= 2)
    {
        g_cy = g_rows - 2;
        g_cx = g_cols - 1;
    }
}

/* 无重画定位（左右键/Ctrl+A/E）：先藏旧处下划线，blink 循环会重显 */
static int shell_place(void)
{
    if (!shell_anchor_ok())
        return shell_redraw();
    if (cursor_hide() != 0)
        return -1;
    disp_goto(g_cur);
    return 0;
}

/* 从锚点起重画整行并落到 g_cur；行首到显示光标的不变量由调用方保证 */
static int shell_redraw(void)
{
    unsigned long i;
    unsigned long old;

    /* 先擦旧处下划线：其后 disp_goto 会搬走显示光标，
     * 若靠重发首字符时的 term_putc 来藏，位置已错，残线越攒越长 */
    if (cursor_hide() != 0)
        return -1;
    if (!shell_anchor_ok())
    {
        /* 锚点已坏（极少）：另起一行重建 */
        if (term_putc('\n') != 0)
            return -1;
        g_lsx = g_cx;
        g_lsy = g_cy;
        g_vis_len = 0;
    }
    else
        disp_goto(0);
    old = g_vis_len;
    for (i = 0; i < g_line_len; i++)
    {
        if (term_putc(g_line[i]) != 0)
            return -1;
    }
    /* 新行变短：空格垫掉残留尾巴 */
    while (old > g_line_len)
    {
        if (term_putc(' ') != 0)
            return -1;
        old--;
    }
    disp_goto(g_cur);
    g_vis_len = g_line_len;
    return 0;
}

/* prompt 后原样重打当前行（补全列目录用，此时显示光标==锚点） */
static int shell_retype(void)
{
    unsigned long i;
    unsigned long save = g_cur;

    if (cursor_hide() != 0)
        return -1;
    for (i = 0; i < g_line_len; i++)
    {
        if (term_putc(g_line[i]) != 0)
            return -1;
    }
    g_cur = g_line_len;
    disp_goto(save);
    g_cur = save;
    g_vis_len = g_line_len;
    return 0;
}

/* 回车时收录历史：空行跳过，与最新一条重复不收 */
static void hist_push(void)
{
    int k;
    unsigned long i;

    if (g_line_len == 0)
        return;
    if (g_hist_count > 0)
    {
        int newest = g_hist_count - 1;

        if (g_hist_len[newest] == g_line_len &&
            strncmp(g_hist[newest], g_line, g_line_len) == 0)
            return;
    }
    if (g_hist_count < HIST_N)
        k = g_hist_count++;
    else
    {
        for (k = 0; k < HIST_N - 1; k++)
        {
            for (i = 0; i <= g_hist_len[k + 1]; i++)
                g_hist[k][i] = g_hist[k + 1][i];
            g_hist_len[k] = g_hist_len[k + 1];
        }
        k = HIST_N - 1;
    }
    for (i = 0; i < g_line_len; i++)
        g_hist[k][i] = g_line[i];
    g_hist[k][g_line_len] = '\0';
    g_hist_len[k] = g_line_len;
}

static int hist_load(int idx)
{
    unsigned long i;

    for (i = 0; i < g_hist_len[idx]; i++)
        g_line[i] = g_hist[idx][i];
    g_line_len = g_hist_len[idx];
    g_cur = g_line_len;
    return shell_redraw();
}

/* dir<0 上翻 dir>0 下翻；下翻越过最新恢复打草稿 */
static int hist_nav(int dir)
{
    unsigned long i;

    if (g_hist_count == 0)
        return 0;
    if (dir < 0)
    {
        if (g_hist_idx == -1)
        {
            for (i = 0; i < g_line_len; i++)
                g_draft[i] = g_line[i];
            g_draft_len = g_line_len;
            g_hist_idx = g_hist_count - 1;
        }
        else if (g_hist_idx > 0)
            g_hist_idx--;
        else
            return 0;
        return hist_load(g_hist_idx);
    }
    if (g_hist_idx == -1)
        return 0;
    if (g_hist_idx == g_hist_count - 1)
    {
        for (i = 0; i < g_draft_len; i++)
            g_line[i] = g_draft[i];
        g_line_len = g_draft_len;
        g_cur = g_line_len;
        g_hist_idx = -1;
        return shell_redraw();
    }
    g_hist_idx++;
    return hist_load(g_hist_idx);
}

/* 在行内光标处插入串（+可选尾随字符），历史脱钩 */
static int shell_insert(const char *s, unsigned long slen, char trail)
{
    unsigned long need = slen + (trail ? 1 : 0);
    unsigned long i;

    if (slen == 0 && !trail)
        return 0;
    if (g_line_len + need > SHELL_MAX_LINE)
        return 0;
    for (i = g_line_len; i > g_cur; i--)
        g_line[i + need - 1] = g_line[i - 1];
    for (i = 0; i < slen; i++)
        g_line[g_cur + i] = s[i];
    if (trail)
        g_line[g_cur + slen] = trail;
    g_line_len += need;
    g_cur += need;
    g_hist_idx = -1;
    return shell_redraw();
}

/* TAB 补全：当前词按最后 '/' 切目录/前缀，getdents 匹配 */
static int shell_complete(void)
{
    unsigned long tok = g_cur;
    unsigned long i;
    unsigned long sl = 0;
    int has_slash = 0;
    const char *dir;
    unsigned long pre_off;
    unsigned long pre_len;
    unsigned long dl;
    long n;
    long m;
    int nmatch = 0;
    unsigned long match0 = 0;
    unsigned long com_end = 0;

    while (tok > 0 && g_line[tok - 1] != ' ' && g_line[tok - 1] != '\t')
        tok--;
    for (i = tok; i < g_cur; i++)
    {
        if (g_line[i] == '/')
        {
            sl = i;
            has_slash = 1;
        }
    }
    if (has_slash)
    {
        dl = sl - tok + 1;
        if (dl >= sizeof(g_comp_dir))
            return 0;
        for (i = 0; i < dl; i++)
            g_comp_dir[i] = g_line[tok + i];
        g_comp_dir[dl] = '\0';
        dir = g_comp_dir;
        pre_off = sl + 1;
        pre_len = g_cur - pre_off;
    }
    else
    {
        dir = ".";
        pre_off = tok;
        pre_len = g_cur - tok;
    }
    n = getdents(dir, g_comp_ents, 64);
    if (n <= 0)
        return 0;
    for (m = 0; m < n; m++)
    {
        for (i = 0; i < pre_len; i++)
        {
            if (g_comp_ents[m].name[i] != g_line[pre_off + i])
                break;
        }
        if (i != pre_len)
            continue;
        if (g_comp_ents[m].name[pre_len] == '\0')
            continue; /* 已完整，跳过 */
        if (nmatch == 0)
        {
            match0 = (unsigned long)m;
            com_end = strlen(g_comp_ents[m].name);
        }
        else
        {
            unsigned long k = pre_len;

            while (k < com_end &&
                   g_comp_ents[match0].name[k] == g_comp_ents[m].name[k] &&
                   g_comp_ents[match0].name[k] != '\0')
                k++;
            com_end = k;
        }
        nmatch++;
    }
    if (nmatch == 0)
        return 0;
    if (com_end > pre_len)
    {
        /* 公共前缀推进；唯一匹配再缀 '/'（目录）或 ' '（文件） */
        char trail = 0;

        if (nmatch == 1)
            trail = g_comp_ents[match0].is_dir ? '/' : ' ';
        return shell_insert(g_comp_ents[match0].name + pre_len,
                            com_end - pre_len, trail);
    }
    /* 多个且无公共延长：列出候选后重打当前行 */
    if (term_putc('\n') != 0)
        return -1;
    for (m = 0; m < n; m++)
    {
        for (i = 0; i < pre_len; i++)
        {
            if (g_comp_ents[m].name[i] != g_line[pre_off + i])
                break;
        }
        if (i != pre_len || g_comp_ents[m].name[pre_len] == '\0')
            continue;
        if (term_puts(g_comp_ents[m].name) != 0)
            return -1;
        if (g_comp_ents[m].is_dir && term_putc('/') != 0)
            return -1;
        if (term_puts("  ") != 0)
            return -1;
    }
    if (term_putc('\n') != 0)
        return -1;
    if (shell_prompt() != 0)
        return -1;
    return shell_retype();
}

static int shell_put_int(long v)
{
    char buf[24];
    char rev[24];
    int n = 0;
    int neg = 0;
    int i;

    if (v < 0)
    {
        neg = 1;
        v = -v;
    }
    if (v == 0)
        rev[n++] = '0';
    while (v > 0 && n < 24)
    {
        rev[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    if (neg && n < 24)
        rev[n++] = '-';
    for (i = 0; i < n; i++)
        buf[i] = rev[n - 1 - i];
    buf[n] = '\0';
    return term_puts(buf);
}

/* 行切分为 argv（只按空格/制表，无引号）；返回 argc */
static int shell_parse()
{
    int argc = 0;
    unsigned long i = 0;

    while (i < g_line_len)
    {
        while (i < g_line_len &&
               (g_line[i] == ' ' || g_line[i] == '\t'))
            i++;
        if (i >= g_line_len)
            break;
        if (argc >= SHELL_MAX_ARGS)
            break;
        g_argv[argc++] = g_argbuf + i;
        while (i < g_line_len &&
               g_line[i] != ' ' && g_line[i] != '\t')
        {
            g_argbuf[i] = g_line[i];
            i++;
        }
        g_argbuf[i] = '\0';
        i++;
    }
    g_argv[argc] = (char *)0;
    return argc;
}

/* 外跑程序：stdout/stderr 进管道回显，EOF 后报 exit 码 */
static int shell_run_external(int argc)
{
    int fds[2];
    long pid;
    long n;
    unsigned long k;
    int st = 0;
    const char *path = g_argv[0];

    (void)argc;
    if (path[0] != '/')
    {
        /* 裸命令名默认去 /bin 找（cd/pwd 之外的小工具都在那） */
        unsigned long i = 0;

        g_exec_path[0] = '/';
        g_exec_path[1] = 'b';
        g_exec_path[2] = 'i';
        g_exec_path[3] = 'n';
        g_exec_path[4] = '/';
        while (path[i] != '\0' && i + 5 < sizeof(g_exec_path) - 1)
        {
            g_exec_path[5 + i] = path[i];
            i++;
        }
        g_exec_path[5 + i] = '\0';
        if (path[i] != '\0')
        {
            if (term_puts("command too long\n") != 0)
                return -1;
            return shell_prompt();
        }
        path = g_exec_path;
    }
    if (pipe(fds) != 0)
    {
        if (term_puts("pipe failed\n") != 0)
            return -1;
        return shell_prompt();
    }
    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        if (term_puts("fork failed\n") != 0)
            return -1;
        return shell_prompt();
    }
    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
            close(fds[1]);
        execve(path, g_argv, (char *const *)0);
        write(STDOUT_FILENO, "exec failed\n", 13);
        exit(1);
    }
    close(fds[1]);
    for (;;)
    {
        n = read(fds[0], g_cap, sizeof(g_cap));
        if (n == 0)
            break; /* 写端全关 */
        if (n < 0)
            break; /* pipe 无读端之外的错误直接收尾 */
        for (k = 0; k < (unsigned long)n; k++)
        {
            if (term_putc((char)g_cap[k]) != 0)
            {
                close(fds[0]);
                wait(&st);
                return -1;
            }
        }
    }
    close(fds[0]);
    wait(&st);
    g_last_exit = st;
    if (term_putc('\n') != 0)
        return -1;
    return shell_prompt();
}

/* 执行一行：内建优先，否则外跑；返回 0 正常，-1 需重连 */
static int shell_run_line()
{
    int argc;

    if (term_putc('\n') != 0)
        return -1;
    if (g_line_len == 0)
        return shell_prompt();
    g_line[g_line_len] = '\0';
    hist_push();
    g_hist_idx = -1;
    {
        unsigned long i;

        for (i = 0; i <= g_line_len; i++)
            g_argbuf[i] = g_line[i];
    }
    argc = shell_parse();
    g_line_len = 0;
    if (argc <= 0)
        return shell_prompt();
    if (strcmp(g_argv[0], "help") == 0)
    {
        if (term_puts("builtins: help echo clear pid cd pwd exit\nrun: ls cat (/bin/…), abs path\nkeys: up/down hist, left/right, ctrl+a/e/u, tab complete\n") != 0)
            return -1;
        return shell_prompt();
    }
    if (strcmp(g_argv[0], "echo") == 0)
    {
        int i;

        for (i = 1; i < argc; i++)
        {
            if (i > 1 && term_putc(' ') != 0)
                return -1;
            if (term_puts(g_argv[i]) != 0)
                return -1;
        }
        if (term_putc('\n') != 0)
            return -1;
        return shell_prompt();
    }
    if (strcmp(g_argv[0], "clear") == 0)
    {
        term_reset();
        if (full_redraw() != 0)
            return -1;
        return shell_prompt();
    }
    if (strcmp(g_argv[0], "pid") == 0)
    {
        if (shell_put_int(getpid()) != 0)
            return -1;
        if (term_putc('\n') != 0)
            return -1;
        return shell_prompt();
    }
    if (strcmp(g_argv[0], "pwd") == 0)
    {
        if (getcwd(g_cwd_buf, sizeof(g_cwd_buf)) != 0)
        {
            if (term_puts("pwd failed\n") != 0)
                return -1;
            return shell_prompt();
        }
        if (term_puts(g_cwd_buf) != 0)
            return -1;
        if (term_putc('\n') != 0)
            return -1;
        return shell_prompt();
    }
    /* cd 只能是内建：fork 出的子进程改不了 shell 的目录 */
    if (strcmp(g_argv[0], "cd") == 0)
    {
        const char *dst = argc > 1 ? g_argv[1] : "/";

        if (chdir(dst) != 0)
        {
            if (errno == ENOENT)
            {
                if (term_puts("cd: no such dir\n") != 0)
                    return -1;
            }
            else if (errno == ENOTDIR)
            {
                if (term_puts("cd: not a dir\n") != 0)
                    return -1;
            }
            else
            {
                if (term_puts("cd failed\n") != 0)
                    return -1;
            }
            return shell_prompt();
        }
        return shell_prompt();
    }
    if (strcmp(g_argv[0], "exit") == 0)
    {
        comp_send(g_cmdfd, COMP_MSG_DESTROY, NULL, 0);
        exit(0);
    }
    return shell_run_external(argc);
}

/* 键入一字：回显 + 行缓冲，回车执行；返回 0 正常，-1 需重连 */
static int shell_on_key(char ch)
{
    unsigned long i;

    if (ch == '\n')
        return shell_run_line();
    if (ch == '\b')
    {
        if (g_cur == 0)
            return 0;
        for (i = g_cur; i < g_line_len; i++)
            g_line[i - 1] = g_line[i];
        g_line_len--;
        g_cur--;
        g_hist_idx = -1;
        return shell_redraw();
    }
    if (ch == 0x01) /* Ctrl+A 行首 */
    {
        g_cur = 0;
        return shell_place();
    }
    if (ch == 0x05) /* Ctrl+E 行尾 */
    {
        g_cur = g_line_len;
        return shell_place();
    }
    if (ch == 0x15) /* Ctrl+U 整行清 */
    {
        g_line_len = 0;
        g_cur = 0;
        g_hist_idx = -1;
        return shell_redraw();
    }
    if (ch == 0 || ch < 32)
        return 0; /* Ctrl+C/D 等暂留空（kill/signal 后续做） */
    if (g_line_len >= SHELL_MAX_LINE)
        return 0; /* 行满丢弃 */
    for (i = g_line_len; i > g_cur; i--)
        g_line[i] = g_line[i - 1];
    g_line[g_cur] = ch;
    g_line_len++;
    g_cur++;
    g_hist_idx = -1;
    return shell_redraw();
}

int main()
{
    printf("term: connecting compositor\n");
    if (term_handshake() != 0)
    {
        printf("term: handshake failed\n");
        exit(1);
    }
    term_reset();
    if (full_redraw() != 0)
        exit(1);
    if (term_puts("csos term v2 -- minimal shell\n") != 0)
        exit(1);
    if (shell_prompt() != 0)
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
            if (h->type == COMP_MSG_FOCUS)
            {
                /* 单窗口恒聚焦；记下状态备用 */
                if (h->len >= 4)
                    g_focused = payload[0] | ((uint32_t)payload[1] << 8) |
                                ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
                continue;
            }
            if (h->type == COMP_MSG_EXPOSE)
            {
                if (full_redraw() != 0)
                    goto reconnect;
                continue;
            }
            if (h->type == COMP_MSG_WIN_GEOM)
            {
                /* 窗口被移动/改变：重读几何并全量重画 */
                if (h->len == sizeof(comp_win_t))
                {
                    comp_win_t *nw = (comp_win_t *)payload;
                    uint32_t sc = nw->scale < 1 ? 1 : nw->scale;
                    uint32_t ocols = g_cols;
                    uint32_t orows = g_rows;

                    if (sc > 4)
                        sc = 4;
                    g_cw = COMP_FONT_W * sc;
                    g_ch = COMP_FONT_H * sc;
                    g_cols = nw->w / g_cw;
                    g_rows = nw->h / g_ch;
                    if (g_cols > TERM_MAX_COLS)
                        g_cols = TERM_MAX_COLS;
                    if (g_rows > TERM_MAX_ROWS)
                        g_rows = TERM_MAX_ROWS;
                    if (g_cols == 0 || g_rows < 2)
                        goto reconnect;
                    /* 左上锚定：网格从 (0,0) 起排，多余像素只留右/下 */
                    g_ox = 0;
                    g_oy = 0;
                    if (g_cx >= g_cols)
                        g_cx = g_cols - 1;
                    if (g_cy >= g_rows)
                        g_cy = g_rows - 2;
                    if (g_cols != ocols || g_rows != orows)
                    {
                        /* 网格尺寸变了：半截输入行作废，重画后给新提示 */
                        g_line_len = 0;
                        if (full_redraw() != 0)
                            goto reconnect;
                        if (shell_prompt() != 0)
                            goto reconnect;
                    }
                    else if (full_redraw() != 0)
                        goto reconnect;
                }
                continue;
            }
            if (h->type == COMP_MSG_CURSOR)
            {
                /* 服务端绝对光标（屏坐标，已钳制）：直接同步 */
                if (h->len >= 8)
                {
                    g_mx = (int)((uint32_t)payload[0] |
                                 ((uint32_t)payload[1] << 8) |
                                 ((uint32_t)payload[2] << 16) |
                                 ((uint32_t)payload[3] << 24));
                    g_my = (int)((uint32_t)payload[4] |
                                 ((uint32_t)payload[5] << 8) |
                                 ((uint32_t)payload[6] << 16) |
                                 ((uint32_t)payload[7] << 24));
                    if (draw_status() != 0)
                        goto reconnect;
                }
                continue;
            }
            if (h->type == COMP_MSG_CLOSE)
            {
                /* 服务端请求关闭：回 DESTROY 后正常退出（init 见 0 不重启） */
                comp_send(g_cmdfd, COMP_MSG_DESTROY, NULL, 0);
                exit(0);
            }
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
                            uint32_t bit = code == INPUT_BTN_LEFT    ? 1u
                                           : code == INPUT_BTN_RIGHT ? 2u
                                                                     : 4u;

                            if (evs[i].value)
                                g_mbuttons |= bit;
                            else
                                g_mbuttons &= ~bit;
                            continue;
                        }
                        if (code == INPUT_KEY_LEFTSHIFT ||
                            code == INPUT_KEY_RIGHTSHIFT)
                        {
                            g_shift = evs[i].value ? 1 : 0;
                            continue;
                        }
                        if (code == INPUT_KEY_LEFTCTRL)
                        {
                            g_ctrl = evs[i].value ? 1 : 0;
                            continue;
                        }
                        if (evs[i].value != INPUT_KEY_PRESS)
                            continue;
                        if (code == INPUT_KEY_UP)
                        {
                            if (hist_nav(-1) != 0)
                                goto reconnect;
                            continue;
                        }
                        if (code == INPUT_KEY_DOWN)
                        {
                            if (hist_nav(1) != 0)
                                goto reconnect;
                            continue;
                        }
                        if (code == INPUT_KEY_LEFT)
                        {
                            if (g_cur > 0)
                            {
                                g_cur--;
                                if (shell_place() != 0)
                                    goto reconnect;
                            }
                            continue;
                        }
                        if (code == INPUT_KEY_RIGHT)
                        {
                            if (g_cur < g_line_len)
                            {
                                g_cur++;
                                if (shell_place() != 0)
                                    goto reconnect;
                            }
                            continue;
                        }
                        if (code == INPUT_KEY_TAB)
                        {
                            if (shell_complete() != 0)
                                goto reconnect;
                            continue;
                        }
                        {
                            char ch = input_key_to_ascii_shift((uint16_t)code,
                                                               g_shift);

                            if (g_ctrl)
                            {
                                /* Ctrl+字母 → 1..26，其余忽略 */
                                if (ch >= 'a' && ch <= 'z')
                                    ch = (char)(ch - 'a' + 1);
                                else if (ch >= 'A' && ch <= 'Z')
                                    ch = (char)(ch - 'A' + 1);
                                else
                                    continue;
                            }
                            if (shell_on_key(ch) != 0)
                                goto reconnect;
                        }
                    }
                    else if (evs[i].type == INPUT_EV_REL ||
                             evs[i].type == INPUT_EV_SYN)
                    {
                        continue; /* 光标位置由 CURSOR 消息同步，相对位移忽略 */
                    }
                }
            }
        }
        g_tick++;
        if (!idle)
        {
            /* 有输入：保持实心可见，重启闪烁周期 */
            if (!g_cursor_on && cursor_show() != 0)
                goto reconnect;
            g_last_toggle = g_tick;
        }
        else if (g_tick - g_last_toggle >= CURSOR_BLINK_TICKS)
        {
            int r = g_cursor_on ? cursor_hide() : cursor_show();

            if (r != 0)
                goto reconnect;
            g_last_toggle = g_tick;
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
        if (full_redraw() != 0)
            exit(1);
        if (term_puts("csos term v2 -- reconnected\n") != 0)
            exit(1);
        if (shell_prompt() != 0)
            exit(1);
    }
    return 0;
}
