#include <fs/ext4.h>
#include <vdev.h>
#include <memory/heap.h>
#include <serial.h>
#include <string.h>

#define EXT4_MAGIC 0xef53u
#define EXT4_EH_MAGIC 0xf30au
#define EXT4_ROOT_INO 2u

/* s_feature_incompat：本驱动可解析的子集 */
#define EXT4_FT_FILETYPE 0x0002u
#define EXT4_FT_EXTENTS 0x0040u
#define EXT4_FT_64BIT 0x0080u
#define EXT4_INCOMPAT_SUPP (EXT4_FT_FILETYPE | EXT4_FT_EXTENTS | EXT4_FT_64BIT)

/* i_flags */
#define EXT4_EXTENTS_FL 0x00080000u

/* i_mode 文件类型 */
#define EXT4_S_IFMT 0xf000u
#define EXT4_S_IFDIR 0x4000u
#define EXT4_S_IFREG 0x8000u
#define EXT4_S_IFLNK 0xa000u

/* 目录项文件类型 */
#define EXT4_FT_DIR 2u

/* 本驱动只支持 block_size == 512 的 vdev（见 ext4_mount 校验）；
 * 不直接用 ATA_SECTOR_SIZE：那是 ATA 层的常量，FS 应对接 vdev 抽象。
 * fat32/part 同样各自定义本地常量（FAT32_SECTOR/PART_SEC_SIZE）。 */
#define EXT4_DEV_BLK 512u

/* extent 未初始化位（ee_len 最高位）：读出为全 0 */
#define EXT4_EE_UNINIT 0x8000u
#define EXT4_EE_LEN_MASK 0x7fffu

#define EXT4_MAX_DEPTH 5

typedef struct ext4_sb
{
    vdev_t *dev;               /* 内存：所属块设备，非盘上字段 */
    uint32_t bs;               /* 派生：s_log_block_size（超块+24），bs = 1024 << log_bs */
    uint32_t blk_per_fs;       /* 派生：bs / EXT4_DEV_BLK，非盘上字段 */
    uint64_t total_blocks;     /* 直读：s_blocks_count_lo（超块+4）| 64bit时拼 s_blocks_count_hi（超块+336）<< 32 */
    uint32_t blocks_per_group; /* 直读：s_blocks_per_group（超块+32） */
    uint32_t inodes_per_group; /* 直读：s_inodes_per_group（超块+40） */
    uint32_t ngroups;          /* 派生：ceil(total_blocks / blocks_per_group)，非盘上字段 */
    uint32_t inode_size;       /* 直读：s_inode_size（超块+88） */
    uint32_t desc_size;        /* 直读：s_desc_size（超块+254，仅 64bit 有效，否则固定 32） */
    uint32_t is_64bit;         /* 派生：s_feature_incompat（超块+96）bit 0x80 是否置位 */
    uint8_t *gd;               /* 内存：读入的组描述符表（ngroups * desc_size），非盘上字段 */
} ext4_sb_t;

typedef struct ext4_inode
{
    uint32_t ino;    /* 内存：传入的 inode 号，非盘上字段 */
    uint16_t mode;   /* 直读：i_mode（inode+0，2 字节），类型+权限 */
    uint64_t size;   /* 直读：i_size_lo（inode+4）| i_size_hi（inode+108）<< 32 */
    uint32_t flags;  /* 直读：i_flags（inode+32），如 EXTENTS_FL 判 extent 文件 */
    uint32_t is_dir; /* 派生：(mode & IFMT) == IFDIR 的缓存 */
    uint32_t is_reg; /* 派生：(mode & IFMT) == IFREG 的缓存 */
    uint8_t blk[60]; /* 直读：i_block（inode+40，60 字节），extent 树根 */
} ext4_inode_t;

