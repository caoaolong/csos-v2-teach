#include <assert.h>
#include <panic.h>

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    panic("ASSERT FAILED: %s at %s:%d in %s()\n", expr, file, line, func);
}