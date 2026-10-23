#include <fs/fat32.h>
#include <fs/vfs.h>
#include <vdev.h>
#include <memory/heap.h>
#include <spinlock.h>
#include <serial.h>
#include <string.h>

/* ===== 盘上常量 ===== */
#define FAT32_ATTR_RO 0x01
#define FAT32_ATTR_HID 0x02
#define FAT32_ATTR_SYS 0x04
#define FAT32_ATTR_VOL 0x08
#define FAT32_ATTR_DIR 0x10
#define FAT32_ATTR_ARCH 0x20
#define FAT32_ATTR_LFN 0x0f

#define FAT32_FAT_MASK 0x0fffffffu
#define FAT32_EOC 0x0ffffff8u
#define FAT32_BAD 0x0ffffff7u

#define FAT32_SECTOR 512u
#define FAT32_DATE 0x5c21u /* 2026-01-01，无 RTC 时的固定时间戳 */
#define FAT32_TIME 0x0000u

#define FAT32_OK 0
#define FAT32_ERR_IO (-100)
#define FAT32_ERR_EOF (-101)

/* ===== 小端读写（避免非对齐访问） ===== */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ===== 超级块 ===== */
typedef struct fat32_sb
{
    spinlock_t lock;
    vdev_t *dev;            /* 挂载的块设备（整盘 hdN 或分区 hdNpM，512 字节/块） */
    uint32_t base_lba;      /* 卷在设备内的起始扇区（分区设备为 0，整盘 MBR 首 FAT32 分区为分区起始） */
    uint32_t sec_per_clus;  /* 每簇扇区数 */
    uint32_t reserved_secs; /* 保留扇区数 */
    uint32_t num_fats;
    uint32_t total_secs;
    uint32_t fat_secs; /* 单个 FAT 占用扇区数 */
    uint32_t root_clus;
    uint16_t fs_info_sec;
    uint8_t active_fat;
    uint8_t mirror; /* 1=所有 FAT 同步写 */
    uint32_t fat_start_lba;
    uint32_t data_start_lba; /* 2 号簇所在扇区 */
    uint32_t cluster_count;
    uint32_t clus_bytes;
    uint32_t next_free; /* FSInfo 下一个空闲簇提示 */
    uint32_t free_count;
    int free_known;
} fat32_sb_t;

/* 簇链游标：带一层“已走到第 index 簇”的缓存，顺序读写 O(1) 前进 */
typedef struct fat32_chain
{
    fat32_sb_t *sb;
    uint32_t start;
    uint32_t cache_clus;
    uint32_t cache_index;
    int cache_ok;
} fat32_chain_t;

/* 定位到的目录项（含短名项与前导 LFN 项） */
typedef struct fat32_loc
{
    int found;
    uint32_t dir_clus; /* 所在目录的首簇 */
    uint32_t off;      /* 短名项在目录数据中的字节偏移 */
    uint32_t lfn_off;  /* 第一个 LFN 项的字节偏移（无 LFN 时等于 off） */
    uint32_t clus;     /* 文件/目录首簇 */
    uint32_t size;
    uint8_t attr;
    char name[VFS_NAME_MAX];
} fat32_loc_t;

/* 打开文件句柄 */
typedef struct fat32_file
{
    fat32_chain_t chain;
    uint32_t size;
    uint32_t dir_clus; /* 目录项所在目录 */
    uint32_t dir_off;  /* 短名项字节偏移，写回 size/首簇用 */
    int is_dir;
} fat32_file_t;

/* 目录遍历句柄 */
typedef struct fat32_dirh
{
    uint32_t dir_clus;
    uint32_t next_index;
} fat32_dirh_t;

/* 前向声明 */
static int fat32_fs_info_write(fat32_sb_t *sb);
static uint32_t fat32_alloc_cluster(fat32_sb_t *sb);
static void fat32_free_chain(fat32_sb_t *sb, uint32_t first);
static uint32_t fat32_chain_count(fat32_chain_t *ch);
static int fat32_chain_at2(fat32_chain_t *ch, uint32_t index, uint32_t *out);

/* ===== 设备 IO（扇区号均相对卷首，经 base_lba 换算为设备 LBA） ===== */
static int fat32_read_sec(fat32_sb_t *sb, uint32_t lba, void *buf)
{
    if (!sb->dev || !sb->dev->bops || !sb->dev->bops->read_blocks)
        return FAT32_ERR_IO;
    return sb->dev->bops->read_blocks(sb->dev, (uint64_t)sb->base_lba + lba, 1, buf);
}

static int fat32_write_sec(fat32_sb_t *sb, uint32_t lba, const void *buf)
{
    if (!sb->dev || !sb->dev->bops || !sb->dev->bops->write_blocks)
        return FAT32_ERR_IO;
    return sb->dev->bops->write_blocks(sb->dev, (uint64_t)sb->base_lba + lba, 1, buf);
}

static uint32_t fat32_clus_lba(fat32_sb_t *sb, uint32_t clus)
{
    return sb->data_start_lba + (clus - 2) * sb->sec_per_clus;
}

static int fat32_clus_valid(fat32_sb_t *sb, uint32_t c)
{
    return c >= 2 && c <= sb->cluster_count + 1;
}

/* ===== FAT 表项 ===== */
static uint32_t fat32_fat_get(fat32_sb_t *sb, uint32_t clus)
{
    uint8_t sec[FAT32_SECTOR];
    uint32_t off = clus * 4;
    uint32_t secno = off / FAT32_SECTOR;
    uint32_t so = off % FAT32_SECTOR;
    uint32_t fatno = sb->mirror ? 0 : sb->active_fat;
    uint32_t lba = sb->fat_start_lba + fatno * sb->fat_secs + secno;

    if (fat32_read_sec(sb, lba, sec) != 0)
        return FAT32_EOC;
    return rd32(sec + so) & FAT32_FAT_MASK;
}

static int fat32_fat_set(fat32_sb_t *sb, uint32_t clus, uint32_t val)
{
    uint8_t sec[FAT32_SECTOR];
    uint32_t off = clus * 4;
    uint32_t secno = off / FAT32_SECTOR;
    uint32_t so = off % FAT32_SECTOR;
    uint32_t nf = sb->mirror ? sb->num_fats : 1;
    uint32_t f;

    for (f = 0; f < nf; f++)
    {
        uint32_t fatno = sb->mirror ? f : (uint32_t)sb->active_fat;
        uint32_t lba = sb->fat_start_lba + fatno * sb->fat_secs + secno;
        uint32_t old;

        if (fat32_read_sec(sb, lba, sec) != 0)
            return FAT32_ERR_IO;
        old = rd32(sec + so);
        wr32(sec + so, (old & 0xf0000000u) | (val & FAT32_FAT_MASK));
        if (fat32_write_sec(sb, lba, sec) != 0)
            return FAT32_ERR_IO;
    }
    return FAT32_OK;
}