static uint16_t ext4_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ext4_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* fs 块号读写：换算为设备 LBA 后直通分区 vdev */
static int ext4_read_fs_blocks(ext4_sb_t *sb, uint64_t fsblk, uint32_t count, void *buf)
{
    uint64_t lba;
    uint64_t n;

    if (count == 0 || buf == NULL)
        return VFS_ERR_INVAL;
    lba = fsblk * sb->blk_per_fs;
    n = (uint64_t)count * sb->blk_per_fs;
    if (n > 0xffffffffu)
        return VFS_ERR_INVAL;
    return sb->dev->bops->read_blocks(sb->dev, lba, (uint32_t)n, buf);
}

static uint64_t ext4_itab_block(ext4_sb_t *sb, uint32_t group)
{
    const uint8_t *d = sb->gd + (uint64_t)group * sb->desc_size;
    uint64_t lo = ext4_rd32(d + 8);

    if (sb->is_64bit)
        lo |= (uint64_t)ext4_rd32(d + 40) << 32; /* bg_inode_table_hi，见组描述符 64 字节布局 */
    return lo;
}

static int ext4_read_inode(ext4_sb_t *sb, uint32_t ino, ext4_inode_t *out)
{
    uint8_t *blk;
    uint32_t group, idx;
    uint64_t off, fsblk, inner;
    const uint8_t *p;
    uint16_t mode;
    int rc;

    if (!sb || !out || ino == 0)
        return VFS_ERR_INVAL;
    if ((uint64_t)ino > (uint64_t)sb->ngroups * sb->inodes_per_group)
        return VFS_ERR_NOENT;
    group = (ino - 1) / sb->inodes_per_group;
    idx = (ino - 1) % sb->inodes_per_group;
    off = (uint64_t)idx * sb->inode_size;
    fsblk = ext4_itab_block(sb, group) + off / sb->bs;
    inner = off % sb->bs;
    if (inner + sb->inode_size > sb->bs)
        return VFS_ERR_NOTSUP; /* inode 跨块：本实现不支持 */

    blk = (uint8_t *)kmalloc(sb->bs);
    if (!blk)
        return VFS_ERR_NOMEM;
    rc = ext4_read_fs_blocks(sb, fsblk, 1, blk);
    if (rc != 0)
    {
        kfree(blk);
        return rc;
    }
    p = blk + inner;
    mode = ext4_rd16(p);
    out->ino = ino;
    out->mode = mode;
    out->size = (uint64_t)ext4_rd32(p + 4) | ((uint64_t)ext4_rd32(p + 108) << 32);
    out->flags = ext4_rd32(p + 32);
    out->is_dir = (mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
    out->is_reg = (mode & EXT4_S_IFMT) == EXT4_S_IFREG;
    kernel_memcpy(out->blk, (void *)(p + 40), 60);
    kfree(blk);
    return VFS_OK;
}

/* extent 树内单层查找：leaf 输出物理块与未初始化标记 */
static int ext4_map_walk(const uint8_t *node, uint32_t node_len, uint32_t lb,
                         uint64_t *phys_out, int *uninit_out,
                         ext4_sb_t *sb, uint8_t *scratch, int depth_left)
{
    uint16_t magic, entries, max, depth;
    uint16_t i;

    if (node_len < 12)
        return VFS_ERR_INVAL;
    magic = ext4_rd16(node);
    entries = ext4_rd16(node + 2);
    max = ext4_rd16(node + 4);
    depth = ext4_rd16(node + 6);
    if (magic != EXT4_EH_MAGIC)
        return VFS_ERR_INVAL;
    if (entries > max || (uint32_t)max * 12 + 12 > node_len)
        return VFS_ERR_INVAL;

    if (depth == 0)
    {
        const uint8_t *e = node + 12;

        for (i = 0; i < entries; i++, e += 12)
        {
            uint32_t b = ext4_rd32(e);
            uint16_t len = ext4_rd16(e + 4);
            uint32_t span = len & EXT4_EE_LEN_MASK;
            uint64_t start = (uint64_t)ext4_rd32(e + 8) |
                             ((uint64_t)ext4_rd16(e + 6) << 32);

            if (span == 0)
                continue;
            if (lb >= b && lb < b + span)
            {
                *phys_out = start + (lb - b);
                *uninit_out = (len & EXT4_EE_UNINIT) != 0;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT; /* 空洞 */
    }

    if (depth_left <= 0 || depth > EXT4_MAX_DEPTH)
        return VFS_ERR_INVAL;
    {
        /* 索引层：取 ee_block <= lb 的最后一项 */
        const uint8_t *e = node + 12;
        const uint8_t *best = NULL;
        uint64_t child;
        int rc;

        for (i = 0; i < entries; i++, e += 12)
        {
            if (ext4_rd32(e) <= lb)
                best = e;
            else
                break;
        }
        if (!best)
            return VFS_ERR_NOENT;
        child = (uint64_t)ext4_rd32(best + 4) | ((uint64_t)ext4_rd16(best + 6) << 32);
        rc = ext4_read_fs_blocks(sb, child, 1, scratch);
        if (rc != 0)
            return rc;
        return ext4_map_walk(scratch, (uint32_t)sb->bs, lb, phys_out, uninit_out,
                             sb, scratch, depth_left - 1);
    }
}

/* 逻辑块 -> 物理块；空洞返回 NOENT（调用者按全 0 处理） */
static int ext4_map(ext4_sb_t *sb, const ext4_inode_t *ino, uint32_t lb,
                    uint64_t *phys_out, int *uninit_out, uint8_t *scratch)
{
    if ((ino->flags & EXT4_EXTENTS_FL) == 0)
        return VFS_ERR_NOTSUP; /* 非 extent 文件（间接块）暂不支持 */
    return ext4_map_walk(ino->blk, 60, lb, phys_out, uninit_out, sb, scratch,
                         EXT4_MAX_DEPTH);
}

/* 在目录数据中取下一条有效目录项；off 为输入/输出字节偏移 */
static int ext4_dir_next(ext4_sb_t *sb, const ext4_inode_t *dir, uint64_t *off,
                         uint32_t *ino_out, uint32_t *ftype_out,
                         char *name_out, uint32_t name_size)
{
    uint8_t *blk = NULL;
    int rc = VFS_OK;

    if (!dir->is_dir)
        return VFS_ERR_NOTDIR;
    blk = (uint8_t *)kmalloc(sb->bs);
    if (!blk)
        return VFS_ERR_NOMEM;

    while (*off < dir->size)
    {
        uint32_t lb = (uint32_t)(*off / sb->bs);
        uint32_t pos = (uint32_t)(*off % sb->bs);
        uint64_t phys = 0;
        int uninit = 0;

        rc = ext4_map(sb, dir, lb, &phys, &uninit, blk);
        if (rc == VFS_ERR_NOENT || rc == VFS_ERR_NOTSUP)
        {
            /* 空洞整块跳过 */
            *off = ((uint64_t)lb + 1) * sb->bs;
            continue;
        }
        if (rc != VFS_OK)
            break;
        if (!uninit)
        {
            rc = ext4_read_fs_blocks(sb, phys, 1, blk);
            if (rc != 0)
                break;
        }
        else
        {
            kernel_memset(blk, 0, sb->bs);
        }

        /* 块内沿 rec_len 链扫描 */
        while (pos + 8 <= sb->bs)
        {
            uint32_t ino = ext4_rd32(blk + pos);
            uint16_t rec = ext4_rd16(blk + pos + 4);
            uint8_t nlen = blk[pos + 6];
            uint8_t ftype = blk[pos + 7];
            uint32_t k;

            if (rec < 8 || pos + rec > sb->bs)
            {
                rc = VFS_ERR_INVAL; /* 目录损坏 */
                goto out;
            }
            *off = (uint64_t)lb * sb->bs + pos + rec;
            if (ino == 0)
            {
                pos += rec;
                continue;
            }
            if (nlen == 0 || (uint32_t)nlen + 8 > rec)
            {
                pos += rec;
                continue;
            }
            if (ino_out)
                *ino_out = ino;
            if (ftype_out)
                *ftype_out = ftype;
            if (name_out && name_size > 0)
            {
                uint32_t cn = nlen < name_size - 1 ? nlen : name_size - 1;

                for (k = 0; k < cn; k++)
                    name_out[k] = (char)blk[pos + 8 + k];
                name_out[cn] = '\0';
            }
            rc = VFS_OK;
            goto out;
        }
        /* 块内无有效项（理论上不应发生）：off 已指向块尾，继续下一块 */
    }
    rc = VFS_ERR_NOENT;

out:
    kfree(blk);
    return rc;
}

static int ext4_name_eq(const char *entry, const char *comp, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        if (entry[i] != comp[i])
            return 0;
    return entry[len] == '\0';
}

/* 路径查找：rel 为空即根；返回目标 ino。符号链接/非常规类型返回 NOTSUP */
static int ext4_lookup(ext4_sb_t *sb, const char *rel, uint32_t *ino_out)
{
    uint32_t ino = EXT4_ROOT_INO;
    const char *p = rel;
    ext4_inode_t dir;
    int rc;

    if (!rel || !ino_out)
        return VFS_ERR_INVAL;
    while (*p == '/')
        p++;
    if (*p == '\0')
    {
        *ino_out = ino;
        return VFS_OK;
    }

    for (;;)
    {
        const char *comp = p;
        uint32_t len = 0;
        uint64_t off = 0;
        uint32_t found = 0;

        while (*p && *p != '/')
        {
            p++;
            len++;
        }
        if (len == 0 || len >= VFS_NAME_MAX)
            return VFS_ERR_INVAL;

        rc = ext4_read_inode(sb, ino, &dir);
        if (rc != VFS_OK)
            return rc;
        if (!dir.is_dir)
            return VFS_ERR_NOTDIR;
        if ((dir.flags & EXT4_EXTENTS_FL) == 0)
            return VFS_ERR_NOTSUP;

        for (;;)
        {
            uint32_t e_ino, e_ft;
            char e_name[VFS_NAME_MAX];

            rc = ext4_dir_next(sb, &dir, &off, &e_ino, &e_ft, e_name, sizeof(e_name));
            if (rc == VFS_ERR_NOENT)
                return VFS_ERR_NOENT;
            if (rc != VFS_OK)
                return rc;
            if (ext4_name_eq(e_name, comp, len))
            {
                ext4_inode_t t;
                uint16_t mt;

                /* 命中：非常规/链接类型直接拒绝（暂无 symlink 解析） */
                rc = ext4_read_inode(sb, e_ino, &t);
                if (rc != VFS_OK)
                    return rc;
                mt = t.mode & EXT4_S_IFMT;
                if (mt != EXT4_S_IFREG && mt != EXT4_S_IFDIR)
                    return VFS_ERR_NOTSUP;
                found = e_ino;
                break;
            }
        }

        while (*p == '/')
            p++;
        ino = found;
        if (*p == '\0')
        {
            *ino_out = ino;
            return VFS_OK;
        }
    }
}

static int ext4_mount(vfs_mount_t *m, const char *dev)
{
    ext4_sb_t *sb = NULL;
    uint8_t *sec = NULL;
    vdev_t *vd;
    uint32_t log_bs, incompat;
    uint64_t gdt_bytes, gdt_blocks;
    int rc;

    if (!m || !dev || dev[0] == '\0')
        return VFS_ERR_INVAL;
    vd = vdev_find(dev);
    if (!vd || vd->type != VDEV_BLOCK || !vd->bops || !vd->bops->read_blocks)
        return VFS_ERR_NODEV;
    if (vd->block_size != EXT4_DEV_BLK)
        return VFS_ERR_NOTSUP;

    sb = (ext4_sb_t *)kmalloc(sizeof(ext4_sb_t));
    sec = (uint8_t *)kmalloc(1024);
    if (!sb || !sec)
    {
        rc = VFS_ERR_NOMEM;
        goto fail;
    }
    kernel_memset(sb, 0, (uint32_t)sizeof(*sb));
    sb->dev = vd;

    /* 超级块位于字节偏移 1024，即设备 LBA 2~3 */
    rc = vd->bops->read_blocks(vd, 2, 2, sec);
    if (rc != 0)
        goto fail;
    if (ext4_rd16(sec + 56) != EXT4_MAGIC)
    {
        rc = VFS_ERR_NODEV; /* 不是 ext4，交给自动挂载继续尝试下一分区 */
        goto fail;
    }

    sb->total_blocks = ext4_rd32(sec + 4);
    log_bs = ext4_rd32(sec + 24);
    if (log_bs > 3)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    sb->bs = 1024u << log_bs;
    sb->blk_per_fs = sb->bs / EXT4_DEV_BLK;
    sb->blocks_per_group = ext4_rd32(sec + 32);
    sb->inodes_per_group = ext4_rd32(sec + 40);
    sb->inode_size = ext4_rd16(sec + 88);
    if (sb->blocks_per_group == 0 || sb->inodes_per_group == 0)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    if (sb->inode_size < 128 || sb->inode_size > sb->bs)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }

    incompat = ext4_rd32(sec + 96);
    if ((incompat & ~EXT4_INCOMPAT_SUPP) != 0 || (incompat & EXT4_FT_EXTENTS) == 0)
    {
        fput_string("[EXT4] %s incompat=0x%x unsupported (need extents, no journal/meta_bg)\n",
                    dev, incompat);
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    sb->is_64bit = (incompat & EXT4_FT_64BIT) != 0;
    if (sb->is_64bit)
        sb->total_blocks |= (uint64_t)ext4_rd32(sec + 336) << 32; /* s_blocks_count_hi */
    if (sb->total_blocks == 0)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }

    sb->desc_size = sb->is_64bit ? ext4_rd16(sec + 254) : 32; /* s_desc_size */
    if (sb->is_64bit && sb->desc_size < 64)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    if (sb->desc_size < 32 || sb->desc_size > sb->bs)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    sb->ngroups = (uint32_t)((sb->total_blocks + sb->blocks_per_group - 1) / sb->blocks_per_group);
    if (sb->ngroups == 0)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }

    gdt_bytes = (uint64_t)sb->ngroups * sb->desc_size;
    gdt_blocks = (gdt_bytes + sb->bs - 1) / sb->bs;
    if (gdt_blocks == 0 || gdt_blocks > 0xffffffffu)
    {
        rc = VFS_ERR_NOTSUP;
        goto fail;
    }
    sb->gd = (uint8_t *)kmalloc((size_t)(gdt_blocks * sb->bs));
    if (!sb->gd)
    {
        rc = VFS_ERR_NOMEM;
        goto fail;
    }
    /* bs=1024 时 GDT 起于块 2，否则块 1 */
    rc = ext4_read_fs_blocks(sb, sb->bs == 1024 ? 2 : 1, (uint32_t)gdt_blocks, sb->gd);
    if (rc != 0)
        goto fail;

    /* 校验根 inode 可读且为目录 */
    {
        ext4_inode_t root;

        rc = ext4_read_inode(sb, EXT4_ROOT_INO, &root);
        if (rc != VFS_OK)
            goto fail;
        if (!root.is_dir)
        {
            rc = VFS_ERR_NOTSUP;
            goto fail;
        }
    }

    {
        char label[17];
        uint32_t k;

        for (k = 0; k < 16; k++)
            label[k] = (char)sec[120 + k];
        label[16] = '\0';
        fput_string("[EXT4] %s bs=%u groups=%u inodes/group=%u inode_size=%u 64bit=%u blocks=%llu label='%s'\n",
                    dev, sb->bs, sb->ngroups, sb->inodes_per_group, sb->inode_size,
                    sb->is_64bit, sb->total_blocks, label);
    }
    kfree(sec);
    m->priv = sb;
    return VFS_OK;

fail:
    if (sb)
    {
        if (sb->gd)
            kfree(sb->gd);
        kfree(sb);
    }
    if (sec)
        kfree(sec);
    return rc;
}

static int ext4_unmount(vfs_mount_t *m)
{
    ext4_sb_t *sb = (ext4_sb_t *)m->priv;

    if (!sb)
        return VFS_OK;
    if (sb->gd)
        kfree(sb->gd);
    kfree(sb);
    m->priv = NULL;
    return VFS_OK;
}

static int ext4_open(vfs_mount_t *m, const char *rel, int flags, vfs_file_t *f)
{
    ext4_sb_t *sb = (ext4_sb_t *)m->priv;
    uint32_t ino;
    ext4_inode_t *ip;
    int want_dir;
    int rc;

    if (!sb || !rel || !f)
        return VFS_ERR_INVAL;
    if ((flags & 0x03) != VFS_O_RDONLY)
        return VFS_ERR_NOTSUP; /* 只读挂载 */
    if (flags & (VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND))
        return VFS_ERR_NOTSUP;
    want_dir = (flags & VFS_O_DIRECTORY) != 0;

    rc = ext4_lookup(sb, rel, &ino);
    if (rc != VFS_OK)
        return rc;
    ip = (ext4_inode_t *)kmalloc(sizeof(ext4_inode_t));
    if (!ip)
        return VFS_ERR_NOMEM;
    rc = ext4_read_inode(sb, ino, ip);
    if (rc != VFS_OK)
    {
        kfree(ip);
        return rc;
    }
    if (ip->is_dir && !want_dir)
    {
        kfree(ip);
        return VFS_ERR_ISDIR;
    }
    if (!ip->is_dir && want_dir)
    {
        kfree(ip);
        return VFS_ERR_NOTDIR;
    }
    if (!ip->is_dir && !ip->is_reg)
    {
        kfree(ip);
        return VFS_ERR_NOTSUP;
    }
    f->priv = ip;
    f->is_dir = ip->is_dir;
    f->size = ip->is_dir ? 0 : ip->size;
    f->pos = 0;
    return VFS_OK;
}

static int ext4_close(vfs_file_t *f)
{
    if (f->priv)
    {
        kfree(f->priv);
        f->priv = NULL;
    }
    return VFS_OK;
}

static int64_t ext4_read(vfs_file_t *f, void *buf, uint64_t len)
{
    ext4_sb_t *sb = (ext4_sb_t *)f->mnt->priv;
    ext4_inode_t *ip = (ext4_inode_t *)f->priv;
    uint8_t *blk = NULL;
    uint8_t *p = (uint8_t *)buf;
    uint64_t done = 0;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!ip)
        return VFS_ERR_INVAL;
    if (len == 0)
        return 0;
    if (f->pos >= ip->size)
        return 0;
    if (len > ip->size - f->pos)
        len = ip->size - f->pos;

    blk = (uint8_t *)kmalloc(sb->bs);
    if (!blk)
        return VFS_ERR_NOMEM;

    while (len > 0)
    {
        uint32_t lb = (uint32_t)(f->pos / sb->bs);
        uint32_t inner = (uint32_t)(f->pos % sb->bs);
        uint32_t chunk = sb->bs - inner;
        uint64_t phys = 0;
        int uninit = 0;
        int rc;

        if ((uint64_t)chunk > len)
            chunk = (uint32_t)len;
        rc = ext4_map(sb, ip, lb, &phys, &uninit, blk);
        if (rc == VFS_ERR_NOENT || (rc == VFS_OK && uninit))
        {
            kernel_memset(p, 0, chunk); /* 空洞/未初始化 extent 读零 */
        }
        else if (rc == VFS_OK)
        {
            rc = ext4_read_fs_blocks(sb, phys, 1, blk);
            if (rc != 0)
                break;
            kernel_memcpy(p, blk + inner, chunk);
        }
        else
        {
            break; /* NOTSUP 等：返回已读部分，无已读则返回错误 */
        }
        p += chunk;
        f->pos += chunk;
        done += chunk;
        len -= chunk;
    }

    kfree(blk);
    if (done)
        return (int64_t)done;
    return VFS_ERR_NOTSUP;
}

