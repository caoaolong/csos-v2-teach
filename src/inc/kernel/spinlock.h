#ifndef CSOS_SPINLOCK_H
#define CSOS_SPINLOCK_H

#include <cpu.h>
#include <stdint.h>

typedef struct spinlock
{
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {.locked = 0}

static inline void spin_lock_init(spinlock_t *lock)
{
    lock->locked = 0;
}

static inline void spin_lock(spinlock_t *lock)
{
    for (;;)
    {
        if (__sync_bool_compare_and_swap(&lock->locked, 0, 1))
            return;
        while (lock->locked)
            cpu_pause();
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    __sync_lock_release(&lock->locked);
}

/* 先关中断再抢锁，避免同 CPU 上「持锁时被中断再抢同一锁」死锁 */
static inline uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags = irq_save();
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}

#endif /* CSOS_SPINLOCK_H */