/* ===== FSInfo / 簇分配 ===== */
static int fat32_fs_info_write(fat32_sb_t *sb)
{
    uint8_t sec[FAT32_SECTOR];

    if (!sb->fs_info_sec)
        return FAT32_OK;
    if (fat32_read_sec(sb, sb->fs_info_sec, sec) != 0)
        return FAT32_ERR_IO;
    if (rd32(sec) != 0x41615252u || rd32(sec + 484) != 0x61417272u)
        return FAT32_OK; /* 无有效 FSInfo，跳过 */
    if (sb->free_known)
        wr32(sec + 488, sb->free_count);
    wr32(sec + 492, sb->next_free);
    if (fat32_write_sec(sb, sb->fs_info_sec, sec) != 0)
        return FAT32_ERR_IO;
    if (sb->fs_info_sec == 1) /* 备份 FSInfo 固定在第 7 扇区 */
        fat32_write_sec(sb, 7, sec);
    return FAT32_OK;
}

static int fat32_zero_cluster(fat32_sb_t *sb, uint32_t clus)
{
    uint8_t zero[FAT32_SECTOR];
    uint32_t i;
    uint32_t base = fat32_clus_lba(sb, clus);

    kernel_memset(zero, 0, FAT32_SECTOR);
    for (i = 0; i < sb->sec_per_clus; i++)
        if (fat32_write_sec(sb, base + i, zero) != 0)
            return FAT32_ERR_IO;
    return FAT32_OK;
}

static uint32_t fat32_alloc_cluster(fat32_sb_t *sb)
{
    uint32_t c, start, limit;
    int pass;

    start = sb->next_free;
    if (!fat32_clus_valid(sb, start))
        start = 2;
    limit = sb->cluster_count + 2;

    for (pass = 0; pass < 2; pass++)
    {
        for (c = start; c < limit; c++)
        {
            if (fat32_fat_get(sb, c) == 0)
            {
                if (fat32_fat_set(sb, c, FAT32_EOC) != FAT32_OK)
                    return 0;
                if (fat32_zero_cluster(sb, c) != FAT32_OK)
                    return 0;
                sb->next_free = c + 1;
                if (sb->free_known && sb->free_count > 0)
                    sb->free_count--;
                fat32_fs_info_write(sb);
                return c;
            }
        }
        start = 2; /* 第二遍从卷首再扫，处理提示点之后无空簇的情况 */
    }
    return 0;
}

static void fat32_free_chain(fat32_sb_t *sb, uint32_t first)
{
    uint32_t c = first;
    uint32_t guard = 0;

    while (fat32_clus_valid(sb, c) && guard <= sb->cluster_count)
    {
        uint32_t next = fat32_fat_get(sb, c);
        if (fat32_fat_set(sb, c, 0) != FAT32_OK)
            return;
        if (sb->free_known)
            sb->free_count++;
        guard++;
        if (next < 2 || next > sb->cluster_count + 1)
            break;
        c = next;
    }
    fat32_fs_info_write(sb);
}

/* ===== 簇链 ===== */
static void fat32_chain_init(fat32_chain_t *ch, fat32_sb_t *sb, uint32_t start)
{
    ch->sb = sb;
    ch->start = start;
    ch->cache_clus = 0;
    ch->cache_index = 0;
    ch->cache_ok = 0;
}

static int fat32_chain_at2(fat32_chain_t *ch, uint32_t index, uint32_t *out)
{
    fat32_sb_t *sb = ch->sb;
    uint32_t c, i;

    if (!fat32_clus_valid(sb, ch->start))
        return FAT32_ERR_IO;
    if (ch->cache_ok && index >= ch->cache_index)
    {
        c = ch->cache_clus;
        i = ch->cache_index;
    }
    else
    {
        c = ch->start;
        i = 0;
    }
    while (i < index)
    {
        uint32_t v = fat32_fat_get(sb, c);
        if (!fat32_clus_valid(sb, v))
            return FAT32_ERR_IO;
        c = v;
        i++;
    }
    ch->cache_clus = c;
    ch->cache_index = i;
    ch->cache_ok = 1;
    *out = c;
    return FAT32_OK;
}

static uint32_t fat32_chain_at(fat32_chain_t *ch, uint32_t index)
{
    uint32_t c = 0;
    if (fat32_chain_at2(ch, index, &c) != FAT32_OK)
        return 0;
    return c;
}

static uint32_t fat32_chain_count(fat32_chain_t *ch)
{
    fat32_sb_t *sb = ch->sb;
    uint32_t c = ch->start;
    uint32_t n = 0;

    while (fat32_clus_valid(sb, c) && n <= sb->cluster_count)
    {
        uint32_t v = fat32_fat_get(sb, c);
        n++;
        if (!fat32_clus_valid(sb, v))
            break;
        c = v;
    }
    return n;
}

static int fat32_chain_read(fat32_chain_t *ch, uint64_t off, void *buf, uint32_t len)
{
    fat32_sb_t *sb = ch->sb;
    uint8_t *p = (uint8_t *)buf;
    uint8_t sec[FAT32_SECTOR];

    while (len)
    {
        uint32_t ci = (uint32_t)(off / sb->clus_bytes);
        uint32_t within = (uint32_t)(off % sb->clus_bytes);
        uint32_t clus = fat32_chain_at(ch, ci);
        uint32_t sec_in, so, lba, n;

        if (!fat32_clus_valid(sb, clus))
            return FAT32_ERR_IO;
        sec_in = within / FAT32_SECTOR;
        so = within % FAT32_SECTOR;
        lba = fat32_clus_lba(sb, clus) + sec_in;
        n = FAT32_SECTOR - so;
        if (n > len)
            n = len;
        if (fat32_read_sec(sb, lba, sec) != 0)
            return FAT32_ERR_IO;
        kernel_memcpy(p, sec + so, n);
        off += n;
        p += n;
        len -= n;
    }
    return FAT32_OK;
}

static int fat32_chain_write(fat32_chain_t *ch, uint64_t off, const void *buf, uint32_t len)
{
    fat32_sb_t *sb = ch->sb;
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t sec[FAT32_SECTOR];

    while (len)
    {
        uint32_t ci = (uint32_t)(off / sb->clus_bytes);
        uint32_t within = (uint32_t)(off % sb->clus_bytes);
        uint32_t clus = fat32_chain_at(ch, ci);
        uint32_t sec_in, so, lba, n;

        if (!fat32_clus_valid(sb, clus))
        {
            /* 需要新簇：追加到链尾 */
            uint32_t nc = fat32_alloc_cluster(sb);
            if (!fat32_clus_valid(sb, nc))
                return VFS_ERR_NOSPC;
            if (!fat32_clus_valid(sb, ch->start))
            {
                if (ci != 0)
                    return FAT32_ERR_IO;
                ch->start = nc;
            }
            else
            {
                uint32_t last = fat32_chain_at(ch, ci - 1);
                if (!fat32_clus_valid(sb, last))
                    return FAT32_ERR_IO;
                if (fat32_fat_set(sb, last, nc) != FAT32_OK)
                    return FAT32_ERR_IO;
            }
            ch->cache_clus = nc;
            ch->cache_index = ci;
            ch->cache_ok = 1;
            clus = nc;
        }

        sec_in = within / FAT32_SECTOR;
        so = within % FAT32_SECTOR;
        lba = fat32_clus_lba(sb, clus) + sec_in;
        n = FAT32_SECTOR - so;
        if (n > len)
            n = len;

        if (so == 0 && n == FAT32_SECTOR)
        {
            if (fat32_write_sec(sb, lba, p) != 0)
                return FAT32_ERR_IO;
        }
        else
        {
            if (fat32_read_sec(sb, lba, sec) != 0)
                return FAT32_ERR_IO;
            kernel_memcpy(sec + so, (void *)p, n);
            if (fat32_write_sec(sb, lba, sec) != 0)
                return FAT32_ERR_IO;
        }
        off += n;
        p += n;
        len -= n;
    }
    return FAT32_OK;
}