static int64_t ext4_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP; /* 只读挂载 */
}

static int64_t ext4_seek(vfs_file_t *f, int64_t off, int whence)
{
    ext4_inode_t *ip = (ext4_inode_t *)f->priv;
    int64_t base;

    if (f->is_dir)
        return VFS_ERR_ISDIR;
    if (!ip)
        return VFS_ERR_INVAL;
    if (whence == VFS_SEEK_SET)
        base = off;
    else if (whence == VFS_SEEK_CUR)
        base = (int64_t)f->pos + off;
    else
        base = (int64_t)ip->size + off;
    if (base < 0)
        return VFS_ERR_INVAL;
    f->pos = (uint64_t)base;
    return (int64_t)f->pos;
}

static int ext4_stat(vfs_mount_t *m, const char *rel, uint64_t *size_out, uint32_t *is_dir_out)
{
    ext4_sb_t *sb = (ext4_sb_t *)m->priv;
    uint32_t ino;
    ext4_inode_t ip;
    int rc;

    if (!sb || !rel)
        return VFS_ERR_INVAL;
    rc = ext4_lookup(sb, rel, &ino);
    if (rc != VFS_OK)
        return rc;
    rc = ext4_read_inode(sb, ino, &ip);
    if (rc != VFS_OK)
        return rc;
    if (size_out)
        *size_out = ip.is_dir ? 0 : ip.size;
    if (is_dir_out)
        *is_dir_out = ip.is_dir;
    return VFS_OK;
}

