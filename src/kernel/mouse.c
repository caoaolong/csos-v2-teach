#include <kernel/mouse.h>
#include <minput.h>
#include <kernel/apic.h>
#include <kernel/apic/ioapic.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>
#include <kernel/spinlock.h>
#include <kernel/vdev.h>

/* PS/2 鼠标：8042 第二端口（AUX），IRQ12，标准 3 字节包。
 * 初始化走 0xD4 写 AUX 命令；数据包由 handler_mouse 组装，解码后进事件环；
 * 用户态经 /dev/mouse0 读 mouse_event_t（读空回 AGAIN，靠 syscall 层 yield 重试）。
 * v1 约束：标准 3 字节协议（无滚轮）；溢出位钳制到 ±255；单读端假设（无 waiter，
 * 多任务同时读会分食事件——display 独占场景够用，focus 路由以后再做）。 */

#define AUX_DATA 0x60
#define AUX_STATUS 0x64
#define AUX_CMD 0x64

#define AUX_STAT_OBF (1u << 0)
#define AUX_STAT_IBF (1u << 1)
#define AUX_STAT_MOUSE (1u << 5) /* OBF 数据来源：1=鼠标，0=键盘 */

#define AUX_CMD_ENABLE_PORT2 0xA8
#define AUX_CMD_WRITE_AUX 0xD4
#define AUX_CMD_READ_CFG 0x20
#define AUX_CMD_WRITE_CFG 0x60

#define AUX_REPLY_ACK 0xFA
#define AUX_REPLY_RESEND 0xFE

#define MOUSE_CMD_RESET 0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE 0xF4

#define MOUSE_EVENT_BUF 128

static mouse_event_t g_mouse_buf[MOUSE_EVENT_BUF];
static uint32_t g_mouse_head;
static uint32_t g_mouse_tail;
static uint32_t g_mouse_count;
static spinlock_t g_mouse_lock = SPINLOCK_INIT;

/* 包组装状态（中断上下文推进）：idx=已收字节数 */
static uint8_t g_pkt[3];
static uint32_t g_pkt_idx;

static int mouse_available; /* 初始化成功标志；失败则只有键盘能用 */

static void mouse_wait_ibf_clear()
{
    int spins = 100000;

    while ((inb(AUX_STATUS) & AUX_STAT_IBF) != 0)
    {
        if (--spins == 0)
            return; /* 不 halt：鼠标缺席不能连累键盘 */
    }
}

/* 读一字节应答，超时返回 -1 */
static int mouse_read_byte()
{
    int spins = 100000;

    while ((inb(AUX_STATUS) & AUX_STAT_OBF) == 0)
    {
        if (--spins == 0)
            return -1;
    }
    return (int)inb(AUX_DATA);
}

static void mouse_write_aux(uint8_t cmd)
{
    mouse_wait_ibf_clear();
    outb(AUX_CMD, AUX_CMD_WRITE_AUX);
    mouse_wait_ibf_clear();
    outb(AUX_DATA, cmd);
}

/* 发 AUX 命令并等 ACK（0xFE 则重发一次）；成功返回 0 */
static int mouse_cmd_ack(uint8_t cmd)
{
    int r;

    mouse_write_aux(cmd);
    r = mouse_read_byte();
    if (r == AUX_REPLY_RESEND)
    {
        mouse_write_aux(cmd);
        r = mouse_read_byte();
    }
    return r == AUX_REPLY_ACK ? 0 : -1;
}

void handler_mouse(exception_frame_t *frame)
{
    uint8_t status;
    uint8_t b;

    (void)frame;

    status = inb(AUX_STATUS);
    if ((status & AUX_STAT_OBF) == 0)
    {
        lapic_eoi();
        return;
    }
    if ((status & AUX_STAT_MOUSE) == 0)
    {
        /* 不是鼠标字节（键盘串门）：别消费，留给 IRQ1 处理 */
        lapic_eoi();
        return;
    }
    b = inb(AUX_DATA);

    if (g_pkt_idx == 0 && (b & 0x08) == 0)
    {
        lapic_eoi();
        return; /* 首字节 bit3 恒 1，否则失步丢弃 */
    }
    g_pkt[g_pkt_idx++] = b;
    if (g_pkt_idx < 3)
    {
        lapic_eoi();
        return;
    }

    g_pkt_idx = 0;
    {
        int dx = (g_pkt[0] & 0x10) ? (int)g_pkt[1] - 256 : (int)g_pkt[1];
        int dy = (g_pkt[0] & 0x20) ? (int)g_pkt[2] - 256 : (int)g_pkt[2];
        uint32_t buttons = g_pkt[0] & 0x07;
        uint64_t flags;

        /* 溢出钳制到 ±255 保方向 */
        if (g_pkt[0] & 0x40)
            dx = dx >= 0 ? 255 : -255;
        if (g_pkt[0] & 0x80)
            dy = dy >= 0 ? 255 : -255;
        dy = -dy; /* PS/2 上为正，屏坐标下为正 */

        flags = spin_lock_irqsave(&g_mouse_lock);
        if (g_mouse_count < MOUSE_EVENT_BUF)
        {
            g_mouse_buf[g_mouse_tail].dx = dx;
            g_mouse_buf[g_mouse_tail].dy = dy;
            g_mouse_buf[g_mouse_tail].buttons = buttons;
            g_mouse_tail = (g_mouse_tail + 1) % MOUSE_EVENT_BUF;
            g_mouse_count++;
        }
        /* 满则丢弃最新包（display 独占读，正常到不了） */
        spin_unlock_irqrestore(&g_mouse_lock, flags);
    }

    lapic_eoi();
}