/* ===== 目录项读写 ===== */
static int fat32_ent_read(fat32_sb_t *sb, fat32_chain_t *ch, uint32_t index,
                          uint8_t out[32], uint32_t *off_out)
{
    uint32_t byteoff = index * 32;
    uint32_t ci = byteoff / sb->clus_bytes;
    uint32_t within = byteoff % sb->clus_bytes;
    uint32_t clus = fat32_chain_at(ch, ci);
    uint32_t sec_in, so;
    uint8_t sec[FAT32_SECTOR];

    if (!fat32_clus_valid(sb, clus))
        return FAT32_ERR_EOF;
    sec_in = within / FAT32_SECTOR;
    so = within % FAT32_SECTOR;
    if (fat32_read_sec(sb, fat32_clus_lba(sb, clus) + sec_in, sec) != 0)
        return FAT32_ERR_IO;
    kernel_memcpy(out, sec + so, 32);
    if (off_out)
        *off_out = byteoff;
    return FAT32_OK;
}

static int fat32_ent_write(fat32_sb_t *sb, fat32_chain_t *ch, uint32_t index,
                           const uint8_t entry[32])
{
    uint32_t byteoff = index * 32;
    uint32_t ci = byteoff / sb->clus_bytes;
    uint32_t within = byteoff % sb->clus_bytes;
    uint32_t clus = fat32_chain_at(ch, ci);
    uint32_t sec_in, so;
    uint8_t sec[FAT32_SECTOR];

    if (!fat32_clus_valid(sb, clus))
        return FAT32_ERR_IO;
    sec_in = within / FAT32_SECTOR;
    so = within % FAT32_SECTOR;
    if (fat32_read_sec(sb, fat32_clus_lba(sb, clus) + sec_in, sec) != 0)
        return FAT32_ERR_IO;
    kernel_memcpy(sec + so, (void *)entry, 32);
    if (fat32_write_sec(sb, fat32_clus_lba(sb, clus) + sec_in, sec) != 0)
        return FAT32_ERR_IO;
    return FAT32_OK;
}

static int fat32_ent_write_direct(fat32_sb_t *sb, uint32_t dir_clus, uint32_t index,
                                  const uint8_t entry[32])
{
    fat32_chain_t ch;
    fat32_chain_init(&ch, sb, dir_clus);
    return fat32_ent_write(sb, &ch, index, entry);
}

/* ===== 名字处理 ===== */
static uint8_t fat32_upper(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
}

static uint8_t fat32_lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

static int fat32_name_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (fat32_upper((uint8_t)*a) != fat32_upper((uint8_t)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int fat32_valid_char(uint8_t c)
{
    static const char extra[] = "$%'-_@~`!(){}^#&";
    const char *p;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    for (p = extra; *p; p++)
        if ((uint8_t)*p == c)
            return 1;
    return 0;
}

static uint8_t fat32_short_sum(const uint8_t entry[32])
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++)
    {
        uint16_t v = (uint16_t)(((sum & 1u) << 7) | (sum >> 1));
        sum = (uint8_t)(v + entry[i]);
    }
    return sum;
}

/* LFN 项的 13 个 UTF-16 码元偏移 */
static const uint8_t g_lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static void fat32_lfn_store(uint16_t *units, uint32_t seq, const uint8_t *e)
{
    uint32_t base = (seq - 1) * 13;
    int i;
    for (i = 0; i < 13; i++)
        units[base + i] = rd16(e + g_lfn_off[i]);
}

static void fat32_lfn_name(const uint16_t *units, uint32_t max_seq, char *out, uint32_t outsz)
{
    uint32_t total = max_seq * 13;
    uint32_t i, j = 0;

    for (i = 0; i < total && j + 1 < outsz; i++)
    {
        uint16_t u = units[i];
        if (u == 0x0000)
            break;
        if (u == 0xffff)
            continue;
        out[j++] = (u < 0x80) ? (char)u : '?';
    }
    out[j] = '\0';
}

static void fat32_short_name(const uint8_t *e, char *out, uint32_t outsz)
{
    char base[9];
    char ext[4];
    int bl = 0, el = 0, i, j = 0;
    uint8_t nt = e[12];

    for (i = 0; i < 8; i++)
    {
        uint8_t c = e[i];
        if (c == ' ')
            break;
        if (i == 0 && c == 0x05)
            c = 0xe5;
        if (nt & 0x08)
            c = fat32_lower(c);
        base[bl++] = (char)c;
    }
    for (i = 0; i < 3; i++)
    {
        uint8_t c = e[8 + i];
        if (c == ' ')
            break;
        if (nt & 0x10)
            c = fat32_lower(c);
        ext[el++] = (char)c;
    }
    base[bl] = '\0';
    ext[el] = '\0';

    for (i = 0; i < bl && j + 1 < (int)outsz; i++)
        out[j++] = base[i];
    if (el && j + 1 < (int)outsz)
    {
        out[j++] = '.';
        for (i = 0; i < el && j + 1 < (int)outsz; i++)
            out[j++] = ext[i];
    }
    out[j] = '\0';
}

/* ===== 目录查找 / 路径解析 ===== */
static int fat32_dir_lookup(fat32_sb_t *sb, uint32_t dir_clus, const char *name, fat32_loc_t *out)
{
    fat32_chain_t ch;
    uint8_t e[32];
    uint16_t lfn[260];
    uint32_t lfn_max = 0;
    uint32_t lfn_start = 0;
    uint8_t lfn_sum = 0;
    int lfn_on = 0;
    uint32_t idx;

    fat32_chain_init(&ch, sb, dir_clus);
    for (idx = 0;; idx++)
    {
        uint32_t off;
        uint8_t attr;
        int rc = fat32_ent_read(sb, &ch, idx, e, &off);

        if (rc == FAT32_ERR_EOF)
            break;
        if (rc != FAT32_OK)
            return rc;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xe5)
        {
            lfn_on = 0;
            lfn_max = 0;
            continue;
        }
        attr = e[11];
        if (attr == FAT32_ATTR_LFN)
        {
            uint32_t seq = e[0] & 0x1f;
            if (!lfn_on)
            {
                lfn_on = 1;
                lfn_max = 0;
                lfn_start = off;
            }
            if (seq >= 1 && seq <= 20)
            {
                fat32_lfn_store(lfn, seq, e);
                if (seq > lfn_max)
                    lfn_max = seq;
                lfn_sum = e[13];
            }
            continue;
        }
        if (attr & FAT32_ATTR_VOL)
        {
            lfn_on = 0;
            lfn_max = 0;
            continue;
        }

        {
            char disp[VFS_NAME_MAX];
            if (lfn_on && lfn_max > 0 && fat32_short_sum(e) == lfn_sum)
                fat32_lfn_name(lfn, lfn_max, disp, sizeof(disp));
            else
                fat32_short_name(e, disp, sizeof(disp));

            if (fat32_name_eq(disp, name))
            {
                out->found = 1;
                out->dir_clus = dir_clus;
                out->off = off;
                out->lfn_off = lfn_on ? lfn_start : off;
                out->clus = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                out->size = rd32(e + 28);
                out->attr = attr;
                kernel_strncpy(out->name, disp, sizeof(out->name));
                return FAT32_OK;
            }
            lfn_on = 0;
            lfn_max = 0;
        }
    }
    out->found = 0;
    return FAT32_OK;
}