static int ext4_mkdir(vfs_mount_t *m, const char *rel)
{
    (void)m;
    (void)rel;
    return VFS_ERR_NOTSUP; /* 只读挂载 */
}

static int ext4_rmdir(vfs_mount_t *m, const char *rel)
{
    (void)m;
    (void)rel;
    return VFS_ERR_NOTSUP; /* 只读挂载 */
}

static int ext4_opendir(vfs_mount_t *m, const char *rel, vfs_dir_t *d)
{
    ext4_sb_t *sb = (ext4_sb_t *)m->priv;
    uint32_t ino;
    ext4_inode_t *ip;
    int rc;

    if (!sb || !rel || !d)
        return VFS_ERR_INVAL;
    rc = ext4_lookup(sb, rel, &ino);
    if (rc != VFS_OK)
        return rc;
    ip = (ext4_inode_t *)kmalloc(sizeof(ext4_inode_t));
    if (!ip)
        return VFS_ERR_NOMEM;
    rc = ext4_read_inode(sb, ino, ip);
    if (rc != VFS_OK)
    {
        kfree(ip);
        return rc;
    }
    if (!ip->is_dir)
    {
        kfree(ip);
        return VFS_ERR_NOTDIR;
    }
    d->priv = ip;
    d->pos = (void *)0;
    return VFS_OK;
}

