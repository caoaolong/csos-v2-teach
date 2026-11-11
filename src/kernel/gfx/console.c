#include <kernel/gfx/console.h>
#include <kernel/gfx/fb.h>
#include <kernel/gfx/font.h>
#include <kernel/gfx/logo.h>
#include <kernel/gfx/layout.h>
#include <kernel/memory/heap.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>

#define CON_FG_RGB 0xE8E8E8U
#define CON_TAB_STOP 8
#define CON_MIN_ROWS 3
#define CON_MIN_COLS 20
#define CON_CURSOR_BLINK_MS 500u

static uint32_t *s_fb;    /* 帧缓冲基址（32 位像素数组） */
static uint32_t s_stride; /* 每扫描行像素数（含行尾填充） */
static uint32_t s_width;  /* 屏幕宽（像素） */
static uint32_t s_height; /* 屏幕高（像素） */
static uint32_t s_format; /* 像素格式（RGB/BGR，见 FB_PIXEL_*） */
static uint32_t s_fg;     /* 前景色（已按 format 打包） */
static uint32_t s_bg;     /* 背景色（清屏/滚屏补行用） */
static uint32_t s_cols;   /* 文本列数（= 宽 / 8） */
static uint32_t s_rows;   /* 文本行数（= 文本区高 / 16） */
static uint32_t s_top;    /* 文本区首像素行 */
static uint32_t s_cx;     /* 光标列 */
static uint32_t s_cy;     /* 光标行 */
static int s_ready;       /* 初始化完成标志（1 才可输出） */
static spinlock_t s_lock = SPINLOCK_INIT;

static char *s_text;
static uint32_t s_cap_cols;
static uint32_t s_cap_rows;

/* 块光标状态：phase=1 表示当前已反白画出；next_ms 为下次翻转时刻 */
static int s_cursor_phase;
static uint64_t s_cursor_next_ms;
static int s_cursor_enabled;

/* 重叠安全的像素搬运：cfb_copyarea 的极简版，按 8 字节分块 */
static void fb_memmove_pixels(uint32_t *dst, const uint32_t *src, uint32_t n)
{
    uint64_t *d64;
    const uint64_t *s64;

    if (n == 0 || dst == src)
        return;

    if (dst < src)
    {
        /* 前向：dst 在 src 之前，从头拷安全 */
        d64 = (uint64_t *)dst;
        s64 = (const uint64_t *)src;
        while (n >= 2 &&
               (((uintptr_t)d64 & 7) == 0) &&
               (((uintptr_t)s64 & 7) == 0))
        {
            *d64++ = *s64++;
            n -= 2;
        }
        dst = (uint32_t *)d64;
        src = (const uint32_t *)s64;
        while (n--)
            *dst++ = *src++;
    }
    else
    {
        /* 后向：dst 在 src 之后，从尾拷避免覆盖源 */
        dst += n;
        src += n;
        d64 = (uint64_t *)dst;
        s64 = (const uint64_t *)src;
        while (n >= 2 &&
               (((uintptr_t)d64 & 7) == 0) &&
               (((uintptr_t)s64 & 7) == 0))
        {
            *--d64 = *--s64;
            n -= 2;
        }
        dst = (uint32_t *)d64;
        src = (const uint32_t *)s64;
        while (n--)
            *--dst = *--src;
    }
}

/*
 * fb_copyarea：矩形像素块搬运（cfb_copyarea 对应物）。
 * 坐标均为帧缓冲绝对像素；按垂直方向选拷贝顺序，行内用 memmove。
 */
static void fb_copyarea_locked(uint32_t sx, uint32_t sy,
                               uint32_t w, uint32_t h,
                               uint32_t dx, uint32_t dy)
{
    uint32_t i;

    if (w == 0 || h == 0 || s_fb == NULL)
        return;
    if (sx + w > s_width)
        w = (sx < s_width) ? s_width - sx : 0;
    if (dx + w > s_width)
        w = (dx < s_width) ? s_width - dx : 0;
    if (sy + h > s_height)
        h = (sy < s_height) ? s_height - sy : 0;
    if (dy + h > s_height)
        h = (dy < s_height) ? s_height - dy : 0;
    if (w == 0 || h == 0)
        return;

    if (dy <= sy)
    {
        for (i = 0; i < h; i++)
            fb_memmove_pixels(&s_fb[(dy + i) * s_stride + dx],
                              &s_fb[(sy + i) * s_stride + sx], w);
    }
    else
    {
        for (i = h; i > 0; i--)
            fb_memmove_pixels(&s_fb[(dy + i - 1) * s_stride + dx],
                              &s_fb[(sy + i - 1) * s_stride + sx], w);
    }
}

