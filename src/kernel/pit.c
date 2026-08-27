#include <pit.h>
#include <kernel.h>
#include <serial.h>
#include <pic.h>

#define PIT_INPUT_HZ 1193182u

#define PIT_CMD 0x43
#define PIT_CH0_DATA 0x40
#define PIT_CH2_DATA 0x42
/* Channel 2，lobyte/hibyte，mode 0（中断闸门），二进制 */
#define PIT_CMD_CH2_MODE0 0xB0
#define PIT_CMD_CH0_MODE3 0x36

#define PORT_SPEAKER 0x61
#define SPEAKER_GATE_CH2 (1u << 0)
#define SPEAKER_DATA (1u << 1)
#define SPEAKER_CH2_OUT (1u << 5)

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

static uint8_t g_port61_saved;

int pit_ch2_oneshot_start(uint32_t duration_ms)
{
    uint32_t count;
    uint8_t port61;

    if (duration_ms == 0)
        return -1;

    /* count = 1193182 * ms / 1000，须落入 16 位 */
    count = (PIT_INPUT_HZ * duration_ms) / 1000u;
    if (count == 0)
        count = 1;
    if (count > 65535u)
        return -1;

    port61 = inb(PORT_SPEAKER);
    g_port61_saved = port61;
    /* 开 Ch2 gate，关扬声器数据位，避免校准期间发声 */
    outb(PORT_SPEAKER, (uint8_t)((port61 & ~SPEAKER_DATA) | SPEAKER_GATE_CH2));

    outb(PIT_CMD, PIT_CMD_CH2_MODE0);
    outb(PIT_CH2_DATA, (uint8_t)(count & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)((count >> 8) & 0xFF));
    return 0;
}

int pit_ch2_expired(void)
{
    return (inb(PORT_SPEAKER) & SPEAKER_CH2_OUT) != 0;
}

void pit_ch2_stop(void)
{
    outb(PORT_SPEAKER, g_port61_saved);
}