static int ext4_readdir(vfs_dir_t *d, vfs_dirent_t *out)
{
    ext4_sb_t *sb = (ext4_sb_t *)d->mnt->priv;
    ext4_inode_t *ip = (ext4_inode_t *)d->priv;
    uint64_t off = (uint64_t)(uintptr_t)d->pos;
    uint32_t e_ino, e_ft;
    char e_name[VFS_NAME_MAX];
    int rc;

    if (!sb || !ip || !out)
        return VFS_ERR_INVAL;
    rc = ext4_dir_next(sb, ip, &off, &e_ino, &e_ft, e_name, sizeof(e_name));
    if (rc != VFS_OK)
        return rc;
    d->pos = (void *)(uintptr_t)off;
    kernel_strncpy(out->name, e_name, sizeof(out->name));
    out->is_dir = (e_ft == EXT4_FT_DIR);
    out->size = 0;
    return VFS_OK;
}

static int ext4_closedir(vfs_dir_t *d)
{
    if (d->priv)
    {
        kfree(d->priv);
        d->priv = NULL;
    }
    d->pos = NULL;
    return VFS_OK;
}

const fs_driver_t ext4_driver = {
    .name = "ext4",
    .mount = ext4_mount,
    .unmount = ext4_unmount,
    .open = ext4_open,
    .close = ext4_close,
    .read = ext4_read,
    .write = ext4_write,
    .seek = ext4_seek,
    .stat = ext4_stat,
    .mkdir = ext4_mkdir,
    .rmdir = ext4_rmdir,
    .opendir = ext4_opendir,
    .readdir = ext4_readdir,
    .closedir = ext4_closedir,
};