void init_mouse()
{
    uint8_t cfg;
    int r;

    /* 开第二端口；键盘（第一端口）保持 kbd 初始化的状态不动 */
    mouse_wait_ibf_clear();
    outb(AUX_CMD, AUX_CMD_ENABLE_PORT2);

    mouse_wait_ibf_clear();
    outb(AUX_CMD, AUX_CMD_READ_CFG);
    r = mouse_read_byte();
    if (r < 0)
    {
        fput_string("[MOUSE] no aux port, disabled\n");
        return;
    }
    cfg = (uint8_t)r;
    cfg |= (1u << 1); /* 第二端口中断开 */
    mouse_wait_ibf_clear();
    outb(AUX_CMD, AUX_CMD_WRITE_CFG);
    mouse_wait_ibf_clear();
    outb(AUX_DATA, cfg);

    /* 复位：FA, AA, 00 */
    mouse_write_aux(MOUSE_CMD_RESET);
    if (mouse_read_byte() != AUX_REPLY_ACK ||
        mouse_read_byte() != 0xAA || mouse_read_byte() != 0x00)
    {
        fput_string("[MOUSE] reset failed, disabled\n");
        return;
    }
    if (mouse_cmd_ack(MOUSE_CMD_DEFAULTS) != 0 ||
        mouse_cmd_ack(MOUSE_CMD_ENABLE) != 0)
    {
        fput_string("[MOUSE] enable failed, disabled\n");
        return;
    }

    ioapic_route_isa_irq(MOUSE_ISA_IRQ, MOUSE_VECTOR, lapic_id());
    mouse_available = 1;
    fput_string("[MOUSE] ready IRQ%u vector=%u\n",
                (unsigned)MOUSE_ISA_IRQ, (unsigned)MOUSE_VECTOR);
}

static int64_t mouse_read(vdev_t *dev, vfs_file_t *f, void *buf, uint64_t len)
{
    mouse_event_t *p = (mouse_event_t *)buf;
    uint64_t n = 0;
    uint64_t flags;

    (void)dev;
    (void)f;
    if (len == 0)
        return 0;
    len -= len % sizeof(mouse_event_t); /* 只给完整事件 */
    if (len == 0)
        return VFS_ERR_INVAL;

    flags = spin_lock_irqsave(&g_mouse_lock);
    while (n < len && g_mouse_count > 0)
    {
        p[n / sizeof(mouse_event_t)] = g_mouse_buf[g_mouse_head];
        g_mouse_head = (g_mouse_head + 1) % MOUSE_EVENT_BUF;
        g_mouse_count--;
        n += sizeof(mouse_event_t);
    }
    spin_unlock_irqrestore(&g_mouse_lock, flags);

    return n ? (int64_t)n : VFS_ERR_AGAIN;
}

static int64_t mouse_write(vdev_t *dev, vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)dev;
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP; /* 纯输入设备，不可写 */
}

static const vdev_char_ops_t g_mouse_ops = {
    .read = mouse_read,
    .write = mouse_write,
};

static vdev_t g_mouse0 = {
    .name = "mouse0",
    .type = VDEV_CHAR,
    .major = VDEV_MAJOR_INPUT,
    .minor = 1,
    .ops = &g_mouse_ops,
};

void init_mouse_dev()
{
    if (!mouse_available)
    {
        put_string("[MOUSE] dev skipped (no mouse)\n");
        return;
    }
    if (vdev_register(&g_mouse0) != VFS_OK)
        put_string("[MOUSE] register mouse0 failed\n");
}
