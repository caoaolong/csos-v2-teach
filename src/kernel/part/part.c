#include <part/part.h>
#include <vdev.h>
#include <serial.h>
#include <string.h>

#define PART_SEC_SIZE 512u
#define PART_BOOT_SIG 0xaa55u
#define PART_MBR_TABLE 446u
#define PART_MBR_ENTRY 16u
#define PART_TYPE_GPT_PROTECT 0xeeu
#define PART_TYPE_EXT 0x05u
#define PART_TYPE_EXT_LBA 0x0fu
#define PART_EXT_GUARD 64
#define PART_MAX_DRIVES 4
#define PART_MAX_PER_DRIVE 16

#define PART_GPT_REV 0x00010000u
#define PART_GPT_HDR_MIN_SIZE 92u
#define PART_GPT_ENTRY_SIZE 128u
#define PART_GPT_MAX_ENTRIES 1024u
#define PART_GPT_MAX_ENTRY_SIZE 1024u
static const uint8_t PART_GPT_MAGIC[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};

typedef struct part_entry
{
    uint8_t type; /* MBR 分区类型；GPT 项固定为 0xEE */
    uint64_t start;
    uint64_t sectors;
    uint8_t is_gpt;        /* 1 = 来自 GPT */
    uint8_t type_guid[16]; /* 仅 is_gpt=1 时有效（原始小端字节序） */
} part_entry_t;

typedef struct part_disk
{
    vdev_t *parent;
    uint64_t disk_sectors;
    int count;
    part_entry_t parts[PART_MAX_PER_DRIVE];
} part_disk_t;

typedef struct part_priv
{
    vdev_t *parent;
    uint64_t start;
} part_priv_t;

static part_disk_t g_disks[PART_MAX_DRIVES];
static vdev_t g_pdev[PART_MAX_DRIVES][PART_MAX_PER_DRIVE];
static part_priv_t g_ppriv[PART_MAX_DRIVES][PART_MAX_PER_DRIVE];
static int g_ready;

static uint16_t part_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t part_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t part_rd64(const uint8_t *p)
{
    return (uint64_t)part_rd32(p) | ((uint64_t)part_rd32(p + 4) << 32);
}

/* CRC32（IEEE 802.3，多项式 0xEDB88320），GPT 头/表校验用。
 * 用法：crc = 0xFFFFFFFF; crc = part_crc32_update(crc, buf, len); ...; final = crc ^ 0xFFFFFFFF */
static uint32_t part_crc32_update(uint32_t crc, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        uint32_t k;

        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(0u - (crc & 1u)));
    }
    return crc;
}

static int part_guid_is_zero(const uint8_t *g)
{
    int i;

    for (i = 0; i < 16; i++)
        if (g[i] != 0)
            return 0;
    return 1;
}

static char part_hexchar(uint8_t v)
{
    return (char)(v < 10 ? '0' + v : 'A' + (v - 10));
}

/* 按 EFI GUID 显示字节序格式化 36 字符 + '\0'（调用者保证 out >= 37 字节） */
static void part_guid_str(const uint8_t *g, char *out)
{
    static const uint8_t pos[16] = {3, 2, 1, 0, 5, 4, 7, 6,
                                    8, 9, 10, 11, 12, 13, 14, 15};
    int i = 0;
    int k;

    for (k = 0; k < 16; k++)
    {
        uint8_t b = g[pos[k]];

        out[i++] = part_hexchar((uint8_t)(b >> 4));
        out[i++] = part_hexchar((uint8_t)(b & 0x0f));
        if (k == 3 || k == 5 || k == 7 || k == 9)
            out[i++] = '-';
    }
    out[i] = '\0';
}

