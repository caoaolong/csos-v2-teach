#include <unistd.h>
#include <stdio.h>

int main()
{
    printf("display: placeholder, screen not owned yet\n");
    sleep(2000);
    printf("display: exiting with code 42\n");
    exit(42);
    return 0;
}
