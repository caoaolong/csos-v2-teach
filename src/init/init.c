#include <unistd.h>
#include <string.h>

static void puts(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

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
            puts("\n");
            break;
        }
        if (c == '\b' || c == 0x7f)
        {
            if (len > 0)
            {
                len--;
                puts("\b \b");
            }
            continue;
        }
        if (len + 1 < cap)
        {
            line[len++] = c;
            write(STDOUT_FILENO, &c, 1);
        }
    }
    line[len] = '\0';
    return len;
}

int main()
{
    static char line[LINE_MAX];

    puts("csos shell. type 'help'.\n");
    for (;;)
    {
        puts("csos> ");
        read_line(line, sizeof(line));
        if (line[0] == '\0')
            continue;
        if (strcmp(line, "help") == 0)
            puts("commands: help, display\n");
        else if (strcmp(line, "display") == 0)
        {
            /* fork+exec 拉起：子进程替换为 display，父 shell 继续值守
             * TODO: init 常驻回收器，wait 掉已退出的后台子进程防僵尸堆积 */
            long pid = fork();

            if (pid < 0)
                puts("fork failed\n");
            else if (pid == 0)
            {
                char *const argv[] = {"display", 0};

                if (execve("/sbin/display", argv, 0) < 0)
                {
                    puts("exec /sbin/display failed\n");
                    exit(1);
                }
            }
            else
                puts("display launched\n");
        }
        else
        {
            puts("unknown: ");
            puts(line);
            puts("\n");
        }
    }
}
