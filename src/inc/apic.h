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

/* 周期模式：divide=/16，vector，initial count */
void lapic_timer_start(uint32_t init_count, uint8_t vector);

/* 配置周期 APIC Timer；freq_hz==0 失败并 hlt */
void init_apic_timer(uint32_t freq_hz);

#endif /*CSOS_APIC_H*/