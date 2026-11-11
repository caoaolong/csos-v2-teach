#include <kernel/sched.h>
#include <kernel/user.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/fd.h>
#include <kernel/tss.h>
#include <kernel/string.h>
#include <kernel/serial.h>
#include <kernel/gdt.h>
#include <kernel/apic.h>
#include <kernel/timer.h>
#include <kernel/spinlock.h>

/* Local APIC ID is 8 bits */
#define SCHED_MAX_CPUS 256

typedef struct cpu_runqueue
{
    task_t *head;
    task_t *tail;
    spinlock_t lock;
} cpu_runqueue_t;

static task_t g_idle[SCHED_MAX_CPUS];
static task_t *g_current[SCHED_MAX_CPUS];
static cpu_runqueue_t g_rq[SCHED_MAX_CPUS];

static int g_sched_on;
static task_t *g_sleep_head;
static spinlock_t g_sched_lock = SPINLOCK_INIT;
static unsigned g_rr_create;

/* syscall entry stack (used by syscall_entry.S; switched per current user task) */
uint64_t syscall_kstack_top;
uint64_t syscall_user_rsp;

/* PID allocation and global task list (exit finds orphans, wait finds zombies; hold g_sched_lock) */
task_t *g_init_task = NULL;
static int g_next_pid = 1;
static task_t *g_all_tasks = NULL;

/* 调用方已持锁 */
static void all_link_locked(task_t *t)
{
    t->all_next = g_all_tasks;
    g_all_tasks = t;
}

/* 调用方已持锁 */
static void all_unlink_locked(task_t *t)
{
    task_t **pp = &g_all_tasks;

    while (*pp != NULL && *pp != t)
        pp = &(*pp)->all_next;
    if (*pp != NULL)
        *pp = t->all_next;
    t->all_next = NULL;
}

static unsigned cpu_index()
{
    return (unsigned)(lapic_id() & 0xFFu);
}

task_t *sched_current()
{
    return g_current[cpu_index()];
}

static void rq_init(unsigned cpu)
{
    g_rq[cpu].head = NULL;
    g_rq[cpu].tail = NULL;
    spin_lock_init(&g_rq[cpu].lock);
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
    idle->on_cpu = 1; /* idle always counts as current on its cpu */
    idle->is_idle = 1;
    idle->pinned = 0;
    idle->cpu = 0;
}

/*
 * per-CPU runqueue FIFO.
 * Caller must hold g_rq[cpu].lock; idle never enqueues.
 */
static void ready_enqueue_locked(unsigned cpu, task_t *t)
{
    if (t == NULL || t->is_idle || t->on_ready)
        return;

    t->cpu = (uint8_t)cpu;
    t->next = NULL;
    if (g_rq[cpu].tail != NULL)
        g_rq[cpu].tail->next = t;
    else
        g_rq[cpu].head = t;
    g_rq[cpu].tail = t;
    t->on_ready = 1;
}

static void ready_enqueue(unsigned cpu, task_t *t)
{
    uint64_t flags;

    flags = spin_lock_irqsave(&g_rq[cpu].lock);
    ready_enqueue_locked(cpu, t);
    spin_unlock_irqrestore(&g_rq[cpu].lock, flags);
}

static task_t *ready_dequeue_locked(unsigned cpu)
{
    task_t *t;

    t = g_rq[cpu].head;
    if (t == NULL)
        return NULL;

    g_rq[cpu].head = t->next;
    if (g_rq[cpu].head == NULL)
        g_rq[cpu].tail = NULL;
    t->next = NULL;
    t->on_ready = 0;
    return t;
}

static task_t *ready_dequeue_local(unsigned cpu)
{
    task_t *t;
    uint64_t flags;

    flags = spin_lock_irqsave(&g_rq[cpu].lock);
    t = ready_dequeue_locked(cpu);
    spin_unlock_irqrestore(&g_rq[cpu].lock, flags);
    return t;
}

/*
 * When local queue is empty, steal one task from another online CPU.
 */
static task_t *rq_steal_one(unsigned thief)
{
    unsigned i;

    for (i = 1; i < SCHED_MAX_CPUS; i++)
    {
        unsigned victim = (thief + i) % SCHED_MAX_CPUS;
        task_t *t;
        uint64_t flags;

        if (g_current[victim] == NULL)
            continue;

        flags = spin_lock_irqsave(&g_rq[victim].lock);
        if (g_rq[victim].head != NULL && g_rq[victim].head->pinned)
        {
            spin_unlock_irqrestore(&g_rq[victim].lock, flags);
            continue;
        }
        t = ready_dequeue_locked(victim);
        spin_unlock_irqrestore(&g_rq[victim].lock, flags);
        if (t != NULL)
            return t;
    }
    return NULL;
}

static unsigned pick_create_cpu()
{
    unsigned start;
    unsigned i;

    start = __sync_fetch_and_add(&g_rr_create, 1);
    for (i = 0; i < SCHED_MAX_CPUS; i++)
    {
        unsigned cpu = (start + i) % SCHED_MAX_CPUS;

        if (g_current[cpu] != NULL)
            return cpu;
    }
    return cpu_index();
}

