#include <kernel/kbd.h>
#include <kernel/apic.h>
#include <kernel/apic/ioapic.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vdev.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <input.h>

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

/* 输入事件环：中断写入 input_event_t，vdev read 读出；
 * 静态存储，中断使能前即可使用。每次按键产生 KEY+SYN 两个事件。 */
#define KBD_EVENT_BUF 256
static input_event_t g_kbd_buf[KBD_EVENT_BUF];
static uint32_t g_kbd_head;
static uint32_t g_kbd_tail;
static uint32_t g_kbd_count;
static spinlock_t g_kbd_lock = SPINLOCK_INIT;
static task_t *g_kbd_waiter = NULL;

/* E0 扩展前缀状态（中断上下文推进）：1=下一字节为扩展码 */
static int g_kbd_e0;

/* Set1 make 码 → Linux KEY_*；0 表示忽略 */
static const uint16_t g_set1_keycode[128] = {
    [0x01] = INPUT_KEY_ESC,
    [0x02] = INPUT_KEY_1,
    [0x03] = INPUT_KEY_2,
    [0x04] = INPUT_KEY_3,
    [0x05] = INPUT_KEY_4,
    [0x06] = INPUT_KEY_5,
    [0x07] = INPUT_KEY_6,
    [0x08] = INPUT_KEY_7,
    [0x09] = INPUT_KEY_8,
    [0x0A] = INPUT_KEY_9,
    [0x0B] = INPUT_KEY_0,
    [0x0E] = INPUT_KEY_BACKSPACE,
    [0x0F] = INPUT_KEY_TAB,
    [0x1C] = INPUT_KEY_ENTER,
    [0x1D] = INPUT_KEY_LEFTCTRL,
    [0x39] = INPUT_KEY_SPACE,
    [0x10] = INPUT_KEY_Q,
    [0x11] = INPUT_KEY_W,
    [0x12] = INPUT_KEY_E,
    [0x13] = INPUT_KEY_R,
    [0x14] = INPUT_KEY_T,
    [0x15] = INPUT_KEY_Y,
    [0x16] = INPUT_KEY_U,
    [0x17] = INPUT_KEY_I,
    [0x18] = INPUT_KEY_O,
    [0x19] = INPUT_KEY_P,
    [0x1E] = INPUT_KEY_A,
    [0x1F] = INPUT_KEY_S,
    [0x20] = INPUT_KEY_D,
    [0x21] = INPUT_KEY_F,
    [0x22] = INPUT_KEY_G,
    [0x23] = INPUT_KEY_H,
    [0x24] = INPUT_KEY_J,
    [0x25] = INPUT_KEY_K,
    [0x26] = INPUT_KEY_L,
    [0x2A] = INPUT_KEY_LEFTSHIFT,
    [0x2C] = INPUT_KEY_Z,
    [0x2D] = INPUT_KEY_X,
    [0x2E] = INPUT_KEY_C,
    [0x2F] = INPUT_KEY_V,
    [0x30] = INPUT_KEY_B,
    [0x31] = INPUT_KEY_N,
    [0x32] = INPUT_KEY_M,
};

/* E0 扩展码 → Linux KEY_*（方向键子集） */
static uint16_t kbd_e0_keycode(uint8_t sc)
{
    switch (sc)
    {
    case 0x48:
        return INPUT_KEY_UP;
    case 0x50:
        return INPUT_KEY_DOWN;
    case 0x4B:
        return INPUT_KEY_LEFT;
    case 0x4D:
        return INPUT_KEY_RIGHT;
    default:
        return 0;
    }
}

/* 中断上下文：入队单个事件，满则丢弃（compositor 独占读，正常到不了） */
static void kbd_push_event(uint64_t now, uint16_t type, uint16_t code, int32_t value)
{
    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);

    if (g_kbd_count < KBD_EVENT_BUF)
    {
        g_kbd_buf[g_kbd_tail].time_ms = now;
        g_kbd_buf[g_kbd_tail].type = type;
        g_kbd_buf[g_kbd_tail].code = code;
        g_kbd_buf[g_kbd_tail].value = value;
        g_kbd_tail = (g_kbd_tail + 1) % KBD_EVENT_BUF;
        g_kbd_count++;
    }
    spin_unlock_irqrestore(&g_kbd_lock, flags);
}

static void kbd_halt(const char *msg)
{
    panic("%s", msg);
}

static void kbd_wait_ibf_clear()
{
    int spins = 100000;
    while ((inb(KBD_STATUS) & KBD_STAT_IBF) != 0)
    {
        if (--spins == 0)
            kbd_halt("FATAL: 8042 input buffer stuck\n");
    }
}