/* fb_fillrect：矩形涂色（cfb_fillrect 对应物），按行以 64 位 pattern 填充 */
static void fb_fillrect_locked(uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h, uint32_t pixel)
{
    uint32_t i;
    uint64_t pat;

    if (w == 0 || h == 0 || s_fb == NULL)
        return;
    if (x >= s_width || y >= s_height)
        return;
    if (x + w > s_width)
        w = s_width - x;
    if (y + h > s_height)
        h = s_height - y;

    pat = ((uint64_t)pixel << 32) | pixel;
    for (i = 0; i < h; i++)
    {
        uint32_t *row = &s_fb[(y + i) * s_stride + x];
        uint32_t n = w;

        while (n >= 2 && (((uintptr_t)row & 7) == 0))
        {
            *(uint64_t *)row = pat;
            row += 2;
            n -= 2;
        }
        while (n--)
            *row++ = pixel;
    }
}

static void con_text_clear_locked(char ch)
{
    uint32_t n;
    uint32_t i;

    if (s_text == NULL)
        return;
    n = s_cap_cols * s_cap_rows;
    for (i = 0; i < n; i++)
        s_text[i] = ch;
}

/* 按当前 cols/rows 重建字符缓冲；kmalloc 失败则退化为无缓冲（仅像素面工作） */
static void con_text_alloc_locked(uint32_t cols, uint32_t rows)
{
    char *p;
    uint64_t n;

    if (cols == 0 || rows == 0)
        return;
    if (s_text != NULL && cols == s_cap_cols && rows == s_cap_rows)
    {
        con_text_clear_locked(' ');
        return;
    }
    if (s_text != NULL)
    {
        kfree(s_text);
        s_text = NULL;
    }
    s_cap_cols = 0;
    s_cap_rows = 0;

    n = (uint64_t)cols * (uint64_t)rows;
    if (n > 1024 * 1024)
        return;
    p = (char *)kmalloc((size_t)n);
    if (p == NULL)
        return;
    s_text = p;
    s_cap_cols = cols;
    s_cap_rows = rows;
    con_text_clear_locked(' ');
}

/* 字符网格式上滚一行：KB 级前向搬运（dst<src，从头拷安全） */
static void con_text_scroll_locked()
{
    uint32_t r;
    uint32_t c;

    if (s_text == NULL)
        return;
    for (r = 0; r + 1 < s_rows; r++)
        for (c = 0; c < s_cols; c++)
            s_text[r * s_cap_cols + c] = s_text[(r + 1) * s_cap_cols + c];
    for (c = 0; c < s_cols; c++)
        s_text[(s_rows - 1) * s_cap_cols + c] = ' ';
}

static void con_fill(uint32_t y0, uint32_t y1, uint32_t pixel)
{
    if (y0 >= s_height)
        return;
    if (y1 > s_height)
        y1 = s_height;
    if (y1 <= y0)
        return;
    fb_fillrect_locked(0, y0, s_width, y1 - y0, pixel);
}

static void con_scroll_locked()
{
    uint32_t h;

    if (s_rows == 0)
        return;

    /* 权威内容先滚字符网格（KB 级），再搬像素面 */
    con_text_scroll_locked();

    /* 文本区整体上移一行字高：copyarea + 清末行（fbcon 滚屏序列） */
    h = (s_rows - 1) * FONT_H;
    if (h > 0)
        fb_copyarea_locked(0, s_top + FONT_H, s_width, h, 0, s_top);

    /* 末行清背景 */
    fb_fillrect_locked(0, s_top + (s_rows - 1) * FONT_H,
                       s_width, FONT_H, s_bg);
}