/* 十进制追加到 buf（调用者保证空间），返回新长度 */
static uint32_t part_put_dec(char *buf, uint32_t n, uint32_t v)
{
    char rev[10];
    uint32_t k = 0;

    do
    {
        rev[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (k)
        buf[n++] = rev[--k];
    return n;
}

static void part_disk_name(uint8_t drive, char *buf, uint32_t size)
{
    char tmp[PART_NAME_MAX];
    uint32_t n = 0;

    tmp[n++] = 'h';
    tmp[n++] = 'd';
    n = part_put_dec(tmp, n, drive);
    tmp[n] = '\0';
    kernel_strncpy(buf, tmp, size);
}

static void part_part_name(uint8_t drive, int index, char *buf, uint32_t size)
{
    char tmp[PART_NAME_MAX];
    uint32_t n = 0;

    tmp[n++] = 'h';
    tmp[n++] = 'd';
    n = part_put_dec(tmp, n, drive);
    tmp[n++] = 'p';
    n = part_put_dec(tmp, n, (uint32_t)index);
    tmp[n] = '\0';
    kernel_strncpy(buf, tmp, size);
}

static void part_add(part_disk_t *pd, uint8_t drive, uint8_t type,
                     uint64_t start, uint64_t sectors)
{
    part_entry_t *p;

    if (pd->count >= PART_MAX_PER_DRIVE)
    {
        fput_string("[PART] drive=%u too many partitions, ignored\n", drive);
        return;
    }
    if (sectors == 0 || start == 0 || start + sectors > pd->disk_sectors)
    {
        fput_string("[PART] drive=%u bad range start=%llu sectors=%llu, skipped\n",
                    drive, start, sectors);
        return;
    }
    p = &pd->parts[pd->count];
    p->type = type;
    p->start = start;
    p->sectors = sectors;
    p->is_gpt = 0;
    kernel_memset(p->type_guid, 0, 16);
    pd->count++;
}

static void part_add_gpt(part_disk_t *pd, uint8_t drive, const uint8_t *type_guid,
                         uint64_t start, uint64_t sectors)
{
    part_entry_t *p;
    int i;

    if (pd->count >= PART_MAX_PER_DRIVE)
    {
        fput_string("[PART] drive=%u too many partitions, ignored\n", drive);
        return;
    }
    if (sectors == 0 || start == 0 || start + sectors > pd->disk_sectors)
    {
        fput_string("[PART] drive=%u bad range start=%llu sectors=%llu, skipped\n",
                    drive, start, sectors);
        return;
    }
    p = &pd->parts[pd->count];
    p->type = PART_TYPE_GPT_PROTECT;
    p->start = start;
    p->sectors = sectors;
    p->is_gpt = 1;
    for (i = 0; i < 16; i++)
        p->type_guid[i] = type_guid[i];
    pd->count++;
}

/* 扩展分区链：EBR 表首项为逻辑分区（相对本 EBR），次项指向下一个
 * EBR（相对扩展分区基址 ext_base） */
static void part_scan_ext(part_disk_t *pd, uint8_t drive, uint64_t ext_base,
                          vdev_t *parent)
{
    uint8_t sec[PART_SEC_SIZE];
    uint64_t ebr = ext_base;
    int guard;

    for (guard = 0; guard < PART_EXT_GUARD; guard++)
    {
        const uint8_t *e0;
        const uint8_t *e1;
        uint8_t t1;

        if (parent->bops->read_blocks(parent, ebr, 1, sec) != 0 ||
            part_rd16(sec + 510) != PART_BOOT_SIG)
            return;
        e0 = sec + PART_MBR_TABLE;
        e1 = sec + PART_MBR_TABLE + PART_MBR_ENTRY;
        if (e0[4] != 0 && part_rd32(e0 + 12) != 0)
            part_add(pd, drive, e0[4], ebr + part_rd32(e0 + 8), part_rd32(e0 + 12));
        t1 = e1[4];
        if ((t1 != PART_TYPE_EXT && t1 != PART_TYPE_EXT_LBA) || part_rd32(e1 + 8) == 0)
            return;
        ebr = ext_base + part_rd32(e1 + 8);
    }
}

static int part_mbr_has_protective(const uint8_t *mbr)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        const uint8_t *pe = mbr + PART_MBR_TABLE + (uint32_t)i * PART_MBR_ENTRY;

        if (pe[4] == PART_TYPE_GPT_PROTECT)
            return 1;
    }
    return 0;
}

