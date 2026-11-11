#include <kernel/panic.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>
#include <kernel/gfx/console.h>

static char g_panic_buf[1024];

void panic(const char *fmt, ...)
{
    va_list ap;
    int n;

    __asm__ volatile("cli");

    va_start(ap, fmt);
    n = vsprintf(g_panic_buf, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        g_panic_buf[0] = '\0';
        n = 0;
    }

    /* 先画屏：即使串口锁恰好被占（极端重入），屏上已有信息 */
    fb_console_panic_page("KERNEL PANIC", g_panic_buf);

    put_string("[PANIC] ");
    put_string(g_panic_buf);
    if (n <= 0 || g_panic_buf[n - 1] != '\n')
        put_string("\n");

    for (;;)
        __asm__ volatile("hlt");
}