static void con_newline_locked()
{
    s_cx = 0;
    s_cy++;
    while (s_cy >= s_rows)
    {
        con_scroll_locked();
        s_cy = s_rows - 1;
    }
}

static void con_draw_locked(uint32_t px, uint32_t py, const uint8_t *g)
{
    uint32_t r;
    uint32_t c;

    for (r = 0; r < FONT_H; r++)
    {
        uint8_t bits = g[r];

        for (c = 0; c < FONT_W; c++)
            s_fb[(py + r) * s_stride + px + c] =
                (bits & (0x80u >> c)) ? s_fg : s_bg;
    }
}

/* 取指定格字符（无缓冲/越界时视为空格） */
static char con_cell_char_locked(uint32_t col, uint32_t row)
{
    if (s_text != NULL && row < s_rows && col < s_cols &&
        row < s_cap_rows && col < s_cap_cols)
        return s_text[row * s_cap_cols + col];
    return ' ';
}

/* 反白重画一格：字模 1 位用 bg、0 位用 fg（正常绘制的反色） */
static void con_draw_inverted_locked(uint32_t px, uint32_t py, const uint8_t *g)
{
    uint32_t r;
    uint32_t c;

    for (r = 0; r < FONT_H; r++)
    {
        uint8_t bits = g[r];

        for (c = 0; c < FONT_W; c++)
            s_fb[(py + r) * s_stride + px + c] =
                (bits & (0x80u >> c)) ? s_bg : s_fg;
    }
}

/* 在 (s_cx,s_cy) 画/擦反白块光标；调用者须持有锁 */
static void con_cursor_paint_locked(int show)
{
    uint32_t px;
    uint32_t py;
    char ch;

    if (!s_ready || !s_cursor_enabled || s_fb == NULL)
        return;
    if (s_cx >= s_cols || s_cy >= s_rows)
        return;
    px = s_cx * FONT_W;
    py = s_top + s_cy * FONT_H;
    if (px + FONT_W > s_width || py + FONT_H > s_height)
        return;
    ch = con_cell_char_locked(s_cx, s_cy);
    if (show)
        con_draw_inverted_locked(px, py, fb_font_glyph(ch));
    else
        con_draw_locked(px, py, fb_font_glyph(ch));
}

static void con_cursor_hide_locked()
{
    if (s_cursor_phase)
    {
        con_cursor_paint_locked(0);
        s_cursor_phase = 0;
    }
}

/* 写屏后调用：重画光标并重置闪烁周期，避免刚写完就熄灭 */
static void con_cursor_restart_locked()
{
    con_cursor_hide_locked();
    con_cursor_paint_locked(1);
    s_cursor_phase = 1;
    s_cursor_next_ms = uptime_ms() + CON_CURSOR_BLINK_MS;
}

static void con_putchar_locked(char ch)
{
    uint8_t u = (uint8_t)ch;

    if (u == '\n')
    {
        con_newline_locked();
        return;
    }
    if (u == '\r')
    {
        s_cx = 0;
        return;
    }
    if (u == '\b')
    {
        if (s_cx > 0)
        {
            s_cx--;
            if (s_text != NULL && s_cy < s_rows && s_cx < s_cols)
                s_text[s_cy * s_cap_cols + s_cx] = ' ';
            con_draw_locked(s_cx * FONT_W, s_top + s_cy * FONT_H,
                            fb_font_glyph(' '));
        }
        return;
    }
    if (u == '\t')
    {
        do
        {
            con_putchar_locked(' ');
        } while ((s_cx % CON_TAB_STOP) != 0 && s_cx < s_cols);
        return;
    }

    /* 越界字节显示为 '?'，不丢光标进度 */
    if (s_text != NULL && s_cy < s_rows && s_cx < s_cols)
        s_text[s_cy * s_cap_cols + s_cx] = ch;
    con_draw_locked(s_cx * FONT_W, s_top + s_cy * FONT_H, fb_font_glyph(ch));
    s_cx++;
    if (s_cx >= s_cols)
        con_newline_locked();
}

