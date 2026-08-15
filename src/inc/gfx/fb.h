#ifndef CSOS_GFX_FB_H
#define CSOS_GFX_FB_H

#include <memory/pmm.h>
#include <stdint.h>

void fb_draw_image_centered(boot_info_t *boot,
                            const uint32_t *pixels,
                            uint32_t img_w,
                            uint32_t img_h);

#endif /* CSOS_GFX_FB_H */
