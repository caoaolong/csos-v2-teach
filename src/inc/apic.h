#ifndef CSOS_APIC_H
#define CSOS_APIC_H

#include <stdint.h>
#include <acpi.h>

#define APIC_TIMER_VECTOR 32
#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_TIMER_DEFAULT_HZ 100

/* QEMU 默认 APIC bus ≈ 1GHz；divide=16、100Hz → 625000（未校准写死） */
#define APIC_TIMER_QEMU_INIT_COUNT 625000u

void init_lapic();

void lapic_eoi();

/* 当前 CPU 的 Local APIC ID（物理模式 destination） */
uint32_t lapic_id();

/* 周期模式：divide=/16，vector，initial count */
void lapic_timer_start(uint32_t init_count, uint8_t vector);

/* 停定时器（LVT masked，INIT_COUNT=0） */
void lapic_timer_stop(void);

/* 校准用：divide=/16，LVT masked，INIT_COUNT=0xFFFFFFFF 单向倒计时 */
void lapic_timer_calib_start(void);

/* 读 CUR_COUNT */
uint32_t lapic_timer_current(void);

/* 配置周期 APIC Timer；freq_hz==0 失败并 hlt */
void init_apic_timer(uint32_t freq_hz);

#endif /*CSOS_APIC_H*/