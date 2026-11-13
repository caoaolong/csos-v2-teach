#include <unistd.h>
#include <stdio.h>

/* 占位程序：后续改造成 Display Manager Service。
 * TODO(display):
 *  - 经 /dev/fb0（待建）查询显存信息并映射到用户态；
 *  - 与 fb_console 仲裁屏幕归属，接管输出；
 *  - 对外提供合成 IPC 接口，客户端不再直写显存。
 */

int main()
{
    printf("display: placeholder, screen not owned yet\n");
    for (;;)
        sleep(1000);
    return 0;
}
