#include <unistd.h>
#include <string.h>

static void puts(const char *s)
{
    write(1, s, strlen(s));
}

int main()
{
    puts("csos output demo (write-only)\n");
    for (;;)
    {
        puts("tick\n");
        sleep(500);
    }
}
