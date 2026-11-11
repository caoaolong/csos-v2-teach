#ifndef CSOS_PIT_H
#define CSOS_PIT_H

#include <stdint.h>

#define PIT_DEFAULT_HZ 100
#define PIT_INPUT_HZ 1193182u
#define PIT_CALIBRATE_MS 10u

/* 成功返回 0；freq_hz==0 返回 -1 且不写端口。时钟默认走 APIC Timer。 */
int init_pit(uint32_t freq_hz);

/*
 * 启动 Channel 2 mode0 one-shot，用于校准。
 * 打开 gate、关闭扬声器；duration_ms==0 或 count>65535 返回 -1。
 */
int pit_ch2_oneshot_start(uint32_t duration_ms);

/* port 0x61 bit5：Ch2 OUT 已置位则返回非 0 */
int pit_ch2_expired(void);

/* 恢复校准前的 0x61，关闭 Ch2 gate */
void pit_ch2_stop(void);

#endif /* CSOS_PIT_H */
