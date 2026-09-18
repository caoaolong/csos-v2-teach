#include "ulib.h"

int main()
{
    char name[5];

    name[0] = 'i';
    name[1] = 'n';
    name[2] = 'i';
    name[3] = 't';
    name[4] = '\0';
    if (strlen(name) != 4)
    {
        for (;;)
            __asm__ volatile("hlt");
    }

    /* 待机：系统调用接入前无事可做 */
    for (;;)
        __asm__ volatile("pause");

    return 0; /* 不可达 */
}