void fb_console_init_boot(boot_info_t *boot)
{
    int32_t cy;
    uint32_t text_top;
    uint32_t rows_below;
    uint64_t flags;

    if (boot == NULL || boot->framebuffer_base == 0 ||
        boot->framebuffer_width == 0 || boot->framebuffer_height == 0)
        return;

    flags = spin_lock_irqsave(&s_lock);
    s_fb = (uint32_t *)(uintptr_t)boot->framebuffer_base;
    s_stride = boot->framebuffer_pixels_per_scanline;
    s_width = boot->framebuffer_width;
    s_height = boot->framebuffer_height;
    s_format = boot->framebuffer_pixel_format;
    s_fg = fb_pack_pixel(CON_FG_RGB, s_format);
    s_bg = fb_pack_pixel(LOGO_SPLASH_BG_RGB, s_format);
    s_cx = 0;
    s_cy = 0;
    s_ready = 0;

    /*
     * 布局：logo 居中，能在其下方放下 ≥CON_MIN_ROWS 行则画 splash
     * 并把文本区开在 logo 之下；否则跳过 splash，全屏当控制台用。
     */
    s_top = 0;
    s_cols = s_width / FONT_W;
    s_rows = s_height / FONT_H;
    if (s_cols == 0 || s_rows == 0)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }

    if (s_width >= LOGO_WIDTH && s_height >= LOGO_HEIGHT &&
        s_cols >= CON_MIN_COLS)
    {
        cy = ((int32_t)s_height - (int32_t)LOGO_HEIGHT) / 2;
        text_top = (uint32_t)(cy + (int32_t)LOGO_HEIGHT + LOGO_SPLASH_TEXT_GAP);
        rows_below = (text_top < s_height)
                         ? (s_height - text_top) / FONT_H
                         : 0;
        if (rows_below >= CON_MIN_ROWS)
        {
            fb_draw_logo_splash(boot, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);
            s_top = text_top;
            s_rows = rows_below;
        }
        else
        {
            con_fill(0, s_height, s_bg);
        }
    }
    else
    {
        con_fill(0, s_height, s_bg);
    }

    con_text_alloc_locked(s_cols, s_rows);
    con_fill(s_top, s_height, s_bg);
    s_ready = 1;
    /* 开机即画出实心光标；timer 启动后 tick 接管闪烁 */
    s_cursor_enabled = 1;
    s_cursor_phase = 0;
    con_cursor_restart_locked();
    spin_unlock_irqrestore(&s_lock, flags);
}

void fb_console_write(const void *buf, uint64_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t i;
    uint64_t flags;

    if (buf == NULL || len == 0)
        return;

    flags = spin_lock_irqsave(&s_lock);
    if (!s_ready)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }
    /* 先擦光标再写，避免滚屏把反白块搬进像素面 */
    con_cursor_hide_locked();
    for (i = 0; i < len; i++)
        con_putchar_locked((char)p[i]);
    con_cursor_restart_locked();
    spin_unlock_irqrestore(&s_lock, flags);
}

void fb_console_tick()
{
    uint64_t flags;
    uint64_t now;

    flags = spin_lock_irqsave(&s_lock);
    if (!s_ready)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }
    now = uptime_ms();
    if (!s_cursor_enabled)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }
    if ((int64_t)(now - s_cursor_next_ms) >= 0)
    {
        s_cursor_phase = !s_cursor_phase;
        con_cursor_paint_locked(s_cursor_phase);
        s_cursor_next_ms = now + CON_CURSOR_BLINK_MS;
    }
    spin_unlock_irqrestore(&s_lock, flags);
}

static void con_reset_full_locked(uint32_t bg)
{
    /* 整屏重绘会抹掉光标块，只复位状态，具体画不画由调用方决定 */
    s_cursor_phase = 0;
    con_fill(0, s_height, bg);
    s_top = 0;
    s_cols = s_width / FONT_W;
    s_rows = s_height / FONT_H;
    s_cx = 0;
    s_cy = 0;
    s_bg = bg;
    con_text_alloc_locked(s_cols, s_rows);
}

void fb_console_clear_full()
{
    uint64_t flags;

    flags = spin_lock_irqsave(&s_lock);
    if (!s_ready)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }
    con_reset_full_locked(s_bg);
    con_cursor_restart_locked();
    spin_unlock_irqrestore(&s_lock, flags);
}

