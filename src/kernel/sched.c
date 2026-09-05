#include <sched.h>
#include <memory/heap.h>
#include <memory/pmm.h>
#include <string.h>
#include <serial.h>
#include <gdt.h>
#include <apic.h>
#include <timer.h>
#include <spinlock.h>

static spinlock_t g_sched_lock = SPINLOCK_INIT;

static task_t *g_sleep_head;

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

static void ready_enqueue(task_t *t)
{
    task_t *tail;

    if (t == NULL || t == &g_idle)
        return;
    if (t->next != t)
        return;

    tail = &g_idle;
    while (tail->next != &g_idle)
        tail = tail->next;
    t->next = &g_idle;
    tail->next = t;
}

static void ready_unlink(task_t *t)
{
    task_t *prev;

    if (t == NULL || t == &g_idle)
        return;
    if (t->next == t)
        return;

    prev = t;
    while (prev->next != t)
        prev = prev->next;
    prev->next = t->next;
    t->next = t;
}

static void sleep_enqueue(task_t *t)
{
    t->sleep_next = g_sleep_head;
    g_sleep_head = t;
}

void init_sched()
{
    g_idle.rsp = 0;
    g_idle.next = &g_idle;
    g_idle.sleep_next = NULL;
    g_idle.entry = NULL;
    g_idle.name = "idle";
    g_idle.stack_page = NULL;
    g_idle.state = TASK_READY;
    g_idle.wake_jiffies = 0;

    g_sleep_head = NULL;
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

    if (!g_sched_on || entry == NULL)
        return NULL;

    t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL)
        return NULL;

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
    t->state = TASK_READY;
    t->sleep_next = NULL;
    t->next = t;

    f->rip = (uint64_t)(uintptr_t)task_bootstrap;
    f->cs = KERNEL_CODE_SEG;
    f->rflags = 0x202;
    f->rsp = top - 8;
    f->ss = KERNEL_DATA_SEG;

    t->rsp = (uint64_t)(uintptr_t)f;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    ready_enqueue(t);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    fput_string("[SCHED] create '%s' stack=0x%x frame=0x%x\n",
                t->name, (unsigned)(uintptr_t)stack, (unsigned)(uintptr_t)f);
    return t;
}

uint64_t schedule_from_irq(exception_frame_t *frame)
{
    task_t *next;
    uint64_t flags;
    uint64_t next_rsp;

    if (!g_sched_on || current == NULL || frame == NULL)
        return (uint64_t)(uintptr_t)frame;

    flags = spin_lock_irqsave(&g_sched_lock);
    current->rsp = (uint64_t)(uintptr_t)frame;

    if (current->state == TASK_SLEEPING || current->next == current)
    {
        /* 已离开就绪环：下一个为 idle 之后的任务（或 idle 自己） */
        next = g_idle.next;
    }
    else
    {
        next = current->next;
    }

    next_rsp = next->rsp;
    current = next;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return next_rsp;
}

void yield(void)
{
    __asm__ volatile("int %0" : : "i"(SCHED_YIELD_VECTOR));
}

void handler_yield(exception_frame_t *frame)
{
    (void)frame;
}

void sched_wake_sleepers(void)
{
    task_t **pp;
    task_t *t;
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);

    pp = &g_sleep_head;
    while (*pp != NULL)
    {
        t = *pp;
        if (jiffies >= t->wake_jiffies)
        {
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->state = TASK_READY;
            ready_enqueue(t);
        }
        else
        {
            pp = &t->sleep_next;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_sleep_jiffies(uint64_t jf)
{
    if (jf == 0 || !g_sched_on || current == NULL)
        return;

    if (current == &g_idle)
        return;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    current->wake_jiffies = jiffies + jf;
    current->state = TASK_SLEEPING;
    ready_unlink(current);
    sleep_enqueue(current);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    yield();
}