#ifndef CSOS_GFX_CONSOLE_H
#define CSOS_GFX_CONSOLE_H

#include <memory/pmm.h>
#include <stdint.h>

/*
 * 内核帧缓冲文本控制台：8x16 点阵，无光标。
 * init 前调用写接口是安全的（直接丢弃），故 serial 可无条件镜像。
 */

/* 按分辨率自动布局：能放下 logo+≥3 行日志则画 splash 并在下方开日志区，
// 否则跳过 splash 全屏当控制台用；失败（无显存）则保持未就绪 */
void fb_console_init_boot(boot_info_t *boot);

/* 输出原始字节流（处理 \n \r \t \b，自动换行滚屏） */
void fb_console_write(const void *buf, uint64_t len);

/* 全屏清背景，文本区重置为全屏、光标归零；供进入 init 前清掉 splash */
void fb_console_clear_full();

/* 错误页：全屏底色 + 白框 + 居中标题/内容（版式对齐引导错误页）；
 * 未就绪时无动作。调用后系统一般直接停机。 */
void fb_console_panic_page(const char *title, const char *msg);

/* 输出 NUL 结尾字符串 */
void fb_console_puts(const char *s);

/* 取文本列数/行数；未就绪时填 0 */
void fb_console_size(uint32_t *cols, uint32_t *rows);

#endif /* CSOS_GFX_CONSOLE_H */
