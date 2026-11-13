#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define LINE_MAX 128

static unsigned long read_line(char *line, unsigned long cap)
{
    unsigned long len = 0;
    char c;

    for (;;)
    {
        long n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            continue;
        if (c == '\n')
        {
            printf("\n");
            break;
        }
        if (c == '\b' || c == 0x7f)
        {
            if (len > 0)
            {
                len--;
                printf("\b \b");
            }
            continue;
        }
        if (len + 1 < cap)
        {
            line[len++] = c;
            printf("%c", c);
        }
    }
    line[len] = '\0';
    return len;
}

static long launch_display()
{
    long pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        char *const argv[] = {"display", 0};

        if (execve("/sbin/display", argv, 0) < 0)
        {
            printf("exec /sbin/display failed\n");
            exit(1);
        }
    }
    return pid;
}

#define CAT_BUF_SIZE 512

static void do_cat(const char *path)
{
    static char buf[CAT_BUF_SIZE];
    long fd = open(path, O_RDONLY);
    long n;

    if (fd < 0)
    {
        printf("cat: cannot open '%s'\n", path);
        return;
    }
    for (;;)
    {
        n = read((int)fd, buf, sizeof(buf));
        if (n < 0)
        {
            printf("cat: read error\n");
            break;
        }
        if (n == 0)
            break; /* EOF */
        if (write(STDOUT_FILENO, buf, (unsigned long)n) < 0)
        {
            printf("cat: write error\n");
            break;
        }
    }
    close((int)fd);
}

int main()
{
    static char line[LINE_MAX];

    printf("csos shell. type 'help'.\n");
    if (launch_display() < 0)
        printf("display launch failed, fallback to shell\n");
    else
        printf("display launched\n");
    for (;;)
    {
        {
            int st = 0;
            long pid = waitpid(-1, &st, WNOHANG);

            if (pid > 0)
                printf("reaped pid=%ld exit=%d\n", pid, st);
        }
        printf("csos> ");
        read_line(line, sizeof(line));
        if (line[0] == '\0')
            continue;
        if (strcmp(line, "help") == 0)
            printf("commands: help, cat <path>\n");
        else if (strncmp(line, "cat ", 4) == 0)
        {
            const char *p = line + 4;

            while (*p == ' ')
                p++;
            if (*p == '\0')
                printf("usage: cat <path>\n");
            else
                do_cat(p);
        }
        else
        {
            printf("unknown: %s\n", line);
        }
    }
}
