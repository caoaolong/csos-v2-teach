#ifndef CSOS_SCHED_H
#define CSOS_SCHED_H

#include <idt.h>
#include <stdint.h>

/* 协作式 yield：软件中断，与 timer 共用 exception_frame 切换路径 */
#define SCHED_YIELD_VECTOR 34

typedef enum task_state
{
    TASK_READY = 0,
    TASK_SLEEPING,
} task_state_t;

typedef struct task
{
    uint64_t rsp;            /* 指向栈上 exception_frame_t */
    struct task *next;       /* per-CPU 就绪队列（仅 READY 且未运行） */
    struct task *sleep_next; /* 睡眠链表 */
    void (*entry)(void);     /* 入口（仅新建时使用） */
    const char *name;
    void *stack_page; /* alloc_page；idle 为 NULL */
    task_state_t state;
    uint64_t wake_jiffies; /* state==SLEEPING 时的唤醒时刻 */
    uint8_t on_ready;      /* 是否挂在 per-CPU 就绪队列上 */
    uint8_t on_cpu;        /* 是否正被某核执行（未 schedule 离开） */
    uint8_t is_idle;       /* per-CPU idle，不进就绪队列 */
    uint8_t pinned;        /* 绑定 CPU：不允许被其他核窃取（用户任务用） */
    uint8_t cpu;           /* 所属就绪队列的 apic_id（最后运行核） */
} task_t;

/* 当前 CPU 正在运行的任务（per-CPU） */
task_t *sched_current();
#define current (sched_current())

void sched_cpu_init();

extern task_t *current;

void init_sched();

void sched_cpu_init();

task_t *task_create(void (*entry)(void), const char *name);

task_t *task_create_pinned(void (*entry)(void), const char *name);

void sched_wake_sleepers(void);

void sched_sleep_jiffies(uint64_t jf);

uint64_t schedule_from_irq(exception_frame_t *frame);

void yield();

void handler_yield(exception_frame_t *frame);

#endif /* CSOS_SCHED_H */
