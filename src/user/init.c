#include "ulib.h"

int main(void)
{
    for (;;)
        sleep(500);

    return 0; /* 不可达 */
}