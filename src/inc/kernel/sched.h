#ifndef CSOS_SCHED_H
#define CSOS_SCHED_H

#include <kernel/idt.h>
#include <stdint.h>
#include <kernel/fd.h>

/* 协作式 yield：软件中断，与 timer 共用 exception_frame 切换路径 */
#define SCHED_YIELD_VECTOR 34

/* 用户任务内核栈：2 页连续（8KB）。exec 的 VFS/ATA 调用链实测约 6KB，
 * 单页（4KB）会下溢进相邻任务的 kstack，表现为 wake 时 iret 帧被砸坏。
 * TODO: 加 guard 页 / 栈水位检查。 */
#define USER_KSTACK_PAGES 2
#define USER_KSTACK_SIZE (USER_KSTACK_PAGES * 4096ULL)

typedef enum task_state
{
    TASK_READY = 0,
    TASK_SLEEPING,
    TASK_BLOCKED, /* 等待外部事件（如键盘），由事件源显式唤醒 */
    TASK_ZOMBIE,  /* 已退出，待父进程 wait 回收 */
} task_state_t;

typedef struct task
{
    uint64_t rsp;            /* 指向栈上 exception_frame_t */
    struct task *next;       /* per-CPU 就绪队列（仅 READY 且未运行） */
    struct task *sleep_next; /* 睡眠链表 */
    struct task *all_next;   /* 全局任务链表（exit 找孤儿、wait 找僵尸） */
    void (*entry)(void);     /* 入口（仅新建时使用） */
    const char *name;
    void *stack_page; /* 内核任务的内核栈；用户任务转用 kstack_page 后闲置 */
    int pid;          /* 0=idle，1=init 起 */
    struct task *parent;     /* fork 父任务；init/内核任务为 NULL */
    uint64_t *pgdir;         /* 用户地址空间顶级表；内核任务为 NULL（用 master） */
    uint64_t heap_base;      /* 用户堆下界（映像+固定栈页之上，按页对齐，只增不减直到 exec） */
    uint64_t heap_brk;       /* 用户堆当前顶；已映射 == [heap_base, ALIGN_UP(heap_brk)) */
    void *kstack_page;       /* 用户任务的内核栈页（含 trap 帧） */
    uint64_t kstack_top;
    int exit_code;  /* ZOMBIE 时有效 */
    int wait_child; /* wait 中阻塞，子退出时被唤醒 */
    task_state_t state;
    uint64_t wake_jiffies; /* state==SLEEPING 时的唤醒时刻 */
    uint8_t on_ready;      /* 是否挂在 per-CPU 就绪队列上 */
    uint8_t on_cpu;        /* 是否正被某核执行（未 schedule 离开） */
    uint8_t is_idle;       /* per-CPU idle，不进就绪队列 */
    uint8_t pinned;        /* 绑定 CPU：不允许被其他核窃取（用户任务用） */
    uint8_t cpu;           /* 所属就绪队列的 apic_id（最后运行核） */
    fd_entry_t *fds[FD_MAX]; /* 本任务 fd 表（NULL=空闲；共享描述引用计数） */
} task_t;

/* 当前 CPU 正在运行的任务（per-CPU） */
task_t *sched_current();
#define current (sched_current())

/* syscall 入口栈顶 / 用户栈暂存（syscall_entry.S 用；调度器按当前用户任务维护前者） */
extern uint64_t syscall_kstack_top;
extern uint64_t syscall_user_rsp;

/* PID=1 的 init 任务：孤儿的 reparent 目标 */
extern task_t *g_init_task;

void sched_cpu_init();

extern task_t *current;

void init_sched();

void sched_cpu_init();

task_t *task_create(void (*entry)(void), const char *name);

task_t *task_create_pinned(void (*entry)(void), const char *name);

void sched_wake_sleepers();

void sched_sleep_jiffies(uint64_t jf);

void sched_block();

void sched_unblock(task_t *t);

/* fork 子任务：复制父 trap 帧（子 rax=0），未入队，pgdir/fd 由调用方补齐 */
task_t *task_alloc_fork(exception_frame_t *frame);

/* fork 中途失败回滚（未入队）：释放 kstack + task */
void task_abort_fork(task_t *t);

/* 入队（首次调度） */
void task_enqueue(task_t *t);

/* 当前任务退出为僵尸，唤醒等待的父任务，不返回 */
void task_exit_current(int code) __attribute__((noreturn));

/* 回收一个已退出子任务：>0=pid，0=有子但无僵尸（调用方 yield 重试），-1=无子 */
int sched_wait_reap(int *pid_out, int *code_out);

/* 非阻塞版：只收割已存在的僵尸，不改调用方状态；>0=pid，0=暂无僵尸，-1=无子 */
int sched_wait_reap_nb(int *pid_out, int *code_out);

/* 标记 init 任务（pid 固定 1，孤儿的 reparent 目标） */
void sched_set_init_task(task_t *t);

uint64_t schedule_from_irq(exception_frame_t *frame);

void yield();

void handler_yield(exception_frame_t *frame);

#endif /* CSOS_SCHED_H */
