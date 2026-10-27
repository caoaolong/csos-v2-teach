#ifndef CSOS_CONFIG_H
#define CSOS_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <memory/pmm.h>

#define BOOT_CONFIG_MAX_MOUNTS 6
#define BOOT_CONFIG_DEV_MAX 32
#define BOOT_CONFIG_MP_MAX 64
#define BOOT_CONFIG_FSTYPE_MAX 16
#define BOOT_CONFIG_INIT_MAX 256

typedef struct config_mount
{
    char dev[BOOT_CONFIG_DEV_MAX];
    char mountpoint[BOOT_CONFIG_MP_MAX];
    char fstype[BOOT_CONFIG_FSTYPE_MAX];
} config_mount_t;

typedef struct boot_config
{
    int present; /* csos.cfg 是否有效（无效则全用内核默认值） */

    char pmm[16];                   /* 默认 "bitmap" */
    char disk[16];                  /* 默认 "ata-pio" */
    char root[BOOT_CONFIG_DEV_MAX]; /* 根设备，如 "hd0p0"，可为空 */

    config_mount_t mounts[BOOT_CONFIG_MAX_MOUNTS];
    int mount_count;

    char init[BOOT_CONFIG_INIT_MAX]; /* 首个用户程序路径 */
    char debug[16];                  /* 默认 "serial" */
    int timeout;                     /* 默认 3（秒，预留） */
} boot_config_t;

extern boot_config_t g_boot_config;

/*
 * 解析 BootInfo 中的 csos.cfg 原文并立即应用，只用静态存储。
 * 解析阶段全程静默；应用阶段含条件串口初始化（debug=serial 时）。
 * 必须在 init_pmm 之前调用。
 */
void parse_apply_boot_config(boot_info_t *boot);

/* 在 serial_init 之后调用：打印生效的配置摘要 */
void boot_config_summary();

#endif /* CSOS_CONFIG_H */
