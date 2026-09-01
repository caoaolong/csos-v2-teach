#include <kbd.h>
#include <apic.h>
#include <apic/ioapic.h>
#include <kernel.h>
#include <serial.h>

#define KBD_DATA 0x60
#define KBD_STATUS 0x64
#define KBD_CMD 0x64

#define KBD_STAT_OBF (1u << 0) /* output buffer full */
#define KBD_STAT_IBF (1u << 1) /* input buffer full */

#define KBD_CMD_DISABLE_PORT1 0xAD
#define KBD_CMD_DISABLE_PORT2 0xA7
#define KBD_CMD_ENABLE_PORT1 0xAE
#define KBD_CMD_READ_CFG 0x20
#define KBD_CMD_WRITE_CFG 0x60

/* Set1 make → ASCII；0 表示忽略（含 break 由调用方过滤） */
static const char g_set1_map[128] = {
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0E] = '\b',
    [0x1C] = '\n',
    [0x39] = ' ',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
};

static void kbd_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

static void kbd_wait_ibf_clear(void)
{
    int spins = 100000;
    while ((inb(KBD_STATUS) & KBD_STAT_IBF) != 0)
    {
        if (--spins == 0)
            kbd_halt("FATAL: 8042 input buffer stuck\n");
    }
}

static void kbd_wait_obf_set(void)
{
    int spins = 100000;
    while ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
    {
        if (--spins == 0)
            kbd_halt("FATAL: 8042 output buffer empty\n");
    }
}

static void kbd_flush_output(void)
{
    int i;
    for (i = 0; i < 100; i++)
    {
        if ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
            break;
        (void)inb(KBD_DATA);
    }
}

static void kbd_write_cmd(uint8_t cmd)
{
    kbd_wait_ibf_clear();
    outb(KBD_CMD, cmd);
}

static void kbd_write_data(uint8_t data)
{
    kbd_wait_ibf_clear();
    outb(KBD_DATA, data);
}

static uint8_t kbd_read_data(void)
{
    kbd_wait_obf_set();
    return inb(KBD_DATA);
}

void handler_kbd(exception_frame_t *frame)
{
    uint8_t sc;
    char ch;
    char buf[2];

    (void)frame;

    if ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
    {
        lapic_eoi();
        return;
    }

    sc = inb(KBD_DATA);

    /* 忽略 break（高位置位）与 E0 前缀等扩展，第一版只处理 set1 make */
    if ((sc & 0x80) == 0 && sc < 128)
    {
        ch = g_set1_map[sc];
        if (ch != 0)
        {
            buf[0] = ch;
            buf[1] = '\0';
            put_string(buf);
        }
    }

    lapic_eoi();
}

void init_kbd(void)
{
    uint8_t cfg;

    /* 关闭两端，排空，再开第一端口并启用 IRQ */
    kbd_write_cmd(KBD_CMD_DISABLE_PORT1);
    kbd_write_cmd(KBD_CMD_DISABLE_PORT2);
    kbd_flush_output();

    kbd_write_cmd(KBD_CMD_READ_CFG);
    cfg = kbd_read_data();
    /* bit0：第一端口中断；bit1：第二端口中断；bit6：XT 翻译（保持以便得到 set1） */
    cfg |= (1u << 0);
    cfg &= (uint8_t)~(1u << 1);
    kbd_write_cmd(KBD_CMD_WRITE_CFG);
    kbd_write_data(cfg);

    kbd_write_cmd(KBD_CMD_ENABLE_PORT1);
    kbd_flush_output();

    ioapic_route_isa_irq(KBD_ISA_IRQ, KBD_VECTOR, lapic_id());

    fput_string("[KBD] ready IRQ%u vector=%u cfg=0x%x\n",
                (unsigned)KBD_ISA_IRQ, (unsigned)KBD_VECTOR, (unsigned)cfg);
}
