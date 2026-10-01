#include <ata/ata.h>
#include <kernel.h>
#include <serial.h>
#include <spinlock.h>
#include <cpu.h>
#include <string.h>

/* ---- 端口 ---- */
#define PIO_PRIMARY_IO 0x1F0u
#define PIO_PRIMARY_CTRL 0x3F6u
#define PIO_SECONDARY_IO 0x170u
#define PIO_SECONDARY_CTRL 0x376u

#define REG_DATA(io) ((uint16_t)((io) + 0))
#define REG_ERR(io) ((uint16_t)((io) + 1))
#define REG_FEAT(io) ((uint16_t)((io) + 1))
#define REG_COUNT(io) ((uint16_t)((io) + 2))
#define REG_LBA_LO(io) ((uint16_t)((io) + 3))
#define REG_LBA_MID(io) ((uint16_t)((io) + 4))
#define REG_LBA_HI(io) ((uint16_t)((io) + 5))
#define REG_DRIVE(io) ((uint16_t)((io) + 6))
#define REG_STATUS(io) ((uint16_t)((io) + 7))
#define REG_CMD(io) ((uint16_t)((io) + 7))

/* ---- 状态位 ---- */
#define ST_ERR 0x01u
#define ST_DRQ 0x08u
#define ST_SRV 0x10u
#define ST_DF 0x20u
#define ST_RDY 0x40u
#define ST_BSY 0x80u

/* ---- 命令 ---- */
#define CMD_READ_PIO 0x20u
#define CMD_READ_PIO_EXT 0x24u
#define CMD_WRITE_PIO 0x30u
#define CMD_WRITE_PIO_EXT 0x34u
#define CMD_FLUSH 0xE7u
#define CMD_FLUSH_EXT 0xEAu
#define CMD_IDENTIFY 0xECu

#define CLAMP_MAX_SECTORS 256u
#define PIO_TIMEOUT 1000000

#define ATA_ERR_IO -1
#define ATA_ERR_NODEV -2
#define ATA_ERR_INVAL -3
#define ATA_ERR_TIMEOUT -4

typedef struct pio_drive
{
    int present;
    int atapi;
    int lba48;
    uint64_t sectors;
    char model[41];
    uint16_t io;
    uint16_t ctrl;
    uint8_t slave;
} pio_drive_t;

static pio_drive_t g_drives[ATA_MAX_DRIVES];
static uint8_t g_boot_drive = ATA_NO_BOOT_DRIVE;
static int g_pio_ready;
static spinlock_t g_pio_lock = SPINLOCK_INIT;

/* 块端口 IO：kernel.h 只提供单字节/单字 inb/outb/inw/outw，
 * 扇区批量传输的 rep insw/outsw 保留在此 */
static inline void pio_insw(uint16_t port, uint16_t *buf, uint32_t words)
{
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(words)
                     : "d"(port)
                     : "memory");
}

static inline void pio_outsw(uint16_t port, const uint16_t *buf, uint32_t words)
{
    /* rep outsw 读 DS:(R)SI，buf 视为可变指针推进 */
    uint16_t *tmp = (uint16_t *)(uintptr_t)buf;
    __asm__ volatile("rep outsw"
                     : "+S"(tmp), "+c"(words)
                     : "d"(port)
                     : "memory");
}

/* 400ns 延迟：读 AltStatus 4 次 */
static inline void pio_delay400ns(uint16_t ctrl)
{
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
}

static void pio_select(uint16_t io, uint16_t ctrl, uint8_t slave)
{
    outb(REG_DRIVE(io), (uint8_t)(0xA0 | (slave << 4)));
    pio_delay400ns(ctrl);
}

static int pio_wait_not_busy(uint16_t io)
{
    int i;

    for (i = 0; i < PIO_TIMEOUT; i++)
    {
        if (!(inb(REG_STATUS(io)) & ST_BSY))
            return 0;
        cpu_pause();
    }
    return ATA_ERR_TIMEOUT;
}

/* 等待 DRQ=1（BSY=0 前提下）；遇 ERR/DF 返回 I/O 错误 */
static int pio_wait_drq(uint16_t io)
{
    int i;
    uint8_t st;

    for (i = 0; i < PIO_TIMEOUT; i++)
    {
        st = inb(REG_STATUS(io));
        if (!(st & ST_BSY))
        {
            if (st & ST_ERR)
                return ATA_ERR_IO;
            if (st & ST_DF)
                return ATA_ERR_IO;
            if (st & ST_DRQ)
                return 0;
        }
        cpu_pause();
    }
    return ATA_ERR_TIMEOUT;
}

