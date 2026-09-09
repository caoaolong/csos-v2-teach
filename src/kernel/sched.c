#include <sched.h>
#include <memory/heap.h>
#include <memory/pmm.h>
#include <string.h>
#include <serial.h>
#include <gdt.h>
#include <apic.h>
#include <timer.h>
#include <spinlock.h>

/* Local APIC ID 为 8 位 */
#define SCHED_MAX_CPUS 256

static task_t g_idle[SCHED_MAX_CPUS];
static task_t *g_current[SCHED_MAX_CPUS];

static spinlock_t g_sched_lock = SPINLOCK_INIT;

static task_t *g_sleep_head;

/* 全局就绪队列：仅可运行的 worker；FIFO */
static task_t *g_rq_head;
static task_t *g_rq_tail;

// task_t *current;

// static task_t g_idle;
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
static unsigned cpu_index(void)
{
    return (unsigned)(lapic_id() & 0xFFu);
}

task_t *sched_current(void)
{
    return g_current[cpu_index()];
}

static void idle_init(task_t *idle)
{
    idle->rsp = 0;
    idle->next = NULL;
    idle->sleep_next = NULL;
    idle->entry = NULL;
    idle->name = "idle";
    idle->stack_page = NULL;
    idle->state = TASK_READY;
    idle->wake_jiffies = 0;
    idle->on_ready = 0;
    idle->on_cpu = 1; /* idle 始终可视为本核 current */
    idle->is_idle = 1;
}

/*
 * 全局就绪队列 FIFO。
 * 调用方须已持有 g_sched_lock；idle 不得入队。
 */
static void ready_enqueue(task_t *t)
{
    if (t == NULL || t->is_idle || t->on_ready)
        return;

    t->next = NULL;
    if (g_rq_tail != NULL)
        g_rq_tail->next = t;
    else
        g_rq_head = t;
    g_rq_tail = t;
    t->on_ready = 1;
}

static task_t *ready_dequeue(void)
{
    task_t *t;

    t = g_rq_head;
    if (t == NULL)
        return NULL;

    g_rq_head = t->next;
    if (g_rq_head == NULL)
        g_rq_tail = NULL;
    t->next = NULL;
    t->on_ready = 0;
    return t;
}

static void sleep_enqueue(task_t *t)
{
    t->sleep_next = g_sleep_head;
    g_sleep_head = t;
}

void init_sched(void)
{
    unsigned cpu = cpu_index();
    unsigned i;

    for (i = 0; i < SCHED_MAX_CPUS; i++)
        g_current[i] = NULL;

    g_rq_head = NULL;
    g_rq_tail = NULL;
    g_sleep_head = NULL;

    idle_init(&g_idle[cpu]);
    g_current[cpu] = &g_idle[cpu];
    g_sched_on = 1;
    spin_lock_init(&g_sched_lock);

    put_string("[SCHED] idle ready (SMP)\n");
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
    t->next = NULL;
    t->on_ready = 0;
    t->on_cpu = 0;
    t->is_idle = 0;

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

void sched_wake_sleepers()
{
    task_t **pp;
    task_t *t;
    uint64_t flags;

    flags = spin_lock_irqsave(&g_sched_lock);
    pp = &g_sleep_head;
    while (*pp != NULL)
    {
        t = *pp;
        if (jiffies >= t->wake_jiffies)
        {
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->state = TASK_READY;
            /* 新增：仍在原核 current 上时只改状态，由该核 schedule 时再入队 */
            if (!t->on_cpu)
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
    uint64_t flags;
    task_t *cur;

    if (jf == 0 || !g_sched_on)
        return;

    cur = sched_current();
    if (cur == NULL || cur->is_idle)
        return;

    flags = spin_lock_irqsave(&g_sched_lock);
    /* 运行中的任务本就不在就绪队列上 */
    cur->wake_jiffies = jiffies + jf;
    cur->state = TASK_SLEEPING;
    sleep_enqueue(cur);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    /* 必须在释放锁之后再 yield，否则持锁切走会自锁 */
    yield();
}

uint64_t schedule_from_irq(exception_frame_t *frame)
{
    unsigned cpu;
    task_t *cur;
    task_t *next;
    uint64_t flags;
    uint64_t next_rsp;

    if (!g_sched_on || frame == NULL)
        return (uint64_t)(uintptr_t)frame;

    cpu = cpu_index();
    cur = g_current[cpu];
    if (cur == NULL)
        return (uint64_t)(uintptr_t)frame;

    flags = spin_lock_irqsave(&g_sched_lock);
    cur->rsp = (uint64_t)(uintptr_t)frame;

    /* 离开本核：worker 仍 READY 则交回全局队列；idle / SLEEPING 不入队 */
    if (!cur->is_idle)
    {
        cur->on_cpu = 0;
        if (cur->state == TASK_READY)
            ready_enqueue(cur);
    }

    next = ready_dequeue();
    if (next == NULL)
        next = &g_idle[cpu];
    else
        next->on_cpu = 1;

    g_current[cpu] = next;
    next_rsp = next->rsp;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return next_rsp;
}

void yield()
{
    __asm__ volatile("int %0" : : "i"(SCHED_YIELD_VECTOR));
}

void handler_yield(exception_frame_t *frame)
{
    (void)frame;
}

void sched_cpu_init()
{
    unsigned cpu = cpu_index();

    idle_init(&g_idle[cpu]);
    g_current[cpu] = &g_idle[cpu];
}