/* 按字节偏移从磁盘读 len 字节（len <= 1024），处理跨扇区情况 */
static int part_read_range(vdev_t *parent, uint64_t byte_off, uint8_t *out, uint32_t len)
{
    uint8_t sec[PART_SEC_SIZE];
    uint32_t done = 0;

    if (len == 0 || len > PART_GPT_MAX_ENTRY_SIZE || out == NULL)
        return -1;
    while (done < len)
    {
        uint64_t cur = byte_off + done;
        uint64_t lba = cur / PART_SEC_SIZE;
        uint32_t secoff = (uint32_t)(cur % PART_SEC_SIZE);
        uint32_t chunk = PART_SEC_SIZE - secoff;

        if (chunk > len - done)
            chunk = len - done;
        if (parent->bops->read_blocks(parent, lba, 1, sec) != 0)
            return -1;
        kernel_memcpy(out + done, sec + secoff, chunk);
        done += chunk;
    }
    return 0;
}

static int part_scan_gpt(part_disk_t *pd, uint8_t drive, const uint8_t *mbr,
                         vdev_t *parent)
{
    uint8_t hdr[PART_SEC_SIZE];
    uint8_t tmp[PART_SEC_SIZE];
    uint32_t hdr_size;
    uint32_t hdr_crc_stored;
    uint32_t hdr_crc_calc;
    uint32_t rev;
    uint64_t my_lba;
    uint64_t entries_lba;
    uint32_t num;
    uint32_t esize;
    uint32_t entries_crc_stored;
    uint32_t entries_crc_calc;
    uint64_t entries_bytes;
    uint64_t entries_secs;
    uint64_t s;
    uint32_t i;
    int has_protective;

    if (parent->bops->read_blocks(parent, 1, 1, hdr) != 0)
        return 0;
    if (kernel_memcmp((void *)hdr, (void *)PART_GPT_MAGIC, 8) != 0)
        return 0;

    has_protective = part_mbr_has_protective(mbr);
    rev = part_rd32(hdr + 8);
    hdr_size = part_rd32(hdr + 12);
    hdr_crc_stored = part_rd32(hdr + 16);
    my_lba = part_rd64(hdr + 24);
    entries_lba = part_rd64(hdr + 72);
    num = part_rd32(hdr + 80);
    esize = part_rd32(hdr + 84);
    entries_crc_stored = part_rd32(hdr + 88);

    if (rev != PART_GPT_REV || hdr_size < PART_GPT_HDR_MIN_SIZE ||
        hdr_size > PART_SEC_SIZE || my_lba != 1 ||
        num == 0 || num > PART_GPT_MAX_ENTRIES ||
        esize < PART_GPT_ENTRY_SIZE || esize > PART_GPT_MAX_ENTRY_SIZE ||
        (esize % 8) != 0 || entries_lba == 0 ||
        entries_lba >= pd->disk_sectors)
    {
        fput_string("[PART] drive=%u GPT header invalid, skipped\n", drive);
        return has_protective ? 1 : 0;
    }
    entries_bytes = (uint64_t)num * (uint64_t)esize;
    entries_secs = (entries_bytes + PART_SEC_SIZE - 1) / PART_SEC_SIZE;
    if (entries_lba + entries_secs > pd->disk_sectors)
    {
        fput_string("[PART] drive=%u GPT entries out of range, skipped\n", drive);
        return has_protective ? 1 : 0;
    }

    kernel_memcpy(tmp, hdr, PART_SEC_SIZE);
    tmp[16] = 0;
    tmp[17] = 0;
    tmp[18] = 0;
    tmp[19] = 0;
    hdr_crc_calc = part_crc32_update(0xffffffffu, tmp, hdr_size) ^ 0xffffffffu;
    if (hdr_crc_calc != hdr_crc_stored)
    {
        fput_string("[PART] drive=%u GPT header CRC mismatch, skipped\n", drive);
        return has_protective ? 1 : 0;
    }

    /* 先整表校验 CRC，再解析，避免半截表被部分注册 */
    entries_crc_calc = 0xffffffffu;
    for (s = 0; s < entries_secs; s++)
    {
        uint8_t sec[PART_SEC_SIZE];
        uint64_t remain = entries_bytes - s * PART_SEC_SIZE;
        uint32_t chunk = remain > PART_SEC_SIZE ? PART_SEC_SIZE : (uint32_t)remain;

        if (parent->bops->read_blocks(parent, entries_lba + s, 1, sec) != 0)
        {
            fput_string("[PART] drive=%u GPT entries unreadable, skipped\n", drive);
            return 1;
        }
        entries_crc_calc = part_crc32_update(entries_crc_calc, sec, chunk);
    }
    entries_crc_calc ^= 0xffffffffu;
    if (entries_crc_calc != entries_crc_stored)
    {
        fput_string("[PART] drive=%u GPT entries CRC mismatch, skipped\n", drive);
        return 1;
    }

    for (i = 0; i < num; i++)
    {
        uint8_t e[PART_GPT_MAX_ENTRY_SIZE];
        uint64_t first;
        uint64_t last;

        if (pd->count >= PART_MAX_PER_DRIVE)
        {
            fput_string("[PART] drive=%u GPT too many partitions, capped at %u\n",
                        drive, PART_MAX_PER_DRIVE);
            break;
        }
        if (part_read_range(parent, entries_lba * PART_SEC_SIZE + (uint64_t)i * esize,
                            e, esize) != 0)
        {
            fput_string("[PART] drive=%u GPT entry %u unreadable, stop\n", drive, i);
            break;
        }
        if (part_guid_is_zero(e))
            continue;
        first = part_rd64(e + 32);
        last = part_rd64(e + 40);
        if (first == 0 || last < first || last >= pd->disk_sectors)
        {
            fput_string("[PART] drive=%u GPT entry %u bad range, skipped\n", drive, i);
            continue;
        }
        part_add_gpt(pd, drive, e, first, last - first + 1);
    }
    fput_string("[PART] drive=%u GPT valid entries=%u parts=%d\n",
                drive, num, pd->count);
    return 1;
}

