#ifndef CSOS_GFX_FONT_H
#define CSOS_GFX_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/* 取字符的字模（16 字节，MSB 为左像素）；小写折大写，范围外返回 '?' */
const uint8_t *fb_font_glyph(char c);

#endif /* CSOS_GFX_FONT_H */
