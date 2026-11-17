#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* 启动 compositor 服务（输入归属 + 合成输出，独占键盘/鼠标与显存） */
static long start_compositor()
{
    long pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        char *const argv[] = {"compositor", NULL};

        execve("/sbin/compositor", argv, NULL);

        /* execve 只有执行失败才会返回 */
        printf("execve compositor failed\n");
        exit(1);
    }

    printf("compositor started, pid=%ld\n", pid);
    return pid;
}

int main()
{
    /* 1. 启动系统服务 */
    long compositor_pid = start_compositor();

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
        if (pid == compositor_pid)
            compositor_pid = start_compositor();
    }
}
