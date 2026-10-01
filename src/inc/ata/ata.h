#ifndef CSOS_ATA_H
#define CSOS_ATA_H

#include <stdint.h>
#include <stddef.h>

#define ATA_SECTOR_SIZE 512
#define ATA_MAX_DRIVES 4
#define ATA_NO_BOOT_DRIVE 0xFF

typedef struct ata_ops
{
    const char *name;

    int (*init)();
    int (*read_sectors)(uint8_t drive, uint64_t lba, uint32_t sectors, void *buf);
    int (*write_sectors)(uint8_t drive, uint64_t lba, uint32_t sectors, const void *buf);
    int (*flush)(uint8_t drive);
    uint64_t (*drive_sectors)(uint8_t drive);
    uint8_t (*boot_drive)();
} ata_ops_t;

/* 获取当前策略；未设置时默认返回 ATA PIO 实现 */
const ata_ops_t *ata_current(void);

/* 切换策略（传入 NULL 则恢复默认的 PIO） */
void ata_set(const ata_ops_t *ops);

#endif /* CSOS_ATA_H */