#define PART_MBR_TABLE 446u
#define PART_MBR_ENTRY 16u

static void part_scan_mbr(part_disk_t *pd, uint8_t drive, const uint8_t *mbr,
                          vdev_t *parent)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        const uint8_t *pe = mbr + PART_MBR_TABLE + (uint32_t)i * PART_MBR_ENTRY;
        uint8_t type = pe[4];
        uint64_t lba = part_rd32(pe + 8);
        uint64_t cnt = part_rd32(pe + 12);

        if (type == 0 || cnt == 0)
            continue;
        if (type == PART_TYPE_GPT_PROTECT)
        {
            fput_string("[PART] drive=%u GPT protective entry without valid GPT header, skipped\n", drive);
            continue;
        }
        if (type == PART_TYPE_EXT || type == PART_TYPE_EXT_LBA)
            part_scan_ext(pd, drive, lba, parent);
        else
            part_add(pd, drive, type, lba, cnt);
    }
}

static int part_open(vdev_t *dev, vfs_file_t *f)
{
    (void)dev;
    (void)f;
    return VFS_OK;
}

static int part_close(vdev_t *dev, vfs_file_t *f)
{
    (void)dev;
    (void)f;
    return VFS_OK;
}

static int part_read_blocks(vdev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    part_priv_t *p = (part_priv_t *)dev->priv;

    if (count == 0 || buf == NULL)
        return VFS_ERR_INVAL;
    if (lba >= dev->nblocks || (uint64_t)count > dev->nblocks - lba)
        return VFS_ERR_INVAL;
    return p->parent->bops->read_blocks(p->parent, p->start + lba, count, buf);
}

static int part_write_blocks(vdev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    part_priv_t *p = (part_priv_t *)dev->priv;

    if (count == 0 || buf == NULL)
        return VFS_ERR_INVAL;
    if (!p->parent->bops->write_blocks)
        return VFS_ERR_NOTSUP;
    if (lba >= dev->nblocks || (uint64_t)count > dev->nblocks - lba)
        return VFS_ERR_INVAL;
    return p->parent->bops->write_blocks(p->parent, p->start + lba, count, buf);
}

