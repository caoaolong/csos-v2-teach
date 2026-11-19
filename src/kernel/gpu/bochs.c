#include <kernel/gpu/bochs.h>
#include <kernel/kernel.h>
#include <kernel/serial.h>
#include <kernel/pci.h>
#include <kernel/config.h>
#include <kernel/memory/pmm.h>

/* Bochs VBE dispi 端口与寄存器（QEMU -vga std 实现该接口） */
#define BOCHS_INDEX 0x1CE
#define BOCHS_DATA 0x1CF

#define BOCHS_ID 0
#define BOCHS_XRES 1
#define BOCHS_YRES 2
#define BOCHS_BPP 3
#define BOCHS_ENABLE 4
#define BOCHS_VIRT_W 6
#define BOCHS_VIRT_H 7
#define BOCHS_X_OFF 8
#define BOCHS_Y_OFF 9

#define BOCHS_EN_ENABLED 0x01
#define BOCHS_EN_LFB 0x40

/* 候选分辨率（由高到低）：按显存 BAR0 实际大小取第一个装得下的 */
static const uint16_t g_modes[][2] = {
    {1920, 1080},
    {1600, 900},
    {1280, 1024},
    {1024, 768},
};

static uint16_t dispi_read(uint16_t idx)
{
    outw(BOCHS_INDEX, idx);
    return inw(BOCHS_DATA);
}

static void dispi_write(uint16_t idx, uint16_t v)
{
    outw(BOCHS_INDEX, idx);
    outw(BOCHS_DATA, v);
}

void bochs_modeset(struct boot_info *boot)
{
    int i;
    const pci_dev_t *vga = NULL;
    uint16_t id;
    uint64_t lfb;
    uint64_t vram;
    uint16_t w = 0;
    uint16_t h = 0;
    uint16_t cand[5][2];
    int ncand = 0;

    if (boot == NULL)
        return;
    for (i = 0; i < pci_count(); i++)
    {
        const pci_dev_t *d = pci_get(i);

        if (d->vendor == 0x1234 && d->device == 0x1111)
        {
            vga = d;
            break;
        }
    }
    if (vga == NULL)
    {
        fput_string("[BOCHS] no std vga, keep GOP\n");
        return;
    }
    if (!vga->bars[0].present || vga->bars[0].is_io)
    {
        fput_string("[BOCHS] no LFB bar, keep GOP\n");
        return;
    }
    lfb = vga->bars[0].addr;
    vram = vga->bars[0].size;
    id = dispi_read(BOCHS_ID);
    fput_string("[BOCHS] dispi id=0x%x lfb=0x%llx vram=0x%llx\n", id,
                (unsigned long long)lfb, (unsigned long long)vram);
    if (id < 0xB0C4 || id == 0xFFFF)
    {
        fput_string("[BOCHS] dispi too old, keep GOP\n");
        return;
    }
    /* csos.cfg resolution= 优先（精确值，不降档；失败回 GOP） */
    if (g_boot_config.disp_w != 0 && g_boot_config.disp_h != 0)
    {
        cand[0][0] = (uint16_t)g_boot_config.disp_w;
        cand[0][1] = (uint16_t)g_boot_config.disp_h;
        ncand = 1;
    }
    else
    {
        /* 自动：按显存取最高可装档 */
        for (i = 0; i < (int)(sizeof(g_modes) / sizeof(g_modes[0])); i++)
        {
            cand[i][0] = g_modes[i][0];
            cand[i][1] = g_modes[i][1];
        }
        ncand = (int)(sizeof(g_modes) / sizeof(g_modes[0]));
    }
    for (i = 0; i < ncand; i++)
    {
        uint64_t need = (uint64_t)cand[i][0] * cand[i][1] * 4ULL;

        if (need <= vram)
        {
            w = cand[i][0];
            h = cand[i][1];
            break;
        }
    }
    if (w == 0)
    {
        fput_string("[BOCHS] vram too small, keep GOP\n");
        return;
    }
    dispi_write(BOCHS_ENABLE, 0); /* 先关显示再改模式 */
    dispi_write(BOCHS_XRES, w);
    dispi_write(BOCHS_YRES, h);
    dispi_write(BOCHS_BPP, 32);
    dispi_write(BOCHS_VIRT_W, w);
    dispi_write(BOCHS_VIRT_H, h);
    dispi_write(BOCHS_X_OFF, 0);
    dispi_write(BOCHS_Y_OFF, 0);
    dispi_write(BOCHS_ENABLE, BOCHS_EN_ENABLED | BOCHS_EN_LFB);
    if (dispi_read(BOCHS_XRES) != w || dispi_read(BOCHS_YRES) != h ||
        !(dispi_read(BOCHS_ENABLE) & BOCHS_EN_ENABLED))
    {
        fput_string("[BOCHS] modeset verify failed, keep GOP\n");
        return;
    }
    boot->framebuffer_base = lfb;
    boot->framebuffer_width = w;
    boot->framebuffer_height = h;
    boot->framebuffer_pixels_per_scanline = w;
    boot->framebuffer_pixel_format = 1; /* Bochs 32bpp 为 BGRX */
    boot->framebuffer_phys_w_mm = 0;    /* 无 EDID，按未知处理 */
    boot->framebuffer_phys_h_mm = 0;
    fput_string("[BOCHS] %ux%ux32 lfb=0x%llx\n", w, h,
                (unsigned long long)lfb);
}
