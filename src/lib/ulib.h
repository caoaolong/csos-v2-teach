#ifndef CSOS_USER_ULIB_H
#define CSOS_USER_ULIB_H

unsigned long strlen(const char *s);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *s, int c, unsigned long n);

#endif /* CSOS_USER_ULIB_H */
