#ifndef CSOS_COMP_FONT_H
#define CSOS_COMP_FONT_H

#include <stdint.h>

/* 用户态 8x16 点阵（Terminus ter-116n，SIL OFL 1.1），数据见 font.c
 * （与内核 font.c 同源；compositor 执行 TEXT 命令时的光栅化用）。 */
#define COMP_FONT_W 8
#define COMP_FONT_H 16

const uint8_t *comp_font_glyph(char c);

#endif /* CSOS_COMP_FONT_H */
