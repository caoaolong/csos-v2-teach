#include <idt.h>
#include <apic.h>
#include <serial.h>
#include <timer.h>
#include <apic.h>

static uint32_t g_timer_hz = APIC_TIMER_DEFAULT_HZ;

volatile uint64_t jiffies;

void handler_spurious(exception_frame_t *frame)
{
    (void)frame;
    /* Spurious：通常不写 EOI */
}

void handler_timer(exception_frame_t *frame)
{
    (void)frame;
    jiffies++;
    // if ((jiffies % APIC_TIMER_DEFAULT_HZ) == 0)
    //     fput_string("tick=%llu\n", jiffies);
    lapic_eoi();
}

void timer_set_hz(uint32_t hz)
{
    if (hz == 0)
        hz = APIC_TIMER_DEFAULT_HZ;
    g_timer_hz = hz;
}

uint32_t timer_hz()
{
    return g_timer_hz;
}

uint64_t jiffies_to_ms(uint64_t jf)
{
    return (jf * 1000ull) / (uint64_t)g_timer_hz;
}

uint64_t ms_to_jiffies(uint64_t ms)
{
    uint64_t jf;

    if (ms == 0)
        return 0;

    jf = (ms * (uint64_t)g_timer_hz) / 1000ull;
    /* 亚 tick 请求至少等待一个 tick，避免空转立刻返回 */
    if (jf == 0)
        jf = 1;
    return jf;
}

uint64_t uptime_ms()
{
    return jiffies_to_ms(jiffies);
}

void msleep(uint64_t ms)
{
    uint64_t target;

    if (ms == 0)
        return;

    target = jiffies + ms_to_jiffies(ms);
    while (jiffies < target)
        __asm__ volatile("hlt");
}