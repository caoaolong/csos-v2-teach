#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* 启动 display 服务 */
static long start_display()
{
    long pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        char *const argv[] = {"display", NULL};

        execve("/sbin/display", argv, NULL);

        /* execve 只有执行失败才会返回 */
        printf("execve display failed\n");
        exit(1);
    }

    printf("display started, pid=%ld\n", pid);
    return pid;
}

int main()
{
    /* 1. 启动系统服务 */
    long display_pid = start_display();

    /* 2. 进入子进程监控循环 */
    for (;;)
    {
        int status;
        long pid = wait(&status);

        if (pid < 0)
        {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                break;

            continue;
        }

        printf("reaped pid=%ld exit=%d\n", pid, status);
        if (pid == display_pid)
            display_pid = start_display();
    }
}
