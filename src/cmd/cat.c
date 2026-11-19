#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

/* cat：顺序倾倒文件到 stdout；无参报错（v1 不读 stdin） */

static uint8_t g_buf[512];

static int cat_one(const char *path)
{
    long fd = open(path, O_RDONLY);
    long n;

    if (fd < 0)
    {
        if (errno == ENOENT)
            printf("cat: no such file\n");
        else
            printf("cat: open failed\n");
        return -1;
    }
    for (;;)
    {
        unsigned long off = 0;

        n = read((int)fd, g_buf, sizeof(g_buf));
        if (n == 0)
            break;
        if (n < 0)
        {
            printf("cat: read failed\n");
            close((int)fd);
            return -1;
        }
        /* 经由 stdout 管道回 term 窗；管道满时短写，重试余量 */
        while (off < (unsigned long)n)
        {
            long w = write(STDOUT_FILENO, g_buf + off,
                           (unsigned long)n - off);

            if (w <= 0)
            {
                close((int)fd);
                return -1;
            }
            off += (unsigned long)w;
        }
    }
    close((int)fd);
    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    int bad = 0;

    if (argc < 2)
    {
        printf("usage: cat <file>...\n");
        exit(1);
    }
    for (i = 1; i < argc; i++)
    {
        if (cat_one(argv[i]) != 0)
            bad = 1;
    }
    exit(bad);
}