static void sleep_enqueue(task_t *t)
{
    t->sleep_next = g_sleep_head;
    g_sleep_head = t;
}

static void task_bootstrap()
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

void init_sched()
{
    unsigned cpu = cpu_index();
    unsigned i;

    for (i = 0; i < SCHED_MAX_CPUS; i++)
    {
        g_current[i] = NULL;
        rq_init(i);
    }

    g_sleep_head = NULL;
    g_rr_create = 0;

    idle_init(&g_idle[cpu]);
    g_current[cpu] = &g_idle[cpu];
    g_sched_on = 1;
    spin_lock_init(&g_sched_lock);

    put_string("[SCHED] per-CPU runqueue ready\n");
}

void sched_cpu_init()
{
    unsigned cpu = cpu_index();

    idle_init(&g_idle[cpu]);
    g_current[cpu] = &g_idle[cpu];
}

task_t *task_create_ex(void (*entry)(void), const char *name, int pinned)
{
    task_t *t;
    uint8_t *stack;
    uintptr_t top;
    exception_frame_t *f;
    unsigned target;

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
    t->pinned = pinned ? 1 : 0;
    t->pid = __sync_fetch_and_add(&g_next_pid, 1);
    t->parent = sched_current();
    t->pgdir = NULL;
    t->kstack_page = NULL;
    t->kstack_top = 0;

    f->rip = (uint64_t)(uintptr_t)task_bootstrap;
    f->cs = KERNEL_CODE_SEG;
    f->rflags = 0x202;
    f->rsp = top - 8;
    f->ss = KERNEL_DATA_SEG;

    t->rsp = (uint64_t)(uintptr_t)f;

    target = t->pinned ? cpu_index() : pick_create_cpu();
    t->cpu = (uint8_t)target;
    {
        uint64_t flags = spin_lock_irqsave(&g_sched_lock);

        all_link_locked(t);
        spin_unlock_irqrestore(&g_sched_lock, flags);
    }
    ready_enqueue(target, t);

    return t;
}

task_t *task_create(void (*entry)(void), const char *name)
{
    return task_create_ex(entry, name, 0);
}

task_t *task_create_pinned(void (*entry)(void), const char *name)
{
    return task_create_ex(entry, name, 1);
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
            /* still on its cpu: only change state, requeue on next schedule */
            if (!t->on_cpu)
                ready_enqueue((unsigned)t->cpu, t);
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
    /* running task is never on the ready queue */
    cur->wake_jiffies = jiffies + jf;
    cur->state = TASK_SLEEPING;
    sleep_enqueue(cur);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    /* must yield after releasing the lock, or we self-deadlock */
    yield();
}