static void con_border_locked(uint32_t color)
{
    uint32_t x;
    uint32_t y;
    uint32_t x1 = s_width - 24;
    uint32_t y1 = s_height - 24;

    for (x = 24; x < x1; x++)
    {
        s_fb[24 * s_stride + x] = color;
        s_fb[(y1 - 1) * s_stride + x] = color;
    }
    for (y = 24; y < y1; y++)
    {
        s_fb[y * s_stride + 24] = color;
        s_fb[y * s_stride + (x1 - 1)] = color;
    }
}

/* 在文本行居中画一行（遇 \0 或 \n 截断，超宽裁剪，不滚屏） */
static void con_draw_centered_locked(uint32_t row, const char *s)
{
    uint32_t len = 0;
    uint32_t x;
    uint32_t i;

    if (row >= s_rows || s == NULL)
        return;
    while (s[len] != '\0' && s[len] != '\n')
        len++;
    if (len > s_cols)
        len = s_cols;
    x = (s_cols > len) ? (s_cols - len) / 2 * FONT_W : 0;
    if (s_text != NULL)
    {
        uint32_t c;
        for (c = 0; c < s_cols; c++)
            s_text[row * s_cap_cols + c] = ' ';
        for (i = 0; i < len; i++)
            s_text[row * s_cap_cols + (x / FONT_W) + i] = s[i];
    }
    for (i = 0; i < len; i++)
        con_draw_locked(x + i * FONT_W, s_top + row * FONT_H,
                        fb_font_glyph(s[i]));
}

void fb_console_panic_page(const char *title, const char *msg)
{
    uint64_t flags;
    uint32_t row;
    uint32_t nlines;
    uint32_t maxlines;
    const char *p;

    if (title == NULL)
        title = "KERNEL PANIC";
    if (msg == NULL)
        msg = "";

    flags = spin_lock_irqsave(&s_lock);
    if (!s_ready)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }

    con_reset_full_locked(fb_pack_pixel(PANIC_BG_RGB, s_format));
    /* 错误页不闪光标：关使能并确保已擦除（reset 已清 phase） */
    s_cursor_enabled = 0;
    if (s_width > 96 && s_height > 96)
        con_border_locked(s_fg);

    /* 先数消息行（截断到能放下的数量）：标题 + 空行 + 消息 + 空行 + 提示 */
    maxlines = (s_rows > 4) ? s_rows - 4 : 0;
    nlines = 0;
    p = msg;
    while (*p != '\0' && nlines < maxlines)
    {
        nlines++;
        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    row = 0;
    if (nlines + 4 < s_rows)
        row = (s_rows - (nlines + 4)) / 2;

    if (row < s_rows)
        con_draw_centered_locked(row, title);
    row += 2;

    p = msg;
    while (*p != '\0' && row + 1 < s_rows && nlines > 0)
    {
        con_draw_centered_locked(row, p);
        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
        row++;
        nlines--;
    }

    row++;
    if (row < s_rows)
        con_draw_centered_locked(row, "SYSTEM HALTED - RESET TO RESTART");
    spin_unlock_irqrestore(&s_lock, flags);
}

void fb_console_puts(const char *s)
{
    uint64_t flags;

    if (s == NULL)
        return;

    flags = spin_lock_irqsave(&s_lock);
    if (!s_ready)
    {
        spin_unlock_irqrestore(&s_lock, flags);
        return;
    }
    con_cursor_hide_locked();
    while (*s)
        con_putchar_locked(*s++);
    con_cursor_restart_locked();
    spin_unlock_irqrestore(&s_lock, flags);
}

void fb_console_size(uint32_t *cols, uint32_t *rows)
{
    uint64_t flags;

    if (cols == NULL || rows == NULL)
        return;

    flags = spin_lock_irqsave(&s_lock);
    if (s_ready)
    {
        *cols = s_cols;
        *rows = s_rows;
    }
    else
    {
        *cols = 0;
        *rows = 0;
    }
    spin_unlock_irqrestore(&s_lock, flags);
}
