#ifndef CSOS_GFX_CONSOLE_H
#define CSOS_GFX_CONSOLE_H

#include <kernel/memory/pmm.h>
#include <stdint.h>

void fb_console_init_boot(boot_info_t *boot);

void fb_console_write(const void *buf, uint64_t len);

void fb_console_tick();

void fb_console_clear_full();

void fb_console_panic_page(const char *title, const char *msg);

void fb_console_puts(const char *s);

void fb_console_size(uint32_t *cols, uint32_t *rows);

/* 屏幕仲裁：detach 后停写屏，文字模型冻结。 */
/* attach 按文字模型重画并恢复光标。 */
/* panic 页无条件夺回。 */
void fb_console_detach();
void fb_console_attach();

#endif /* CSOS_GFX_CONSOLE_H */
