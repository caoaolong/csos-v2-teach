#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

/* ls：列目录，一行一项，目录名缀 /；无参列 "." */

#define LS_MAX 128

static csos_dirent_t g_ents[LS_MAX];

int main(int argc, char *argv[])
{
    const char *path = ".";
    long n;
    long i;

    if (argc > 1)
        path = argv[1];
    n = getdents(path, g_ents, LS_MAX);
    if (n < 0)
    {
        if (errno == ENOENT)
            printf("ls: no such dir\n");
        else if (errno == ENOTDIR)
            printf("ls: not a dir\n");
        else
            printf("ls: failed\n");
        exit(1);
    }
    for (i = 0; i < n; i++)
    {
        if (g_ents[i].is_dir)
            printf("%s/\n", g_ents[i].name);
        else
            printf("%s\n", g_ents[i].name);
    }
    exit(0);
}