static int pio_wait_ready(uint16_t io)
{
    int rc = pio_wait_not_busy(io);

    if (rc != 0)
        return rc;
    if (inb(REG_STATUS(io)) & (ST_ERR | ST_DF))
        return ATA_ERR_IO;
    return 0;
}

/* ---- LBA28 / LBA48 单命令块（调用前已持锁，n ∈ [1,256]） ---- */

static int pio_read_chunk(pio_drive_t *d, uint64_t lba, uint32_t n, void *buf)
{
    uint16_t io = d->io;
    uint16_t *p = (uint16_t *)buf;
    uint32_t i;
    uint8_t cmd;
    int rc;
    int use48 = 0;

    if (d->lba48 && (lba + (uint64_t)n) > 0x0FFFFFFFULL)
        use48 = 1;

    rc = pio_wait_not_busy(io);
    if (rc != 0)
        return rc;

    if (use48)
    {
        uint16_t count16 = (uint16_t)n; /* n=256 -> 0x0100，硬件恰为 256 扇区 */

        outb(REG_DRIVE(io), (uint8_t)(0x40 | (d->slave << 4)));
        pio_delay400ns(d->ctrl);
        outb(REG_FEAT(io), 0);
        outb(REG_COUNT(io), (uint8_t)(count16 >> 8));
        outb(REG_LBA_LO(io), (uint8_t)(lba >> 24));
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 32));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 40));
        outb(REG_COUNT(io), (uint8_t)count16);
        outb(REG_LBA_LO(io), (uint8_t)lba);
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 8));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 16));
        cmd = CMD_READ_PIO_EXT;
    }
    else
    {
        uint8_t count8 = (n == 256) ? 0 : (uint8_t)n;

        outb(REG_DRIVE(io), (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        outb(REG_FEAT(io), 0);
        outb(REG_COUNT(io), count8);
        outb(REG_LBA_LO(io), (uint8_t)lba);
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 8));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 16));
        cmd = CMD_READ_PIO;
    }

    outb(REG_CMD(io), cmd);

    for (i = 0; i < n; i++)
    {
        rc = pio_wait_drq(io);
        if (rc != 0)
            return rc;
        pio_insw(REG_DATA(io), p, ATA_SECTOR_SIZE / 2);
        p += ATA_SECTOR_SIZE / 2;
    }
    return 0;
}

