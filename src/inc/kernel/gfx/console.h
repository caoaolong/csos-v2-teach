#ifndef CSOS_GFX_CONSOLE_H
#define CSOS_GFX_CONSOLE_H

#include <memory/pmm.h>
#include <stdint.h>

void fb_console_init_boot(boot_info_t *boot);

void fb_console_write(const void *buf, uint64_t len);

void fb_console_tick();

void fb_console_clear_full();

void fb_console_panic_page(const char *title, const char *msg);

void fb_console_puts(const char *s);

void fb_console_size(uint32_t *cols, uint32_t *rows);

#endif /* CSOS_GFX_CONSOLE_H */
