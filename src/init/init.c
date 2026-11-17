#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* 启动 compositor 服务 */
static long start_compositor()
{
    long pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        char *const argv[] = {"compositor", NULL};

        execve("/sbin/compositor", argv, NULL);

        /* execve 失败才会返回 */
        printf("execve compositor failed\n");
        exit(1);
    }

    printf("compositor started, pid=%ld\n", pid);
    return pid;
}

/* 启动 term 终端客户端 */
static long start_term()
{
    long pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        char *const argv[] = {"term", NULL};

        execve("/bin/term", argv, NULL);

        /* execve 失败才会返回 */
        printf("execve term failed\n");
        exit(1);
    }

    printf("term started, pid=%ld\n", pid);
    return pid;
}

int main()
{
    /* 按序启动服务：先 compositor 建 fifo，再启动 term */
    long compositor_pid = start_compositor();
    long term_pid = start_term();

    /* 进入子进程监控循环 */
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
        if (pid == term_pid)
            term_pid = start_term();
    }
}
