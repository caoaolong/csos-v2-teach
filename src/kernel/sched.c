#include <sched.h>
#include <memory/heap.h>
#include <memory/pmm.h>
#include <string.h>
#include <serial.h>
#include <gdt.h>

task_t *current;

static task_t g_idle;
static int g_sched_on;

static void task_bootstrap(void)
{
    void (*fn)(void);

    fn = current->entry;
    if (fn)
        fn();

    fput_string("[SCHED] task '%s' returned\n",
                current->name ? current->name : "?");
    for (;;)
        __asm__ volatile("hlt");
}

void init_sched(void)
{
    g_idle.rsp = 0;
    g_idle.next = &g_idle;
    g_idle.entry = NULL;
    g_idle.name = "idle";
    g_idle.stack_page = NULL;

    current = &g_idle;
    g_sched_on = 1;

    put_string("[SCHED] idle ready\n");
}

task_t *task_create(void (*entry)(void), const char *name)
{
    task_t *t;
    uint8_t *stack;
    uintptr_t top;
    exception_frame_t *f;
    task_t *tail;

    if (!g_sched_on || entry == NULL)
        return NULL;

    /* 分配任务控制块 */
    t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL)
        return NULL;

    /* 分配任务栈 */
    stack = (uint8_t *)alloc_page();
    if (stack == NULL)
    {
        kfree(t);
        return NULL;
    }

    kernel_memset(stack, 0, (uint32_t)PAGE_SIZE);
    kernel_memset(t, 0, sizeof(*t));

    top = (uintptr_t)stack + (uintptr_t)PAGE_SIZE;
    f = (exception_frame_t *)(top - sizeof(exception_frame_t));
    kernel_memset(f, 0, sizeof(*f));

    t->entry = entry;
    t->name = name ? name : "?";
    t->stack_page = stack;

    f->rip = (uint64_t)(uintptr_t)task_bootstrap;
    f->cs = KERNEL_CODE_SEG;
    /* bit1 恒为 1；IF=1 以便新线程可被抢占 / 使用 msleep */
    f->rflags = 0x202;
    /* SysV：函数入口处 rsp % 16 == 8 */
    f->rsp = top - 8;
    f->ss = KERNEL_DATA_SEG;

    t->rsp = (uint64_t)(uintptr_t)f;

    /* 插到环尾（current 之后一圈的末尾） */
    tail = current;
    while (tail->next != current)
        tail = tail->next;
    t->next = current;
    tail->next = t;

    fput_string("[SCHED] create '%s' stack=0x%x frame=0x%x\n",
                t->name, (unsigned)(uintptr_t)stack, (unsigned)(uintptr_t)f);
    return t;
}

uint64_t schedule_from_irq(exception_frame_t *frame)
{
    if (!g_sched_on || current == NULL || frame == NULL)
        return (uint64_t)(uintptr_t)frame;

    if (current->next == current)
        return (uint64_t)(uintptr_t)frame;

    current->rsp = (uint64_t)(uintptr_t)frame;
    current = current->next;
    return current->rsp;
}

void yield(void)
{
    __asm__ volatile("int %0" : : "i"(SCHED_YIELD_VECTOR));
}

void handler_yield(exception_frame_t *frame)
{
    (void)frame;
}
