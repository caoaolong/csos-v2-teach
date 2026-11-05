#ifndef CSOS_LIBC_STRING_H
#define CSOS_LIBC_STRING_H

#define NULL ((void *)0)

unsigned long strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, unsigned long n);
char *strcat(char *dst, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);
char *strchr(const char *s, int c);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *s, int c, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);

#endif /* CSOS_LIBC_STRING_H */