static int pio_write_chunk(pio_drive_t *d, uint64_t lba, uint32_t n, const void *buf)
{
    uint16_t io = d->io;
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t i;
    uint8_t cmd;
    int rc;
    int use48 = 0;

    if (d->lba48 && (lba + (uint64_t)n) > 0x0FFFFFFFULL)
        use48 = 1;

    rc = pio_wait_not_busy(io);
    if (rc != 0)
        return rc;

    if (use48)
    {
        uint16_t count16 = (uint16_t)n;

        outb(REG_DRIVE(io), (uint8_t)(0x40 | (d->slave << 4)));
        pio_delay400ns(d->ctrl);
        outb(REG_FEAT(io), 0);
        outb(REG_COUNT(io), (uint8_t)(count16 >> 8));
        outb(REG_LBA_LO(io), (uint8_t)(lba >> 24));
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 32));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 40));
        outb(REG_COUNT(io), (uint8_t)count16);
        outb(REG_LBA_LO(io), (uint8_t)lba);
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 8));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 16));
        cmd = CMD_WRITE_PIO_EXT;
    }
    else
    {
        uint8_t count8 = (n == 256) ? 0 : (uint8_t)n;

        outb(REG_DRIVE(io), (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        outb(REG_FEAT(io), 0);
        outb(REG_COUNT(io), count8);
        outb(REG_LBA_LO(io), (uint8_t)lba);
        outb(REG_LBA_MID(io), (uint8_t)(lba >> 8));
        outb(REG_LBA_HI(io), (uint8_t)(lba >> 16));
        cmd = CMD_WRITE_PIO;
    }

    outb(REG_CMD(io), cmd);

    for (i = 0; i < n; i++)
    {
        rc = pio_wait_drq(io);
        if (rc != 0)
            return rc;
        pio_outsw(REG_DATA(io), p, ATA_SECTOR_SIZE / 2);
        p += ATA_SECTOR_SIZE / 2;
    }
    return 0;
}

static int pio_flush_drive(pio_drive_t *d)
{
    uint16_t io = d->io;
    int rc = pio_wait_not_busy(io);

    if (rc != 0)
        return rc;
    outb(REG_DRIVE(io), (uint8_t)(0xA0 | (d->slave << 4)));
    pio_delay400ns(d->ctrl);
    outb(REG_CMD(io), d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    return pio_wait_not_busy(io);
}

/* ---- IDENTIFY 探测 ---- */

static void pio_save_model(pio_drive_t *d, const uint16_t *id)
{
    int i;
    int end = 40;

    for (i = 0; i < 20; i++)
    {
        d->model[i * 2] = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    while (end > 0 && d->model[end - 1] == ' ')
        end--;
    d->model[end] = '\0';
}

static int pio_probe(uint16_t io, uint16_t ctrl, uint8_t slave, pio_drive_t *out)
{
    uint16_t id[256];
    uint8_t mid, hi, st;
    int i;

    out->present = 0;
    out->atapi = 0;
    out->io = io;
    out->ctrl = ctrl;
    out->slave = slave;

    pio_select(io, ctrl, slave);

    /* 浮空总线读回 0x00/0xFF：无设备，直接返回 */
    st = inb(REG_STATUS(io));
    if (st == 0x00 || st == 0xFF)
        return ATA_ERR_NODEV;

    /* 选中后若仍 BSY，先等待再发 IDENTIFY，避免命令被忽略 */
    if (st & ST_BSY)
    {
        if (pio_wait_not_busy(io) != 0)
            return ATA_ERR_TIMEOUT;
    }

    /* 预清扇区计数/LBA，IDENTIFY 要求 */
    outb(REG_COUNT(io), 0);
    outb(REG_LBA_LO(io), 0);
    outb(REG_LBA_MID(io), 0);
    outb(REG_LBA_HI(io), 0);
    outb(REG_CMD(io), CMD_IDENTIFY);

    if (inb(REG_STATUS(io)) == 0)
        return ATA_ERR_NODEV; /* 发送后状态归零：无设备 */

    if (pio_wait_not_busy(io) != 0)
        return ATA_ERR_TIMEOUT;

    mid = inb(REG_LBA_MID(io));
    hi = inb(REG_LBA_HI(io));

    /* ATAPI 签名：暂不驱动，标记后跳过 */
    if ((mid == 0x14 && hi == 0xEB) || (mid == 0x69 && hi == 0x96))
    {
        out->present = 1;
        out->atapi = 1;
        return 0;
    }

    st = inb(REG_STATUS(io));
    if (st & ST_ERR)
        return ATA_ERR_NODEV;

    if (pio_wait_drq(io) != 0)
        return ATA_ERR_NODEV;

    pio_insw(REG_DATA(io), id, 256);

    out->present = 1;
    out->atapi = 0;
    out->lba48 = (id[83] & (1u << 10)) ? 1 : 0;
    out->sectors = ((uint32_t)id[61] << 16) | id[60];
    if (out->lba48)
    {
        uint64_t ext = 0;
        for (i = 3; i >= 0; i--)
            ext = (ext << 16) | id[100 + i];
        if (ext != 0)
            out->sectors = ext;
    }
    pio_save_model(out, id);
    return 0;
}

static int pio_init()
{
    static const uint16_t ios[2] = {PIO_PRIMARY_IO, PIO_SECONDARY_IO};
    static const uint16_t ctrls[2] = {PIO_PRIMARY_CTRL, PIO_SECONDARY_CTRL};
    int bus, s;
    int found = 0;

    if (g_pio_ready)
        return 0;

    kernel_memset(g_drives, 0, (uint32_t)sizeof(g_drives));
    g_boot_drive = ATA_NO_BOOT_DRIVE;

    for (bus = 0; bus < 2; bus++)
    {
        for (s = 0; s < 2; s++)
        {
            pio_drive_t *d = &g_drives[bus * 2 + s];
            int rc = pio_probe(ios[bus], ctrls[bus], (uint8_t)s, d);

            if (rc == 0 && d->present && !d->atapi && d->sectors != 0)
            {
                fput_string("[ATA-PIO] drive=%d %s %s sectors=%llu model='%s'\n",
                            bus * 2 + s,
                            bus == 0 ? "primary" : "secondary",
                            s == 0 ? "master" : "slave",
                            d->sectors, d->model);
                if (g_boot_drive == ATA_NO_BOOT_DRIVE)
                    g_boot_drive = (uint8_t)(bus * 2 + s);
                found++;
            }
            else if (rc == 0 && d->present && d->atapi)
            {
                fput_string("[ATA-PIO] drive=%d ATAPI skipped\n", bus * 2 + s);
            }
        }
    }

    if (!found)
    {
        fput_string("[ATA-PIO] no ATA drives (QEMU: add -drive if=id,index=N)\n");
        /* 无盘不视为致命错误：允许无盘启动，后续 read/write 返回 NODEV */
        g_pio_ready = 1;
        return -2;
    }

    g_pio_ready = 1;
    fput_string("[ATA-PIO] ready boot=%d mode=PIO LBA28/48\n", g_boot_drive);
    return 0;
}

static int pio_check(uint8_t drive, uint64_t lba, uint32_t sectors, const void *buf, pio_drive_t **out)
{
    pio_drive_t *d;

    if (sectors == 0 || buf == NULL)
        return ATA_ERR_INVAL;
    if (drive >= ATA_MAX_DRIVES)
        return ATA_ERR_INVAL;
    d = &g_drives[drive];
    if (!g_pio_ready || !d->present || d->atapi)
        return ATA_ERR_NODEV;
    if (lba >= d->sectors || (uint64_t)sectors > d->sectors - lba)
        return ATA_ERR_INVAL;
    if (!d->lba48 && (lba + (uint64_t)sectors) > 0x0FFFFFFFULL)
        return ATA_ERR_INVAL;
    *out = d;
    return 0;
}

static int pio_read_sectors(uint8_t drive, uint64_t lba, uint32_t sectors, void *buf)
{
    pio_drive_t *d;
    uint8_t *p = (uint8_t *)buf;
    uint64_t flags;
    int rc;

    rc = pio_check(drive, lba, sectors, buf, &d);
    if (rc != 0)
        return rc;

    flags = spin_lock_irqsave(&g_pio_lock);
    while (sectors > 0)
    {
        uint32_t n = sectors > CLAMP_MAX_SECTORS ? CLAMP_MAX_SECTORS : sectors;

        rc = pio_read_chunk(d, lba, n, p);
        if (rc != 0)
            break;
        lba += n;
        p += (uint64_t)n * ATA_SECTOR_SIZE;
        sectors -= n;
    }
    spin_unlock_irqrestore(&g_pio_lock, flags);
    return rc;
}

static int pio_write_sectors(uint8_t drive, uint64_t lba, uint32_t sectors, const void *buf)
{
    pio_drive_t *d;
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t flags;
    int rc;

    rc = pio_check(drive, lba, sectors, buf, &d);
    if (rc != 0)
        return rc;

    flags = spin_lock_irqsave(&g_pio_lock);
    while (sectors > 0)
    {
        uint32_t n = sectors > CLAMP_MAX_SECTORS ? CLAMP_MAX_SECTORS : sectors;

        rc = pio_write_chunk(d, lba, n, p);
        if (rc != 0)
            break;
        lba += n;
        p += (uint64_t)n * ATA_SECTOR_SIZE;
        sectors -= n;
    }
    if (rc == 0)
        rc = pio_flush_drive(d);
    spin_unlock_irqrestore(&g_pio_lock, flags);
    return rc;
}

static int pio_flush(uint8_t drive)
{
    pio_drive_t *d;
    uint64_t flags;
    int rc;

    if (drive >= ATA_MAX_DRIVES)
        return ATA_ERR_INVAL;
    d = &g_drives[drive];
    if (!g_pio_ready || !d->present || d->atapi)
        return ATA_ERR_NODEV;

    flags = spin_lock_irqsave(&g_pio_lock);
    rc = pio_flush_drive(d);
    spin_unlock_irqrestore(&g_pio_lock, flags);
    return rc;
}

static uint64_t pio_drive_sectors(uint8_t drive)
{
    if (drive >= ATA_MAX_DRIVES)
        return 0;
    if (!g_drives[drive].present || g_drives[drive].atapi)
        return 0;
    return g_drives[drive].sectors;
}

static uint8_t pio_boot_drive(void)
{
    return g_boot_drive;
}

const ata_ops_t ata_pio_ops = {
    .name = "ata-pio",
    .init = pio_init,
    .read_sectors = pio_read_sectors,
    .write_sectors = pio_write_sectors,
    .flush = pio_flush,
    .drive_sectors = pio_drive_sectors,
    .boot_drive = pio_boot_drive,
};