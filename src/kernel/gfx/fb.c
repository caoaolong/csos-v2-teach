#include <gfx/fb.h>
#include <gfx/logo.h>
#include <serial.h>

/* EFI_GRAPHICS_PIXEL_FORMAT */
#define FB_PIXEL_RGB 0 /* byte order R,G,B,X */
#define FB_PIXEL_BGR 1 /* byte order B,G,R,X */

static uint32_t xrgb_to_fb(uint32_t xrgb, uint32_t format)
{
    uint32_t r = (xrgb >> 16) & 0xff;
    uint32_t g = (xrgb >> 8) & 0xff;
    uint32_t b = xrgb & 0xff;

    if (format == FB_PIXEL_BGR)
    {
        /* LE store of 0x00RRGGBB => memory B,G,R,X */
        return (r << 16) | (g << 8) | b;
    }

    /* FB_PIXEL_RGB: LE store of 0x00BBGGRR => memory R,G,B,X */
    return (b << 16) | (g << 8) | r;
}

void fb_draw_image_centered(boot_info_t *boot,
                            const uint32_t *pixels,
                            uint32_t img_w,
                            uint32_t img_h)
{
    uint32_t *fb;
    uint32_t stride;
    int32_t x0;
    int32_t y0;
    uint32_t y;
    uint32_t x;

    if (boot == NULL || boot->framebuffer_base == 0 || pixels == NULL)
        return;
    if (boot->framebuffer_width == 0 || boot->framebuffer_height == 0)
        return;

    fb = (uint32_t *)(uintptr_t)boot->framebuffer_base;
    stride = boot->framebuffer_pixels_per_scanline;
    x0 = ((int32_t)boot->framebuffer_width - (int32_t)img_w) / 2;
    y0 = ((int32_t)boot->framebuffer_height - (int32_t)img_h) / 2;

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
            /* 源图里纯黑多为 PNG 透明转出的 0，跳过以保留背景 */
            if (src == 0)
                continue;

            fb[(uint32_t)dy * stride + (uint32_t)dx] =
                xrgb_to_fb(src, boot->framebuffer_pixel_format);
        }
    }
}