static int part_flush(vdev_t *dev)
{
    part_priv_t *p = (part_priv_t *)dev->priv;

    if (p->parent->bops->flush)
        return p->parent->bops->flush(p->parent);
    return VFS_OK;
}

static uint64_t part_capacity(vdev_t *dev)
{
    return dev->nblocks;
}

static const vdev_block_ops_t g_part_ops = {
    .open = part_open,
    .close = part_close,
    .read_blocks = part_read_blocks,
    .write_blocks = part_write_blocks,
    .flush = part_flush,
    .capacity = part_capacity,
};

static void part_scan_drive(uint8_t drive)
{
    part_disk_t *pd = &g_disks[drive];
    char dname[PART_NAME_MAX];
    vdev_t *parent;
    uint8_t sec[PART_SEC_SIZE];
    int i;

    kernel_memset(pd, 0, (uint32_t)sizeof(*pd));
    part_disk_name(drive, dname, sizeof(dname));
    parent = vdev_find(dname);
    if (!parent || parent->type != VDEV_BLOCK || !parent->bops)
        return;
    if (parent->block_size != PART_SEC_SIZE || parent->nblocks == 0)
        return;
    pd->parent = parent;
    pd->disk_sectors = parent->nblocks;

    if (parent->bops->read_blocks(parent, 0, 1, sec) != 0 ||
        part_rd16(sec + 510) != PART_BOOT_SIG)
    {
        fput_string("[PART] %s no boot signature, no partition table\n", dname);
        return;
    }

    if (!part_scan_gpt(pd, drive, sec, parent))
        part_scan_mbr(pd, drive, sec, parent);
    fput_string("[PART] %s sectors=%llu parts=%d\n", dname, pd->disk_sectors, pd->count);
    for (i = 0; i < pd->count; i++)
    {
        char pname[PART_NAME_MAX];
        vdev_t *d;
        int rc;

        part_part_name(drive, i, pname, sizeof(pname));
        d = &g_pdev[drive][i];
        kernel_memset(d, 0, (uint32_t)sizeof(*d));
        kernel_strncpy(d->name, pname, sizeof(d->name));
        d->type = VDEV_BLOCK;
        d->major = parent->major;
        d->minor = parent->minor;
        d->bops = &g_part_ops;
        d->block_size = PART_SEC_SIZE;
        d->nblocks = pd->parts[i].sectors;
        g_ppriv[drive][i].parent = parent;
        g_ppriv[drive][i].start = pd->parts[i].start;
        d->priv = &g_ppriv[drive][i];

        rc = vdev_register(d);
        if (rc != VFS_OK)
            fput_string("[PART] register %s failed rc=%d\n", pname, rc);
        else if (pd->parts[i].is_gpt)
        {
            char guid[37];

            part_guid_str(pd->parts[i].type_guid, guid);
            fput_string("[PART]   %s start=%llu sectors=%llu GPT %s\n",
                        pname, pd->parts[i].start, pd->parts[i].sectors,
                        guid);
        }
        else
            fput_string("[PART]   %s start=%llu sectors=%llu type=0x%x\n",
                        pname, pd->parts[i].start, pd->parts[i].sectors,
                        pd->parts[i].type);
    }
}

void init_partitions()
{
    uint8_t d;

    if (g_ready)
        return;
    kernel_memset(g_disks, 0, (uint32_t)sizeof(g_disks));
    for (d = 0; d < PART_MAX_DRIVES; d++)
        part_scan_drive(d);
    g_ready = 1;
}

int part_count()
{
    int n = 0;
    int d;

    if (!g_ready)
        return 0;
    for (d = 0; d < PART_MAX_DRIVES; d++)
        n += g_disks[d].count;
    return n;
}

int part_name(int idx, char *buf, uint32_t size)
{
    int d;

    if (!g_ready || !buf || size == 0 || idx < 0)
        return VFS_ERR_INVAL;
    for (d = 0; d < PART_MAX_DRIVES; d++)
    {
        if (idx < g_disks[d].count)
        {
            part_part_name((uint8_t)d, idx, buf, size);
            return VFS_OK;
        }
        idx -= g_disks[d].count;
    }
    return VFS_ERR_NOENT;
}
