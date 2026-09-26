#ifndef CSOS_USER_ULIB_H
#define CSOS_USER_ULIB_H

#include <user.h>

unsigned long strlen(const char *s);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *s, int c, unsigned long n);

/* 睡眠 ms 毫秒（int 0x80 / SYS_SLEEP，由内核定时器唤醒） */
void sleep(unsigned long ms);

#endif /* CSOS_USER_ULIB_H */