static int fat32_resolve_dir(fat32_sb_t *sb, const char *rel, uint32_t *out)
{
    uint32_t cur = sb->root_clus;
    const char *p = rel;

    while (*p)
    {
        char comp[VFS_NAME_MAX];
        uint32_t l = 0;
        fat32_loc_t loc;
        int rc;

        while (*p && *p != '/')
        {
            if (l + 1 < sizeof(comp))
                comp[l++] = *p;
            p++;
        }
        comp[l] = '\0';
        if (*p == '/')
            p++;
        if (l == 0)
            continue;

        rc = fat32_dir_lookup(sb, cur, comp, &loc);
        if (rc != FAT32_OK)
            return rc;
        if (!loc.found)
            return VFS_ERR_NOENT;
        if (!(loc.attr & FAT32_ATTR_DIR))
            return VFS_ERR_NOTDIR;
        cur = loc.clus;
    }
    *out = cur;
    return FAT32_OK;
}

static int fat32_split(const char *rel, char *dir, uint32_t dir_sz,
                       char *leaf, uint32_t leaf_sz)
{
    int n = (int)kernel_strlen(rel);
    int i = n;
    uint32_t dl, ll;

    while (i > 0 && rel[i - 1] != '/')
        i--;
    ll = (uint32_t)(n - i);
    if (ll == 0 || ll >= leaf_sz)
        return VFS_ERR_INVAL;
    kernel_memcpy(leaf, (void *)(rel + i), ll);
    leaf[ll] = '\0';

    dl = (i > 0) ? (uint32_t)(i - 1) : 0;
    if (dl >= dir_sz)
        return VFS_ERR_INVAL;
    kernel_memcpy(dir, (void *)rel, dl);
    dir[dl] = '\0';
    return FAT32_OK;
}

static int fat32_resolve(fat32_sb_t *sb, const char *rel, fat32_loc_t *out)
{
    char dir[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX];
    uint32_t parent;
    int rc;

    if (rel[0] == '\0')
    {
        kernel_memset(out, 0, sizeof(*out));
        out->found = 1;
        out->attr = FAT32_ATTR_DIR;
        out->clus = sb->root_clus;
        out->dir_clus = sb->root_clus;
        out->name[0] = '/';
        out->name[1] = '\0';
        return FAT32_OK;
    }

    rc = fat32_split(rel, dir, sizeof(dir), leaf, sizeof(leaf));
    if (rc != FAT32_OK)
        return rc;
    rc = fat32_resolve_dir(sb, dir, &parent);
    if (rc != FAT32_OK)
        return rc;
    rc = fat32_dir_lookup(sb, parent, leaf, out);
    if (rc != FAT32_OK)
        return rc;
    if (!out->found)
        return VFS_ERR_NOENT;
    return FAT32_OK;
}

/* ===== 新建目录项 ===== */
static int fat32_short_exists(fat32_sb_t *sb, uint32_t dir_clus, const uint8_t raw[11])
{
    fat32_chain_t ch;
    uint8_t e[32];
    uint32_t i;

    fat32_chain_init(&ch, sb, dir_clus);
    for (i = 0;; i++)
    {
        int rc = fat32_ent_read(sb, &ch, i, e, NULL);
        if (rc == FAT32_ERR_EOF)
            break;
        if (rc != FAT32_OK)
            return 0;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xe5 || e[11] == FAT32_ATTR_LFN)
            continue;
        if (kernel_memcmp(e, (void *)raw, 11) == 0)
            return 1;
    }
    return 0;
}

/* 生成 8.3 短名；name 含小写或过长/含非法字符时置 need_lfn */
static int fat32_make_short(fat32_sb_t *sb, uint32_t dir_clus, const char *name,
                            uint8_t raw[11], uint8_t *nt_res, int *need_lfn)
{
    int n = (int)kernel_strlen(name);
    int dot = -1;
    int i, bl = 0, el = 0, fit, has_lower = 0;
    char base[VFS_NAME_MAX];
    char ext[VFS_NAME_MAX];

    for (i = n - 1; i > 0; i--)
        if (name[i] == '.')
        {
            dot = i;
            break;
        }
    if (dot > 0 && dot < n - 1)
    {
        for (i = 0; i < dot; i++)
            base[bl++] = name[i];
        for (i = dot + 1; i < n; i++)
            ext[el++] = name[i];
    }
    else
    {
        for (i = 0; i < n; i++)
            base[bl++] = name[i];
    }
    base[bl] = '\0';
    ext[el] = '\0';

    for (i = 0; i < n; i++)
        if (name[i] >= 'a' && name[i] <= 'z')
            has_lower = 1;

    *need_lfn = has_lower;
    *nt_res = 0;

    fit = (bl >= 1 && bl <= 8 && el <= 3);
    for (i = 0; i < bl && fit; i++)
        if (!fat32_valid_char((uint8_t)base[i]))
            fit = 0;
    for (i = 0; i < el && fit; i++)
        if (!fat32_valid_char((uint8_t)ext[i]))
            fit = 0;

    if (fit)
    {
        kernel_memset(raw, ' ', 11);
        for (i = 0; i < bl; i++)
            raw[i] = fat32_upper((uint8_t)base[i]);
        for (i = 0; i < el; i++)
            raw[8 + i] = fat32_upper((uint8_t)ext[i]);
        if (!fat32_short_exists(sb, dir_clus, raw))
            return FAT32_OK;
        /* 名字冲突则退化为 ~n */
    }

    {
        char cb[32];
        char ce[8];
        int cl = 0, cel = 0;

        for (i = 0; i < bl && cl < (int)sizeof(cb) - 1; i++)
        {
            uint8_t c = fat32_upper((uint8_t)base[i]);
            cb[cl++] = fat32_valid_char(c) ? (char)c : '_';
        }
        if (cl == 0)
        {
            cb[cl++] = 'F';
            cb[cl++] = 'I';
            cb[cl++] = 'L';
            cb[cl++] = 'E';
        }
        cb[cl] = '\0';
        for (i = 0; i < el && cel < (int)sizeof(ce) - 1; i++)
        {
            uint8_t c = fat32_upper((uint8_t)ext[i]);
            ce[cel++] = fat32_valid_char(c) ? (char)c : '_';
        }
        ce[cel] = '\0';

        for (i = 1; i < 1000; i++)
        {
            char num[8];
            int nl = 0, t = i, tl = 0, head, use, k;
            char tmp[8];

            while (t > 0)
            {
                tmp[tl++] = (char)('0' + (t % 10));
                t /= 10;
            }
            while (tl > 0)
                num[nl++] = tmp[--tl];
            num[nl] = '\0';

            head = 8 - 1 - nl;
            if (head < 1)
                head = 1;
            use = (cl < head) ? cl : head;
            kernel_memset(raw, ' ', 11);
            for (k = 0; k < use; k++)
                raw[k] = (uint8_t)cb[k];
            raw[use] = '~';
            for (k = 0; k < nl && use + 1 + k < 8; k++)
                raw[use + 1 + k] = (uint8_t)num[k];
            for (k = 0; k < cel && k < 3; k++)
                raw[8 + k] = (uint8_t)ce[k];
            if (!fat32_short_exists(sb, dir_clus, raw))
            {
                *need_lfn = 1;
                return FAT32_OK;
            }
        }
    }
    return VFS_ERR_NOSPC;
}

