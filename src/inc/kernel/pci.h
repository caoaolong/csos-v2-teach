#ifndef CSOS_PCI_H
#define CSOS_PCI_H

#include <stdint.h>

/* PCI 配置空间访问 + 总线枚举（mechanism 1：0xCF8/0xCFC）。
 * 本步只做读枚举（设备清单 + BAR 尺寸探测），不改任何设备状态，
 * 为后续 virtio-gpu / Bochs modesetting 驱动铺路。 */

#define PCI_MAX_DEVICES 32

typedef struct pci_bar
{
    uint64_t addr;  /* I/O 端口基址或 MMIO 物理基址 */
    uint64_t size;  /* 窗口大小（尺寸探测得出） */
    int is_io;      /* 1=I/O 端口，0=MMIO */
    int is64;       /* 1=64 位 MMIO（占两条 BAR 槽） */
    int present;    /* 1=该槽有效 */
} pci_bar_t;

typedef struct pci_dev
{
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header; /* 低 7 位：0=普通设备，1=PCI 桥 */
    uint8_t irq;    /* 中断线（传统 INTx） */
    pci_bar_t bars[6];
} pci_dev_t;

/* 配置空间读写（offset 须按位宽对齐要求传，函数内取整） */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                     uint32_t v);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);

/* 枚举结果查询 */
int pci_count(void);
const pci_dev_t *pci_get(int idx);

/* 扫描全部总线并打印清单 */
void init_pci(void);

#endif /* CSOS_PCI_H */
