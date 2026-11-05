#ifndef CSOS_GFX_FONT_H
#define CSOS_GFX_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

const uint8_t *fb_font_glyph(char c);

#endif /* CSOS_GFX_FONT_H */
