#ifndef CSOS_FBINFO_H
#define CSOS_FBINFO_H

#include <stdint.h>

/* 内核↔用户态显存描述：/dev/fb0 的 read() 一次返回 32 字节；
 * 映射显存用 fbmap()（SYS_FBMAP），返回像素起点的用户态虚地址。 */
typedef struct fb_info
{
    uint64_t phys_base; /* 显存物理基址（GOP framebuffer_base） */
    uint32_t width;     /* 可见宽（像素） */
    uint32_t height;    /* 可见高（像素） */
    uint32_t pitch;     /* 每行字节数（= pixels_per_scanline * 4） */
    uint32_t bpp;       /* 每像素位数，恒 32（XRGB8888） */
    uint32_t phys_w_mm; /* 显示器物理宽（EDID，毫米；0=未知，按 96DPI 保底） */
    uint32_t phys_h_mm; /* 显示器物理高（EDID，毫米；0=未知） */
} fb_info_t;

#endif /* CSOS_FBINFO_H */
