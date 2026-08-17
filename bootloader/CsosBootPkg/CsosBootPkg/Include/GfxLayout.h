/** @file
  Shared splash constants. Logo is independently screen-centered on both
  EfiBoot and the kernel; these values only affect EfiBoot hint text / bg.

  Keep in sync with src/inc/gfx/layout.h in the csos-v2-teach project.
**/

#ifndef CSOS_GFX_LAYOUT_H
#define CSOS_GFX_LAYOUT_H

#define LOGO_SPLASH_TEXT_GAP  28
#define LOGO_SPLASH_FONT_PX   19
#define LOGO_SPLASH_BG_RGB    0x0F0F12U

#endif /* CSOS_GFX_LAYOUT_H */
