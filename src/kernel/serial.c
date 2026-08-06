#include <kernel.h>
#include <serial.h>

static void serial_putchar(char c)
{
    if (c == '\n')
    {
        serial_putchar('\r');
    }

    while ((inb(COM1 + 5) & 0x20) == 0)
        ;

    outb(COM1, (uint8_t)c);
}

void serial_init()
{
    outb(COM1 + 1, 0x00); /* 关闭中断 */
    outb(COM1 + 3, 0x80); /* 打开 DLAB */
    outb(COM1 + 0, 0x03); /* 115200/3 = 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* 启用 FIFO */
    outb(COM1 + 4, 0x0B); /* IRQ、RTS/DSR */
}

void put_string(const char *s)
{
    static size_t cursor = 0;

    for (; *s; s++)
    {
        VGA_ADDR[cursor++] = (uint16_t)*s | 0x0F00;
        serial_putchar(*s);
    }
}

int fput_string(const char *fmt, ...)
{
    static char buf[1024];
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsprintf(buf, fmt, args);
    va_end(args);

    put_string(buf);
    return i;
}