static void fat32_build_lfn(const char *name, const uint8_t raw[11],
                            uint8_t *entries, int *count)
{
    int len = (int)kernel_strlen(name);
    int n = (len + 12) / 13;
    uint8_t sum = fat32_short_sum(raw);
    int seq, k;

    if (n < 1)
        n = 1;
    if (n > 20)
        n = 20;

    for (seq = n; seq >= 1; seq--)
    {
        uint8_t *e = entries + (n - seq) * 32;
        kernel_memset(e, 0, 32);
        for (k = 0; k < 13; k++)
        {
            int idx = (seq - 1) * 13 + k;
            uint16_t u;
            if (idx < len)
                u = (uint8_t)name[idx];
            else if (idx == len)
                u = 0x0000;
            else
                u = 0xffff;
            wr16(e + g_lfn_off[k], u);
        }
        e[0] = (uint8_t)(seq | (seq == n ? 0x40 : 0x00));
        e[11] = FAT32_ATTR_LFN;
        e[13] = sum;
    }
    *count = n;
}

static void fat32_make_slot(uint8_t e[32], const uint8_t raw[11], uint8_t attr,
                            uint8_t nt_res, uint32_t clus, uint32_t size)
{
    kernel_memset(e, 0, 32);
    kernel_memcpy(e, (void *)raw, 11);
    e[11] = attr;
    e[12] = nt_res;
    wr16(e + 14, FAT32_TIME);
    wr16(e + 16, FAT32_DATE);
    wr16(e + 18, FAT32_DATE);
    wr16(e + 20, (uint16_t)((clus >> 16) & 0xffff));
    wr16(e + 22, FAT32_TIME);
    wr16(e + 24, FAT32_DATE);
    wr16(e + 26, (uint16_t)(clus & 0xffff));
    wr32(e + 28, size);
}

static int fat32_update_entry(fat32_sb_t *sb, uint32_t dir_clus, uint32_t off32,
                              uint32_t clus, uint32_t size)
{
    fat32_chain_t ch;
    uint8_t e[32];
    uint32_t idx = off32 / 32;

    fat32_chain_init(&ch, sb, dir_clus);
    if (fat32_ent_read(sb, &ch, idx, e, NULL) != FAT32_OK)
        return FAT32_ERR_IO;
    wr16(e + 20, (uint16_t)((clus >> 16) & 0xffff));
    wr16(e + 26, (uint16_t)(clus & 0xffff));
    wr32(e + 28, size);
    return fat32_ent_write(sb, &ch, idx, e);
}

static int fat32_find_slots(fat32_sb_t *sb, fat32_chain_t *ch, uint32_t need,
                            uint32_t *out_idx, int *out_grow)
{
    uint32_t total = fat32_chain_count(ch) * sb->clus_bytes;
    uint32_t run = 0, run_start = 0, i;

    *out_grow = 0;
    for (i = 0;; i++)
    {
        uint32_t byteoff = i * 32;
        uint8_t e[32];
        int rc;

        if (byteoff >= total)
        {
            *out_idx = i;
            *out_grow = 1;
            return FAT32_OK;
        }
        rc = fat32_ent_read(sb, ch, i, e, NULL);
        if (rc == FAT32_ERR_EOF)
        {
            *out_idx = i;
            *out_grow = 1;
            return FAT32_OK;
        }
        if (rc != FAT32_OK)
            return rc;
        if (e[0] == 0x00)
        {
            *out_idx = i;
            *out_grow = (total - byteoff < need * 32) ? 1 : 0;
            return FAT32_OK;
        }
        if (e[0] == 0xe5)
        {
            if (run == 0)
                run_start = i;
            run++;
            if (run >= need)
            {
                *out_idx = run_start;
                *out_grow = 0;
                return FAT32_OK;
            }
        }
        else
        {
            run = 0;
        }
    }
}

static int fat32_dir_grow(fat32_sb_t *sb, fat32_chain_t *ch, uint32_t *out_total)
{
    uint32_t n = fat32_chain_count(ch);
    uint32_t last = fat32_chain_at(ch, n - 1);
    uint32_t nc;

    if (!fat32_clus_valid(sb, last))
        return FAT32_ERR_IO;
    nc = fat32_alloc_cluster(sb);
    if (!fat32_clus_valid(sb, nc))
        return VFS_ERR_NOSPC;
    if (fat32_fat_set(sb, last, nc) != FAT32_OK)
        return FAT32_ERR_IO;
    ch->cache_ok = 0;
    *out_total = (n + 1) * sb->clus_bytes;
    return FAT32_OK;
}

static int fat32_dir_insert(fat32_sb_t *sb, uint32_t dir_clus, const char *name,
                            int is_dir, uint32_t first_clus, uint32_t size,
                            fat32_loc_t *out)
{
    fat32_chain_t ch;
    uint8_t raw[11];
    uint8_t nt_res = 0;
    int need_lfn = 0;
    uint8_t lfns[20 * 32];
    int lfn_count = 0;
    uint32_t idx = 0, total;
    int grow = 0;
    int rc, i;

    rc = fat32_make_short(sb, dir_clus, name, raw, &nt_res, &need_lfn);
    if (rc != FAT32_OK)
        return rc;
    if (need_lfn)
        fat32_build_lfn(name, raw, lfns, &lfn_count);
    if (lfn_count > 20)
        lfn_count = 20;

    fat32_chain_init(&ch, sb, dir_clus);
    rc = fat32_find_slots(sb, &ch, (uint32_t)(lfn_count + 1), &idx, &grow);
    if (rc != FAT32_OK)
        return rc;

    total = fat32_chain_count(&ch) * sb->clus_bytes;
    if (grow)
        idx = total / 32;
    while ((uint64_t)(idx + (uint32_t)(lfn_count + 1)) * 32 > total)
    {
        uint32_t ntotal;
        rc = fat32_dir_grow(sb, &ch, &ntotal);
        if (rc != FAT32_OK)
            return rc;
        total = ntotal;
    }

    for (i = 0; i < lfn_count; i++)
    {
        rc = fat32_ent_write(sb, &ch, idx + (uint32_t)i, lfns + i * 32);
        if (rc != FAT32_OK)
            return rc;
    }
    {
        uint8_t se[32];
        fat32_make_slot(se, raw, is_dir ? FAT32_ATTR_DIR : FAT32_ATTR_ARCH,
                        nt_res, first_clus, size);
        rc = fat32_ent_write(sb, &ch, idx + (uint32_t)lfn_count, se);
        if (rc != FAT32_OK)
            return rc;
    }

    out->found = 1;
    out->dir_clus = dir_clus;
    out->off = (idx + (uint32_t)lfn_count) * 32;
    out->lfn_off = idx * 32;
    out->clus = first_clus;
    out->size = size;
    out->attr = (uint8_t)(is_dir ? FAT32_ATTR_DIR : FAT32_ATTR_ARCH);
    kernel_strncpy(out->name, name, sizeof(out->name));
    return FAT32_OK;
}

