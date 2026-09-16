#include <user.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <serial.h>
#include <string.h>
#include <tss.h>

extern char user_demo_start[];
extern char user_demo_end[];
extern void enter_user(uint64_t rip, uint64_t rsp);

static void *g_kernel_stack_page;

static void user_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

void user_enter_demo()
{
    void *code_page;
    void *stack_page;
    uint64_t demo_size;
    uint64_t kstack_top;

    demo_size = (uint64_t)(user_demo_end - user_demo_start);
    if (demo_size == 0 || demo_size > PAGE_SIZE)
        user_halt("FATAL: user demo size invalid\n");

    code_page = (void *)alloc_page();
    stack_page = (void *)alloc_page();
    g_kernel_stack_page = (void *)alloc_page();
    if (code_page == NULL || stack_page == NULL || g_kernel_stack_page == NULL)
        user_halt("FATAL: user_enter_demo alloc_page failed\n");

    kernel_memset(code_page, 0, (uint32_t)PAGE_SIZE);
    kernel_memset(stack_page, 0, (uint32_t)PAGE_SIZE);
    kernel_memset(g_kernel_stack_page, 0, (uint32_t)PAGE_SIZE);
    kernel_memcpy(code_page, user_demo_start, (uint32_t)demo_size);

    if (map_page(USER_CODE_VADDR, (uint64_t)(uintptr_t)code_page,
                 PTE_PRESENT | PTE_USER) != 0)
        user_halt("FATAL: map user code failed\n");

    if (map_page(USER_STACK_VADDR, (uint64_t)(uintptr_t)stack_page,
                 PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0)
        user_halt("FATAL: map user stack failed\n");

    kstack_top = (uint64_t)(uintptr_t)g_kernel_stack_page + PAGE_SIZE;
    tss_set_rsp0(kstack_top);

    fput_string("[USER] enter rip=0x%llx rsp=0x%llx rsp0=0x%llx demo=%u\n",
                (unsigned long long)USER_CODE_VADDR,
                (unsigned long long)USER_STACK_TOP,
                (unsigned long long)kstack_top,
                (unsigned)demo_size);

    enter_user(USER_CODE_VADDR, USER_STACK_TOP);
}
