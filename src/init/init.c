#include <unistd.h>
#include <string.h>

static void puts(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

#define LINE_MAX 128

/* 新 fd 系调用自测：只动 >=3 的 fd（t4 借 1 做 dup2 但随后归还） */
static int test_fd()
{
    int pass = 0;
    long fd, n, pos;
    static char magic[4];

    /* t1: /dev/null 写+关 */
    fd = open("/dev/null", O_WRONLY);
    if (fd >= 3)
    {
        n = write(fd, "hi", 2);
        if (n == 2 && close(fd) == 0)
        {
            pass++;
            puts("t1 null write/close ok\n");
        }
        else
            puts("t1 FAIL io\n");
    }
    else
        puts("t1 FAIL open\n");

    /* t2: 不存在路径应失败 */
    if (open("/no/such", O_RDONLY) < 0)
    {
        pass++;
        puts("t2 bad open -> -1 ok\n");
    }
    else
        puts("t2 FAIL\n");

    /* t3: /dev/console 写+关（行本身即写通证明） */
    fd = open("/dev/console", O_WRONLY);
    if (fd >= 3)
    {
        const char *s = "t3 console ok\n";
        n = write(fd, s, strlen(s));
        if (n == (long)strlen(s) && close(fd) == 0)
            pass++;
        else
            puts("t3 FAIL io\n");
    }
    else
        puts("t3 FAIL open\n");

    /* t4: dup2 共享描述，关复制后原 fd 仍可用 */
    if (dup2(STDOUT_FILENO, 5) == 5)
    {
        const char *a = "t4 dup2 ok\n";
        const char *b = "t4 stdout intact\n";
        n = write(5, a, strlen(a));
        if (n == (long)strlen(a) && close(5) == 0)
        {
            n = write(STDOUT_FILENO, b, strlen(b));
            if (n == (long)strlen(b))
                pass++;
        }
        else
            puts("t4 FAIL io\n");
    }
    else
        puts("t4 FAIL dup2\n");

    /* t5: 字符设备不可 seek */
    if (lseek(STDOUT_FILENO, 0, SEEK_SET) < 0)
    {
        pass++;
        puts("t5 console seek -> -1 ok\n");
    }
    else
        puts("t5 FAIL\n");

    /* t6: 普通文件 seek END/SET + 读 */
    fd = open("/sbin/init", O_RDONLY);
    if (fd >= 3)
    {
        pos = lseek(fd, 0, SEEK_END);
        if (pos > 0 && lseek(fd, 0, SEEK_SET) == 0)
        {
            n = read(fd, magic, sizeof(magic));
            if (n == 4 && close(fd) == 0)
            {
                pass++;
                puts("t6 file seek/read ok\n");
            }
            else
                puts("t6 FAIL io\n");
        }
        else
            puts("t6 FAIL seek\n");
    }
    else
        puts("t6 FAIL open\n");

    return pass;
}

static void test_fd_summary(int pass)
{
    char line[32];
    int n = 0;

    line[n++] = 't';
    line[n++] = 'e';
    line[n++] = 's';
    line[n++] = 't';
    line[n++] = 'f';
    line[n++] = 'd';
    line[n++] = ':';
    line[n++] = ' ';
    line[n++] = (char)('0' + pass);
    line[n++] = '/';
    line[n++] = '6';
    if (pass == 6)
    {
        const char *s = " passed\n";
        unsigned long i;
        for (i = 0; s[i]; i++)
            line[n++] = s[i];
    }
    else
    {
        const char *s = " FAILED\n";
        unsigned long i;
        for (i = 0; s[i]; i++)
            line[n++] = s[i];
    }
    write(STDOUT_FILENO, line, (unsigned long)n);
}

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
    test_fd_summary(test_fd());
    for (;;)
    {
        puts("csos> ");
        read_line(line, sizeof(line));
        if (line[0] == '\0')
            continue;
        if (strcmp(line, "help") == 0)
            puts("commands: help, testfd, display\n");
        else if (strcmp(line, "testfd") == 0)
            test_fd_summary(test_fd());
        else if (strcmp(line, "display") == 0)
        {
            /* 替换式拉起：成功则 shell 即被 display 取代，不返回 */
            char *const argv[] = {"display", 0};

            if (execve("/sbin/display", argv, 0) < 0)
                puts("exec /sbin/display failed (need make master)\n");
        }
        else
        {
            puts("unknown: ");
            puts(line);
            puts("\n");
        }
    }
}