static int fat32_delete_slots(fat32_sb_t *sb, uint32_t dir_clus, uint32_t from, uint32_t to)
{
    fat32_chain_t ch;
    uint32_t i0 = from / 32;
    uint32_t i1 = to / 32;
    uint32_t i;

    fat32_chain_init(&ch, sb, dir_clus);
    for (i = i0; i <= i1; i++)
    {
        uint8_t e[32];
        if (fat32_ent_read(sb, &ch, i, e, NULL) != FAT32_OK)
            break;
        e[0] = 0xe5;
        if (fat32_ent_write(sb, &ch, i, e) != FAT32_OK)
            break;
    }
    return FAT32_OK;
}

static int fat32_dir_is_empty(fat32_sb_t *sb, uint32_t clus)
{
    fat32_chain_t ch;
    uint8_t e[32];
    uint32_t i;

    fat32_chain_init(&ch, sb, clus);
    for (i = 0;; i++)
    {
        int rc = fat32_ent_read(sb, &ch, i, e, NULL);
        if (rc == FAT32_ERR_EOF)
            break;
        if (rc != FAT32_OK)
            return 0;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xe5)
            continue;
        if (e[11] == FAT32_ATTR_LFN)
            continue;
        if (e[11] & FAT32_ATTR_VOL)
            continue;
        if (e[0] == '.')
            continue; /* "." 与 ".." */
        return 0;
    }
    return 1;
}

static int fat32_create_dir(fat32_sb_t *sb, uint32_t parent, const char *leaf,
                            fat32_loc_t *out)
{
    uint32_t clus = fat32_alloc_cluster(sb);
    uint8_t raw[11];
    uint8_t e[32];
    int rc;

    if (!fat32_clus_valid(sb, clus))
        return VFS_ERR_NOSPC;
    rc = fat32_dir_insert(sb, parent, leaf, 1, clus, 0, out);
    if (rc != FAT32_OK)
    {
        fat32_free_chain(sb, clus);
        return rc;
    }

    kernel_memset(raw, ' ', 11);
    raw[0] = '.';
    fat32_make_slot(e, raw, FAT32_ATTR_DIR, 0, clus, 0);
    fat32_ent_write_direct(sb, clus, 0, e);

    kernel_memset(raw, ' ', 11);
    raw[0] = '.';
    raw[1] = '.';
    fat32_make_slot(e, raw, FAT32_ATTR_DIR, 0,
                    (parent == sb->root_clus) ? 0 : parent, 0);
    fat32_ent_write_direct(sb, clus, 1, e);
    return FAT32_OK;
}

static void fat32_truncate(fat32_sb_t *sb, fat32_loc_t *loc, fat32_file_t *ff)
{
    if (fat32_clus_valid(sb, loc->clus))
        fat32_free_chain(sb, loc->clus);
    loc->clus = 0;
    loc->size = 0;
    fat32_update_entry(sb, loc->dir_clus, loc->off, 0, 0);
    if (ff)
    {
        ff->chain.start = 0;
        ff->chain.cache_ok = 0;
        ff->size = 0;
    }
}

/* ===== 引导扇区识别 ===== */
static int fat32_is_dbr(const uint8_t *sec)
{
    uint16_t bps = rd16(sec + 11);
    uint8_t spc = sec[13];
    uint32_t root;

    if (rd16(sec + 510) != 0xaa55)
        return 0;
    if (bps != 512)
        return 0; /* 驱动固定按 512 字节扇区工作 */
    if (spc == 0 || (spc & (spc - 1)))
        return 0;
    if (sec[16] == 0)
        return 0;
    if (rd16(sec + 17) != 0) /* FAT32 根目录项数必须为 0 */
        return 0;
    if (rd16(sec + 22) != 0) /* FAT32 FAT16 大小必须为 0 */
        return 0;
    if (rd32(sec + 36) == 0)
        return 0;
    root = rd32(sec + 44);
    if (root < 2)
        return 0;
    return 1;
}

static uint32_t fat32_mbr_find(const uint8_t *sec)
{
    int i;
    if (rd16(sec + 510) != 0xaa55)
        return 0;
    for (i = 0; i < 4; i++)
    {
        const uint8_t *pe = sec + 446 + i * 16;
        uint8_t type = pe[4];
        if (type == 0x0b || type == 0x0c)
        {
            uint32_t lba = rd32(pe + 8);
            if (lba != 0)
                return lba;
        }
    }
    return 0;
}

