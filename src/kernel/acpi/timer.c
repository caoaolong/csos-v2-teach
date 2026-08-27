#include <idt.h>
#include <apic.h>
#include <serial.h>

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
    if ((jiffies % APIC_TIMER_DEFAULT_HZ) == 0)
        fput_string("tick=%llu\n", jiffies);
    lapic_eoi();
}