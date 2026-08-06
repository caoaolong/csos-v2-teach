#include <assert.h>

void __assert_fail(const char *expr, const char *file, int line)
{
    fput_string("ASSERT FAILED: %s at %s:%d\n", expr, file, line);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}