static void kbd_wait_obf_set()
{
    int spins = 100000;
    while ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
    {
        if (--spins == 0)
            kbd_halt("FATAL: 8042 output buffer empty\n");
    }
}

static void kbd_flush_output()
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

static uint8_t kbd_read_data()
{
    kbd_wait_obf_set();
    return inb(KBD_DATA);
}

void handler_kbd(exception_frame_t *frame)
{
    uint8_t sc;
    uint16_t code;
    int press;
    uint64_t now;
    task_t *w;

    (void)frame;

    if ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
    {
        lapic_eoi();
        return;
    }

    sc = inb(KBD_DATA);

    /* E0 前缀：只记状态，不产生事件 */
    if (sc == 0xE0)
    {
        g_kbd_e0 = 1;
        lapic_eoi();
        return;
    }
    press = (sc & 0x80) == 0 ? INPUT_KEY_PRESS : INPUT_KEY_RELEASE;
    sc &= (uint8_t)~0x80u;
    if (sc >= 128)
    {
        g_kbd_e0 = 0;
        lapic_eoi();
        return;
    }
    if (g_kbd_e0)
    {
        g_kbd_e0 = 0;
        code = kbd_e0_keycode(sc);
    }
    else
    {
        code = g_set1_keycode[sc];
    }
    if (code != 0)
    {
        now = uptime_ms();
        /* Linux 语义：KEY 事件后跟 SYN_REPORT 分组 */
        kbd_push_event(now, INPUT_EV_KEY, code, press);
        kbd_push_event(now, INPUT_EV_SYN, INPUT_SYN_REPORT, 0);
        /* 缓冲满则丢弃新事件；回显由用户态负责，这里不再输出 */
        w = g_kbd_waiter;
        g_kbd_waiter = NULL;
        if (w != NULL)
            sched_unblock(w);
    }

    lapic_eoi();
}

void init_kbd()
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

static int64_t kbd0_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    input_event_t *p = (input_event_t *)buf;
    uint64_t n = 0;
    uint64_t flags;

    (void)dev;
    (void)f;
    if (len == 0)
        return 0;
    len -= len % sizeof(input_event_t); /* 只给完整事件 */
    if (len == 0)
        return VFS_ERR_INVAL;

    flags = spin_lock_irqsave(&g_kbd_lock);
    while (n < len && g_kbd_count > 0)
    {
        p[n / sizeof(input_event_t)] = g_kbd_buf[g_kbd_head];
        g_kbd_head = (g_kbd_head + 1) % KBD_EVENT_BUF;
        g_kbd_count--;
        n += sizeof(input_event_t);
    }
    spin_unlock_irqrestore(&g_kbd_lock, flags);

    return n ? (int64_t)n : VFS_ERR_AGAIN;
}

int64_t kbd_read_blocking(void *buf, uint64_t len)
{
    input_event_t *p = (input_event_t *)buf;

    if (buf == NULL || len == 0)
        return 0;
    len -= len % sizeof(input_event_t);
    if (len == 0)
        return 0;

    for (;;)
    {
        uint64_t n = 0;
        uint64_t flags = spin_lock_irqsave(&g_kbd_lock);

        while (n < len && g_kbd_count > 0)
        {
            p[n / sizeof(input_event_t)] = g_kbd_buf[g_kbd_head];
            g_kbd_head = (g_kbd_head + 1) % KBD_EVENT_BUF;
            g_kbd_count--;
            n += sizeof(input_event_t);
        }
        if (n > 0)
        {
            spin_unlock_irqrestore(&g_kbd_lock, flags);
            return (int64_t)n;
        }

        /* 单用户任务：直接覆盖等待位（唤醒时已清零，不会残留） */
        g_kbd_waiter = sched_current();
        sched_block();
        spin_unlock_irqrestore(&g_kbd_lock, flags);

        /* 被 sched_unblock 唤醒后回来重查；yield 不持任何锁 */
        yield();
    }
}

static int64_t kbd0_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP; /* 纯输入设备，不可写 */
}

static const vdev_char_ops_t g_kbd0_ops = {
    .read = kbd0_read,
    .write = kbd0_write,
};

static vdev_t g_kbd0 = {
    .name = "kbd0",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_INPUT,
    .minor = 0,
    .ops = &g_kbd0_ops,
};

void init_kbd_dev()
{
    if (vdev_register(&g_kbd0) != VFS_OK)
        put_string("[KBD] register kbd0 failed\n");
}