static int fat32_mount(vfs_mount_t *m, const char *dev)
{
    fat32_sb_t *sb;
    uint8_t sec[FAT32_SECTOR];
    vdev_t *vd;
    uint32_t base = 0;
    uint16_t extflags;
    uint32_t data_secs;

    if (!m || !dev || dev[0] == '\0')
        return VFS_ERR_INVAL;
    vd = vdev_find(dev);
    if (!vd || vd->type != VDEV_BLOCK || !vd->bops || !vd->bops->read_blocks)
        return VFS_ERR_NODEV;
    if (vd->block_size != FAT32_SECTOR)
        return VFS_ERR_NOTSUP;

    sb = (fat32_sb_t *)kmalloc(sizeof(*sb));
    if (!sb)
        return VFS_ERR_NOMEM;
    kernel_memset(sb, 0, (uint32_t)sizeof(*sb));
    spin_lock_init(&sb->lock);
    sb->dev = vd;

    if (vd->bops->read_blocks(vd, 0, 1, sec) != 0)
    {
        kfree(sb);
        return VFS_ERR_NODEV;
    }
    if (!fat32_is_dbr(sec))
    {
        base = fat32_mbr_find(sec);
        if (base == 0 ||
            vd->bops->read_blocks(vd, base, 1, sec) != 0 ||
            !fat32_is_dbr(sec))
        {
            kfree(sb);
            return VFS_ERR_NOTSUP;
        }
    }

    sb->base_lba = base;
    sb->sec_per_clus = sec[13];
    sb->reserved_secs = rd16(sec + 14);
    sb->num_fats = sec[16];
    sb->total_secs = rd32(sec + 32);
    sb->fat_secs = rd32(sec + 36);
    sb->root_clus = rd32(sec + 44);
    sb->fs_info_sec = rd16(sec + 48);
    extflags = rd16(sec + 40);
    sb->mirror = (extflags & 0x0080) ? 0 : 1;
    sb->active_fat = (uint8_t)(extflags & 0x000f);
    if (sb->active_fat >= sb->num_fats)
        sb->active_fat = 0;
    sb->clus_bytes = sb->sec_per_clus * FAT32_SECTOR;
    sb->fat_start_lba = sb->reserved_secs;

    data_secs = sb->total_secs > (sb->reserved_secs + sb->num_fats * sb->fat_secs)
                    ? sb->total_secs - (sb->reserved_secs + sb->num_fats * sb->fat_secs)
                    : 0;
    sb->cluster_count = data_secs / sb->sec_per_clus;
    sb->data_start_lba = sb->fat_start_lba + sb->num_fats * sb->fat_secs;
    sb->next_free = 2;
    sb->free_known = 0;

    if (sb->fs_info_sec)
    {
        uint8_t fi[FAT32_SECTOR];
        if (fat32_read_sec(sb, sb->fs_info_sec, fi) == 0 &&
            rd32(fi) == 0x41615252u && rd32(fi + 484) == 0x61417272u)
        {
            uint32_t fc = rd32(fi + 488);
            uint32_t nf = rd32(fi + 492);
            if (fc != 0xffffffffu)
            {
                sb->free_count = fc;
                sb->free_known = 1;
            }
            if (fat32_clus_valid(sb, nf))
                sb->next_free = nf;
        }
    }

    if (sb->cluster_count < 2 || sb->clus_bytes == 0)
    {
        kfree(sb);
        return VFS_ERR_NOTSUP;
    }

    m->priv = sb;
    fput_string("[FAT32] mount %s base=%u spc=%u fats=%u fat_secs=%u root=%u clusters=%u bps=512\n",
                dev, (unsigned)sb->base_lba, (unsigned)sb->sec_per_clus,
                (unsigned)sb->num_fats, (unsigned)sb->fat_secs, (unsigned)sb->root_clus,
                (unsigned)sb->cluster_count);
    return VFS_OK;
}

static int fat32_unmount(vfs_mount_t *m)
{
    if (m->priv)
        kfree(m->priv);
    m->priv = NULL;
    return VFS_OK;
}

static int fat32_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    fat32_sb_t *sb = (fat32_sb_t *)m->priv;
    fat32_loc_t loc;
    fat32_file_t *ff;
    uint64_t irq;
    int want_dir = (flags & VFS_O_DIRECTORY) != 0;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    rc = fat32_resolve(sb, rel, &loc);
    if (rc == VFS_ERR_NOENT && (flags & VFS_O_CREAT) && !want_dir)
    {
        char dir[VFS_PATH_MAX];
        char leaf[VFS_NAME_MAX];

        rc = fat32_split(rel, dir, sizeof(dir), leaf, sizeof(leaf));
        if (rc == FAT32_OK)
        {
            uint32_t parent;
            rc = fat32_resolve_dir(sb, dir, &parent);
            if (rc == FAT32_OK)
            {
                fat32_loc_t ex;
                rc = fat32_dir_lookup(sb, parent, leaf, &ex);
                if (rc == FAT32_OK && ex.found)
                {
                    loc = ex;
                    rc = FAT32_OK;
                }
                else if (rc == FAT32_OK)
                {
                    rc = fat32_dir_insert(sb, parent, leaf, 0, 0, 0, &loc);
                }
            }
        }
    }
    if (rc != FAT32_OK)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return rc;
    }

    if ((loc.attr & FAT32_ATTR_DIR) && !want_dir)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_ISDIR;
    }
    if (!(loc.attr & FAT32_ATTR_DIR) && want_dir)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOTDIR;
    }

    ff = (fat32_file_t *)kmalloc(sizeof(*ff));
    if (!ff)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_ERR_NOMEM;
    }
    kernel_memset(ff, 0, (uint32_t)sizeof(*ff));
    fat32_chain_init(&ff->chain, sb, loc.clus);
    ff->size = loc.size;
    ff->dir_clus = loc.dir_clus;
    ff->dir_off = loc.off;
    ff->is_dir = (loc.attr & FAT32_ATTR_DIR) ? 1 : 0;

    if (!ff->is_dir && (flags & VFS_O_TRUNC) && (flags & 0x03) != VFS_O_RDONLY)
        fat32_truncate(sb, &loc, ff);

    f->priv = ff;
    f->is_dir = (uint32_t)ff->is_dir;
    f->size = ff->size;
    f->pos = (!ff->is_dir && (flags & VFS_O_APPEND)) ? ff->size : 0;

    spin_unlock_irqrestore(&sb->lock, irq);
    return VFS_OK;
}

static int fat32_close(vfs_file_t *f)
{
    if (f->priv)
    {
        kfree(f->priv);
        f->priv = NULL;
    }
    return VFS_OK;
}

static int64_t fat32_read(vfs_file_t *f, void *buf, uint64_t len)
{
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    fat32_sb_t *sb;
    uint64_t irq;
    int rc;

    if (!ff)
        return VFS_ERR_INVAL;
    if (ff->is_dir)
        return VFS_ERR_ISDIR;
    sb = ff->chain.sb;

    irq = spin_lock_irqsave(&sb->lock);
    if (f->pos >= ff->size)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return 0;
    }
    if (len > ff->size - f->pos)
        len = ff->size - f->pos;
    if (len > 0xffffffffu)
        len = 0xffffffffu;

    rc = fat32_chain_read(&ff->chain, f->pos, buf, (uint32_t)len);
    if (rc != FAT32_OK)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return rc;
    }
    f->pos += len;
    spin_unlock_irqrestore(&sb->lock, irq);
    return (int64_t)len;
}

static int64_t fat32_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    fat32_sb_t *sb;
    uint8_t zbuf[128];
    uint64_t irq;
    int rc;

    if (!ff)
        return VFS_ERR_INVAL;
    if (ff->is_dir)
        return VFS_ERR_ISDIR;
    sb = ff->chain.sb;
    if (len > 0xffffffffu)
        len = 0xffffffffu;

    irq = spin_lock_irqsave(&sb->lock);

    /* seek 越过 EOF 的空洞按 0 填充 */
    if (f->pos > ff->size)
    {
        uint64_t gap = f->pos - ff->size;
        uint64_t gpos = ff->size;
        kernel_memset(zbuf, 0, sizeof(zbuf));
        while (gap)
        {
            uint32_t n = (gap > sizeof(zbuf)) ? (uint32_t)sizeof(zbuf) : (uint32_t)gap;
            rc = fat32_chain_write(&ff->chain, gpos, zbuf, n);
            if (rc != FAT32_OK)
            {
                spin_unlock_irqrestore(&sb->lock, irq);
                return rc;
            }
            gpos += n;
            gap -= n;
        }
    }

    rc = fat32_chain_write(&ff->chain, f->pos, buf, (uint32_t)len);
    if (rc != FAT32_OK)
    {
        spin_unlock_irqrestore(&sb->lock, irq);
        return rc;
    }
    f->pos += len;
    if (f->pos > ff->size)
        ff->size = (uint32_t)f->pos;

    fat32_update_entry(sb, ff->dir_clus, ff->dir_off, ff->chain.start, ff->size);
    f->size = ff->size;
    spin_unlock_irqrestore(&sb->lock, irq);
    return (int64_t)len;
}

