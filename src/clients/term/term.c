#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <input.h>
#include <comp_ipc.h>

/* term：compositor 的第一个正式客户端（终端） */

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
    }
    for (x = 0; x < g_cols; x++)
        g_screen[g_rows - 2][x] = ' ';
}

static int term_putc(char ch)
{
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
    g_line_len = 0;
    g_cursor_on = 0;
    g_last_toggle = g_tick;
    g_shift = 0;
}

static int shell_prompt()
{
    /* cwd 提示符：拿不到则退化成裸 > */
    if (getcwd(g_cwd_buf, sizeof(g_cwd_buf)) == 0)
    {
        if (term_puts(g_cwd_buf) != 0)
            return -1;
    }
    return term_puts("> ");
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
    if (term_puts("[exit ") != 0)
        return -1;
    if (shell_put_int((long)st) != 0)
        return -1;
    if (term_puts("]\n") != 0)
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
        if (term_puts("builtins: help echo clear pid cd pwd exit\nrun: ls cat (/bin/…), abs path\n") != 0)
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
    if (ch == '\b')
    {
        if (g_line_len > 0)
        {
            g_line_len--;
            if (term_putc('\b') != 0)
                return -1;
        }
        return 0;
    }
    if (ch == '\n')
        return shell_run_line();
    if (ch == 0 || ch < 32)
        return 0;
    if (g_line_len >= SHELL_MAX_LINE)
        return 0; /* 行满丢弃 */
    g_line[g_line_len++] = ch;
    return term_putc(ch);
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
                    g_ox = (nw->w > g_cols * g_cw) ? (nw->w - g_cols * g_cw) / 2 : 0;
                    g_oy = (nw->h > g_rows * g_ch) ? (nw->h - g_rows * g_ch) / 2 : 0;
                    if (g_cx >= g_cols)
                        g_cx = g_cols - 1;
                    if (g_cy >= g_rows)
                        g_cy = g_rows - 2;
                    if (full_redraw() != 0)
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
                        if (evs[i].value != INPUT_KEY_PRESS)
                            continue;
                        if (shell_on_key(input_key_to_ascii_shift((uint16_t)code, g_shift)) != 0)
                            goto reconnect;
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
