#ifndef CSOS_PANIC_H
#define CSOS_PANIC_H

void panic(const char *fmt, ...) __attribute__((noreturn));

#endif /* CSOS_PANIC_H */
