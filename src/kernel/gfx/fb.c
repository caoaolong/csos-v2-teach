#include <kernel/gfx/fb.h>
#include <kernel/gfx/layout.h>
#include <kernel/serial.h>

/* 像素格式定义 */
#define FB_PIXEL_RGB 0 /* 内存顺序 R,G,B,X */
#define FB_PIXEL_BGR 1 /* 内存顺序 B,G,R,X */

uint32_t fb_pack_pixel(uint32_t xrgb, uint32_t format)
{
    uint32_t r = (xrgb >> 16) & 0xff;
    uint32_t g = (xrgb >> 8) & 0xff;
    uint32_t b = xrgb & 0xff;

    if (format == FB_PIXEL_BGR)
    {
        /* 小端存储转 BGR */
        return (r << 16) | (g << 8) | b;
    }

    /* 小端存储转 RGB */
    return (b << 16) | (g << 8) | r;
}

static uint32_t xrgb_to_fb(uint32_t xrgb, uint32_t format)
{
    return fb_pack_pixel(xrgb, format);
}

static void fb_fill(boot_info_t *boot, uint32_t color_rgb)
{
    uint32_t *fb;
    uint32_t stride;
    uint32_t pixel;
    uint32_t count;
    uint32_t i;

    if (boot == NULL || boot->framebuffer_base == 0)
        return;
    if (boot->framebuffer_width == 0 || boot->framebuffer_height == 0)
        return;

    fb = (uint32_t *)(uintptr_t)boot->framebuffer_base;
    stride = boot->framebuffer_pixels_per_scanline;
    pixel = xrgb_to_fb(color_rgb, boot->framebuffer_pixel_format);
    count = stride * boot->framebuffer_height;

    for (i = 0; i < count; i++)
        fb[i] = pixel;
}

static void fb_draw_image_at(boot_info_t *boot,
                             const uint32_t *pixels,
                             uint32_t img_w,
                             uint32_t img_h,
                             int32_t x0,
                             int32_t y0)
{
    uint32_t *fb;
    uint32_t stride;
    uint32_t y;
    uint32_t x;

    if (boot == NULL || boot->framebuffer_base == 0 || pixels == NULL)
        return;

    fb = (uint32_t *)(uintptr_t)boot->framebuffer_base;
    stride = boot->framebuffer_pixels_per_scanline;

    for (y = 0; y < img_h; y++)
    {
        int32_t dy = y0 + (int32_t)y;
        if (dy < 0 || (uint32_t)dy >= boot->framebuffer_height)
            continue;

        for (x = 0; x < img_w; x++)
        {
            int32_t dx = x0 + (int32_t)x;
            uint32_t src;

            if (dx < 0 || (uint32_t)dx >= boot->framebuffer_width)
                continue;

            src = pixels[y * img_w + x];
            /* 纯黑像素跳过以保留背景 */
            if (src == 0)
                continue;

            fb[(uint32_t)dy * stride + (uint32_t)dx] =
                xrgb_to_fb(src, boot->framebuffer_pixel_format);
        }
    }
}

void fb_draw_image_centered(boot_info_t *boot,
                            const uint32_t *pixels,
                            uint32_t img_w,
                            uint32_t img_h)
{
    int32_t x0;
    int32_t y0;

    if (boot == NULL || boot->framebuffer_base == 0 || pixels == NULL)
        return;
    if (boot->framebuffer_width == 0 || boot->framebuffer_height == 0)
        return;

    x0 = ((int32_t)boot->framebuffer_width - (int32_t)img_w) / 2;
    y0 = ((int32_t)boot->framebuffer_height - (int32_t)img_h) / 2;
    fb_draw_image_at(boot, pixels, img_w, img_h, x0, y0);
}

void fb_draw_logo_splash(boot_info_t *boot,
                         const uint32_t *pixels,
                         uint32_t img_w,
                         uint32_t img_h)
{
    if (boot == NULL || boot->framebuffer_base == 0 || pixels == NULL)
        return;

    /* 与启动画面同公式，居中徽标 */
    fb_fill(boot, LOGO_SPLASH_BG_RGB);
    fb_draw_image_centered(boot, pixels, img_w, img_h);
}
