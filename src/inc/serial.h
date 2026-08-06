#ifndef SERIAL_H
#define SERIAL_H

#include <kernel.h>

#define VGA_ADDR ((volatile uint16_t *)0xB8000)
#define COM1 0x3F8

void serial_init();
void put_string(const char *s);
int fput_string(const char *fmt, ...);

#endif /* SERIAL_H */