#include <gfx/console.h>
#include <gfx/fb.h>
#include <gfx/font.h>
#include <gfx/logo.h>
#include <gfx/layout.h>
#include <spinlock.h>

#define CON_FG_RGB 0xE8E8E8U
#define CON_TAB_STOP 8
#define CON_MIN_ROWS 3
#define CON_MIN_COLS 20

static uint32_t *s_fb;     /* 帧缓冲基址（32 位像素数组） */
static uint32_t s_stride;  /* 每扫描行像素数（含行尾填充） */
static uint32_t s_width;   /* 屏幕宽（像素） */
static uint32_t s_height;  /* 屏幕高（像素） */
static uint32_t s_format;  /* 像素格式（RGB/BGR，见 FB_PIXEL_*） */
static uint32_t s_fg;      /* 前景色（已按 format 打包） */
static uint32_t s_bg;      /* 背景色（清屏/滚屏补行用） */
static uint32_t s_cols;    /* 文本列数（= 宽 / 8） */
static uint32_t s_rows;    /* 文本行数（= 文本区高 / 16） */
static uint32_t s_top;     /* 文本区首像素行 */
static uint32_t s_cx;      /* 光标列 */
static uint32_t s_cy;      /* 光标行 */
static int s_ready;        /* 初始化完成标志（1 才可输出） */
static spinlock_t s_lock = SPINLOCK_INIT;

static void con_fill(uint32_t y0, uint32_t y1, uint32_t pixel)
{
    uint32_t y;
    uint32_t x;

    if (y0 >= s_height)
        return;
    if (y1 > s_height)
        y1 = s_height;
    for (y = y0; y < y1; y++)
        for (x = 0; x < s_width; x++)
            s_fb[y * s_stride + x] = pixel;
}

static void con_scroll_locked()
{
    uint32_t r;
    uint32_t y;
    uint32_t x;

    /* 文本行整体上移一行 */
    for (r = 0; r + 1 < s_rows; r++)
        for (y = 0; y < FONT_H; y++)
            for (x = 0; x < s_width; x++)
                s_fb[(s_top + r * FONT_H + y) * s_stride + x] =
                    s_fb[(s_top + (r + 1) * FONT_H + y) * s_stride + x];

    /* 末行清背景 */
    con_fill(s_top + (s_rows - 1) * FONT_H, s_top + s_rows * FONT_H, s_bg);
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

    con_fill(s_top, s_height, s_bg);
    s_ready = 1;
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
    for (i = 0; i < len; i++)
        con_putchar_locked((char)p[i]);
    spin_unlock_irqrestore(&s_lock, flags);
}

static void con_reset_full_locked(uint32_t bg)
{
    con_fill(0, s_height, bg);
    s_top = 0;
    s_cols = s_width / FONT_W;
    s_rows = s_height / FONT_H;
    s_cx = 0;
    s_cy = 0;
    s_bg = bg;
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
    while (*s)
        con_putchar_locked(*s++);
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
