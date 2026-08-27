#include <pit.h>
#include <kernel.h>
#include <serial.h>
#include <pic.h>

#define PIT_CH0_DATA 0x40
#define PIT_CMD 0x43
#define PIT_CMD_CH0_MODE3 0x36
#define PIT_INPUT_HZ 1193182u

// volatile uint64_t jiffies;

// void handler_timer(exception_frame_t *frame)
// {
//     (void)frame;
//     jiffies++;
//     if ((jiffies % PIT_DEFAULT_HZ) == 0)
//         fput_string("tick=%llu\n", jiffies);
//     pic_eoi(0);
// }

int init_pit(uint32_t freq_hz)
{
    uint32_t divisor;

    if (freq_hz == 0)
        return -1;

    divisor = PIT_INPUT_HZ / freq_hz;
    if (divisor < 1)
        divisor = 1;
    if (divisor > 65535)
        divisor = 65535;

    outb(PIT_CMD, PIT_CMD_CH0_MODE3);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    return 0;
}