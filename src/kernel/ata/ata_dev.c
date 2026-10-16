#include <ata/ata.h>
#include <vdev.h>
#include <serial.h>
#include <string.h>

static uint8_t g_hd_drive[ATA_MAX_DRIVES];
static vdev_t g_hd[ATA_MAX_DRIVES];

static uint8_t hd_id(vdev_t *dev)
{
    uint8_t *p = (uint8_t *)dev->priv;
    return p ? *p : 0;
}

static int hd_open(vdev_t *dev, vfs_file_t *f)
{
    (void)dev;
    (void)f;
    return VFS_OK;
}

static int hd_close(vdev_t *dev, vfs_file_t *f)
{
    (void)dev;
    (void)f;
    return VFS_OK;
}

static int hd_read_blocks(vdev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (count == 0 || buf == NULL)
        return VFS_ERR_INVAL;
    return ata_current()->read_sectors(hd_id(dev), lba, count, buf);
}

static int hd_write_blocks(vdev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (count == 0 || buf == NULL)
        return VFS_ERR_INVAL;
    return ata_current()->write_sectors(hd_id(dev), lba, count, buf);
}

static int hd_flush(vdev_t *dev)
{
    return ata_current()->flush(hd_id(dev));
}

static uint64_t hd_capacity(vdev_t *dev)
{
    return ata_current()->drive_sectors(hd_id(dev));
}

static const vdev_block_ops_t g_hd_ops = {
    .open = hd_open,
    .close = hd_close,
    .read_blocks = hd_read_blocks,
    .write_blocks = hd_write_blocks,
    .flush = hd_flush,
    .capacity = hd_capacity,
};

void init_ata_dev()
{
    int i;

    for (i = 0; i < ATA_MAX_DRIVES; i++)
    {
        uint64_t sectors = ata_current()->drive_sectors((uint8_t)i);
        vdev_t *d;
        int rc;

        if (sectors == 0)
            continue;

        g_hd_drive[i] = (uint8_t)i;
        d = &g_hd[i];
        kernel_memset(d, 0, (uint32_t)sizeof(*d));
        d->name[0] = 'h';
        d->name[1] = 'd';
        d->name[2] = (char)('0' + i);
        d->name[3] = '\0';
        d->type = VDEV_BLOCK;
        d->major = VDEV_MAJOR_HD;
        d->minor = (uint32_t)i;
        d->bops = &g_hd_ops;
        d->block_size = ATA_SECTOR_SIZE;
        d->nblocks = sectors;
        d->priv = &g_hd_drive[i];

        rc = vdev_register(d);
        if (rc != VFS_OK)
            fput_string("[ATA-DEV] register hd%d failed rc=%d\n", i, rc);
        else
            fput_string("[ATA-DEV] register hd%d sectors=%llu\n", i, sectors);
    }
}
