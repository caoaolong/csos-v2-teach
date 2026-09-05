#ifndef CSOS_TIMER_H
#define CSOS_TIMER_H

#include <stdint.h>

extern volatile uint64_t jiffies;

/* 由 init_apic_timer 写入实际 Hz；换算与 sleep 依赖此值 */
void timer_set_hz(uint32_t hz);
uint32_t timer_hz();

uint64_t jiffies_to_ms(uint64_t jf);
uint64_t ms_to_jiffies(uint64_t ms);

/* 自开机（定时器启动并开始累计 jiffies）以来的毫秒数 */
uint64_t uptime_ms();

/*
 * 睡眠至少 ms 毫秒（向上对齐到 tick）。
 * 要求中断已开启；ms==0 立即返回。
 */
void msleep(uint64_t ms);

#endif /* CSOS_TIMER_H */