static int64_t fat32_seek(vfs_file_t *f, int64_t off, int whence)
{
    fat32_file_t *ff = (fat32_file_t *)f->priv;
    int64_t base;

    if (!ff)
        return VFS_ERR_INVAL;
    if (ff->is_dir)
        return VFS_ERR_ISDIR;
    if (whence == VFS_SEEK_SET)
        base = off;
    else if (whence == VFS_SEEK_CUR)
        base = (int64_t)f->pos + off;
    else
        base = (int64_t)ff->size + off;
    if (base < 0)
        return VFS_ERR_INVAL;
    f->pos = (uint64_t)base;
    return (int64_t)f->pos;
}

static int fat32_stat(vfs_mount_t *m, const char *rel, uint64_t *size_out, uint32_t *is_dir_out)
{
    fat32_sb_t *sb = (fat32_sb_t *)m->priv;
    fat32_loc_t loc;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = fat32_resolve(sb, rel, &loc);
    if (rc == FAT32_OK)
    {
        if (size_out)
            *size_out = loc.size;
        if (is_dir_out)
            *is_dir_out = (loc.attr & FAT32_ATTR_DIR) ? 1 : 0;
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int fat32_mkdir(vfs_mount_t *m, const char *rel)
{
    fat32_sb_t *sb = (fat32_sb_t *)m->priv;
    char dir[VFS_PATH_MAX];
    char leaf[VFS_NAME_MAX];
    uint32_t parent;
    fat32_loc_t loc;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = fat32_split(rel, dir, sizeof(dir), leaf, sizeof(leaf));
    if (rc == FAT32_OK)
    {
        rc = fat32_resolve_dir(sb, dir, &parent);
        if (rc == FAT32_OK)
        {
            rc = fat32_dir_lookup(sb, parent, leaf, &loc);
            if (rc == FAT32_OK)
            {
                if (loc.found)
                    rc = VFS_ERR_EXIST;
                else
                    rc = fat32_create_dir(sb, parent, leaf, &loc);
            }
        }
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int fat32_rmdir(vfs_mount_t *m, const char *rel)
{
    fat32_sb_t *sb = (fat32_sb_t *)m->priv;
    fat32_loc_t loc;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = fat32_resolve(sb, rel, &loc);
    if (rc == FAT32_OK)
    {
        if (!(loc.attr & FAT32_ATTR_DIR))
            rc = VFS_ERR_NOTDIR;
        else if (!fat32_dir_is_empty(sb, loc.clus))
            rc = VFS_ERR_NOTEMPTY;
        else
        {
            fat32_free_chain(sb, loc.clus);
            fat32_delete_slots(sb, loc.dir_clus, loc.lfn_off, loc.off);
        }
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int fat32_opendir(vfs_mount_t *m, const char *rel, vfs_dir_t *d)
{
    fat32_sb_t *sb = (fat32_sb_t *)m->priv;
    fat32_loc_t loc;
    fat32_dirh_t *h;
    uint64_t irq;
    int rc;

    if (!sb)
        return VFS_ERR_INVAL;
    irq = spin_lock_irqsave(&sb->lock);
    rc = fat32_resolve(sb, rel, &loc);
    if (rc == FAT32_OK)
    {
        if (!(loc.attr & FAT32_ATTR_DIR))
            rc = VFS_ERR_NOTDIR;
        else
        {
            h = (fat32_dirh_t *)kmalloc(sizeof(*h));
            if (!h)
                rc = VFS_ERR_NOMEM;
            else
            {
                h->dir_clus = loc.clus;
                h->next_index = 0;
                d->priv = h;
                d->pos = NULL;
            }
        }
    }
    spin_unlock_irqrestore(&sb->lock, irq);
    return rc;
}

static int fat32_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    fat32_dirh_t *h = (fat32_dirh_t *)d->priv;
    fat32_sb_t *sb = (fat32_sb_t *)d->mnt->priv;
    fat32_chain_t ch;
    uint8_t e[32];
    uint16_t lfn[260];
    uint32_t lfn_max = 0;
    uint8_t lfn_sum = 0;
    int lfn_on = 0;
    uint64_t irq;
    int rc;

    if (!sb || !h)
        return VFS_ERR_INVAL;

    irq = spin_lock_irqsave(&sb->lock);
    fat32_chain_init(&ch, sb, h->dir_clus);
    for (;;)
    {
        uint8_t attr;
        char disp[VFS_NAME_MAX];

        rc = fat32_ent_read(sb, &ch, h->next_index, e, NULL);
        if (rc == FAT32_ERR_EOF)
        {
            spin_unlock_irqrestore(&sb->lock, irq);
            return VFS_ERR_NOENT;
        }
        if (rc != FAT32_OK)
        {
            spin_unlock_irqrestore(&sb->lock, irq);
            return rc;
        }
        h->next_index++;

        if (e[0] == 0x00)
        {
            spin_unlock_irqrestore(&sb->lock, irq);
            return VFS_ERR_NOENT;
        }
        if (e[0] == 0xe5)
        {
            lfn_on = 0;
            lfn_max = 0;
            continue;
        }
        attr = e[11];
        if (attr == FAT32_ATTR_LFN)
        {
            uint32_t seq = e[0] & 0x1f;
            if (!lfn_on)
            {
                lfn_on = 1;
                lfn_max = 0;
            }
            if (seq >= 1 && seq <= 20)
            {
                fat32_lfn_store(lfn, seq, e);
                if (seq > lfn_max)
                    lfn_max = seq;
                lfn_sum = e[13];
            }
            continue;
        }
        if (attr & FAT32_ATTR_VOL)
        {
            lfn_on = 0;
            lfn_max = 0;
            continue;
        }

        if (lfn_on && lfn_max > 0 && fat32_short_sum(e) == lfn_sum)
            fat32_lfn_name(lfn, lfn_max, disp, sizeof(disp));
        else
            fat32_short_name(e, disp, sizeof(disp));
        lfn_on = 0;
        lfn_max = 0;

        if (disp[0] == '.')
            continue; /* 跳过 "." 与 ".." */

        kernel_strncpy(out->name, disp, sizeof(out->name));
        out->is_dir = (attr & FAT32_ATTR_DIR) ? 1 : 0;
        out->size = rd32(e + 28);
        spin_unlock_irqrestore(&sb->lock, irq);
        return VFS_OK;
    }
}

static int fat32_closedir(vfs_dir_t *d)
{
    if (d->priv)
    {
        kfree(d->priv);
        d->priv = NULL;
    }
    return VFS_OK;
}

const fs_driver_t fat32_driver = {
    .name = "fat32",
    .mount = fat32_mount,
    .unmount = fat32_unmount,
    .open = fat32_open,
    .close = fat32_close,
    .read = fat32_read,
    .write = fat32_write,
    .seek = fat32_seek,
    .stat = fat32_stat,
    .mkdir = fat32_mkdir,
    .rmdir = fat32_rmdir,
    .opendir = fat32_opendir,
    .readdir = fat32_readdir,
    .closedir = fat32_closedir,
};
