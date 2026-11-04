#ifdef NDEBUG
#define assert(cond) ((void)0)
#else
#define assert(cond) \
    ((cond) ? (void)0 : __assert_fail(#cond, __FILE__, __LINE__, __func__))
void __assert_fail(const char *expr, const char *file, int line, const char *func);
#endif