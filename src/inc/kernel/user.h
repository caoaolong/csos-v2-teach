#ifndef CSOS_USER_H
#define CSOS_USER_H

#include <stdint.h>

#define USER_STACK_VADDR 0x40001000ULL
#define USER_STACK_TOP (USER_STACK_VADDR + 0x1000ULL)

/* 用户地址空间范围：init/display 映像 + 用户栈；exec 时整体拆除重建 */
#define USER_IMAGE_BASE 0x40000000ULL
#define USER_IMAGE_END 0x42000000ULL

/* 单次 exec 允许的最大参数/环境个数（栈预算内） */
#define EXEC_MAX_ARGS 16

/* 内核内部入口：加载并进入用户态（不返回），仅内核侧使用 */
void user_enter_init(const char *init_path);

/* execve 内核侧：kpath/kargv/kenvp 均已拷入内核；成功返回 0 并输出新入口/栈顶，
 * 旧用户地址空间已被销毁；-1 表示装载前失败（原程序不受影响）。 */
int user_exec_replace(const char *kpath, char *const kargv[], int kargc,
                      char *const kenvp[], int kenvc,
                      uint64_t *entry_out, uint64_t *rsp_out);

#endif /* CSOS_USER_H */