void sched_block()
{
    uint64_t flags;
    task_t *cur;

    cur = sched_current();
    if (cur == NULL || cur->is_idle)
        return;

    flags = spin_lock_irqsave(&g_sched_lock);
    cur->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_unblock(task_t *t)
{
    uint64_t flags;

    if (t == NULL || t->is_idle)
        return;

    flags = spin_lock_irqsave(&g_sched_lock);
    if (t->state == TASK_BLOCKED)
    {
        t->state = TASK_READY;
        /* still running on some cpu: only change state, requeue on next schedule */
        if (!t->on_cpu)
            ready_enqueue((unsigned)t->cpu, t);
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

uint64_t schedule_from_irq(exception_frame_t *frame)
{
    unsigned cpu;
    task_t *cur;
    task_t *next;
    uint64_t irqf;
    uint64_t next_rsp;

    if (!g_sched_on || frame == NULL)
        return (uint64_t)(uintptr_t)frame;

    cpu = cpu_index();
    cur = g_current[cpu];
    if (cur == NULL)
        return (uint64_t)(uintptr_t)frame;

    irqf = irq_save();
    cur->rsp = (uint64_t)(uintptr_t)frame;

    if (!cur->is_idle)
    {
        uint64_t sflags;
        int requeue;

        sflags = spin_lock_irqsave(&g_sched_lock);
        cur->on_cpu = 0;
        requeue = (cur->state == TASK_READY);
        spin_unlock_irqrestore(&g_sched_lock, sflags);
        if (requeue)
            ready_enqueue(cpu, cur);
    }

    next = ready_dequeue_local(cpu);
    if (next == NULL)
        next = rq_steal_one(cpu);

    if (next == NULL)
        next = &g_idle[cpu];
    else
    {
        next->on_cpu = 1;
        next->cpu = (uint8_t)cpu;
    }


    /* Switch address space and entry stack with task: kernel tasks (pgdir NULL) use master;
     * kernel stack/TSS only maintained for user tasks. */
    {
        uint64_t *root = next->pgdir != NULL ? next->pgdir : vmm_master_pml4();

        vmm_switch_active(root);
        if (next->kstack_top != 0)
        {
            syscall_kstack_top = next->kstack_top;
            tss_set_rsp0(next->kstack_top);
        }
    }

    g_current[cpu] = next;
    next_rsp = next->rsp;
    irq_restore(irqf);
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

/* fork child: copy of parent trap frame on kstack top (child rax=0), enters user via iret.
 * Not enqueued: caller fills pgdir/fd then task_enqueue. NULL on failure. */
task_t *task_alloc_fork(exception_frame_t *frame)
{
    task_t *parent = sched_current();
    task_t *t;
    uint8_t *kstack;
    exception_frame_t *f;

    if (parent == NULL || parent->pgdir == NULL || frame == NULL)
        return NULL;
    t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL)
        return NULL;
    kstack = (uint8_t *)alloc_pages(USER_KSTACK_PAGES);
    if (kstack == NULL)
    {
        kfree(t);
        return NULL;
    }
    kernel_memset(kstack, 0, (uint32_t)USER_KSTACK_SIZE);
    kernel_memset(t, 0, sizeof(*t));
    f = (exception_frame_t *)(kstack + USER_KSTACK_SIZE - sizeof(*f));
    *f = *frame;
    f->rax = 0; /* child fork returns 0 */
    t->rsp = (uint64_t)(uintptr_t)f;
    t->name = parent->name;
    t->kstack_page = kstack;
    t->kstack_top = (uint64_t)(uintptr_t)kstack + USER_KSTACK_SIZE;
    t->pid = __sync_fetch_and_add(&g_next_pid, 1);
    t->parent = parent;
    t->state = TASK_READY;
    t->pinned = parent->pinned;
    t->cpu = parent->cpu;
    return t;
}

void task_abort_fork(task_t *t)
{
    if (t == NULL)
        return;
    if (t->kstack_page != NULL)
        free_pages((uint64_t)(uintptr_t)t->kstack_page, USER_KSTACK_PAGES);
    kfree(t);
}

void task_enqueue(task_t *t)
{
    uint64_t flags;

    if (t == NULL)
        return;
    flags = spin_lock_irqsave(&g_sched_lock);
    all_link_locked(t);
    spin_unlock_irqrestore(&g_sched_lock, flags);
    ready_enqueue((unsigned)t->cpu, t);
}

/* Release all private resources of a zombie (caller already unlinked it, not holding lock) */
static void task_destroy(task_t *z)
{
    if (z->pgdir != NULL)
        vmm_free_user_pml4(z->pgdir, USER_IMAGE_BASE, USER_IMAGE_END);
    fd_table_cleanup(z);
    if (z->kstack_page != NULL)
        free_pages((uint64_t)(uintptr_t)z->kstack_page, USER_KSTACK_PAGES);
    kfree(z);
}

void task_exit_current(int code)
{
    task_t *cur = sched_current();
    task_t *parent;
    task_t *it;
    task_t *wake = NULL;
    uint64_t flags;

    if (cur == NULL || cur->is_idle)
        for (;;)
            __asm__ volatile("hlt");
    flags = spin_lock_irqsave(&g_sched_lock);
    cur->state = TASK_ZOMBIE;
    cur->exit_code = code;
    /* orphans are reparented to init (NULL if init itself exits) */
    for (it = g_all_tasks; it != NULL; it = it->all_next)
        if (it->parent == cur)
            it->parent = (cur == g_init_task) ? NULL : g_init_task;
    parent = cur->parent;
    if (parent != NULL && parent->wait_child)
    {
        parent->wait_child = 0;
        wake = parent;
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);

    fput_string("[USER] task '%s' pid=%d exit(%d)\n",
                cur->name != NULL ? cur->name : "?",
                cur->pid, code);
    if (wake != NULL)
        sched_unblock(wake);
    /* zombie never requeues; reaped by parent wait after switching away */
    for (;;)
        yield();
}

int sched_wait_reap(int *pid_out, int *code_out)
{
    task_t *cur = sched_current();
    task_t *prev = NULL;
    task_t *it;
    task_t *z = NULL;
    int has_child = 0;
    uint64_t flags;

    if (cur == NULL || pid_out == NULL || code_out == NULL)
        return -1;
    flags = spin_lock_irqsave(&g_sched_lock);
    for (it = g_all_tasks; it != NULL; prev = it, it = it->all_next)
    {
        if (it->parent != cur)
            continue;
        has_child = 1;
        if (it->state == TASK_ZOMBIE)
        {
            z = it;
            if (prev != NULL)
                prev->all_next = it->all_next;
            else
                g_all_tasks = it->all_next;
            z->all_next = NULL;
            break;
        }
    }
    if (z == NULL)
    {
        if (!has_child)
        {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return -1;
        }
        cur->wait_child = 1;
        cur->state = TASK_BLOCKED;
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return 0;
    }
    cur->wait_child = 0;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    *pid_out = z->pid;
    *code_out = z->exit_code;
    task_destroy(z);
    return *pid_out;
}

void sched_set_init_task(task_t *t)
{
    uint64_t flags;

    if (t == NULL)
        return;
    flags = spin_lock_irqsave(&g_sched_lock);
    g_init_task = t;
    t->pid = 1;
    if (g_next_pid <= 1)
        g_next_pid = 2;
    spin_unlock_irqrestore(&g_sched_lock, flags);
}
