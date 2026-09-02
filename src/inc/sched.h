#ifndef CSOS_SCHED_H
#define CSOS_SCHED_H

#include <idt.h>
#include <stdint.h>

/* 协作式 yield：软件中断，与 timer 共用 exception_frame 切换路径 */
#define SCHED_YIELD_VECTOR 34

typedef struct task
{
    uint64_t rsp;        /* 指向栈上 exception_frame_t */
    struct task *next;   /* 环形就绪队列 */
    void (*entry)(void); /* 入口（仅新建时使用） */
    const char *name;
    void *stack_page; /* alloc_page；idle 为 NULL */
} task_t;

extern task_t *current;

/* 将当前引导栈登记为 idle，之后可 create / sti */
void init_sched(void);

/* 新建内核线程并链入就绪环；失败返回 NULL */
task_t *task_create(void (*entry)(void), const char *name);

/*
 * 中断路径调度：保存 frame 到 current，切到 next，返回新 rsp。
 * 若调度未启用或仅一任务，返回原 frame。
 */
uint64_t schedule_from_irq(exception_frame_t *frame);

/* 协作让出（int SCHED_YIELD_VECTOR） */
void yield();

/* yield 向量：仅调度；返回值由 interrupt.S 经 schedule_from_irq 取得 */
void handler_yield(exception_frame_t *frame);

#endif /* CSOS_SCHED_H */
