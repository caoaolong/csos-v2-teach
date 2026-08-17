#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>
#include <assert.h>
#include <gdt.h>
#include <idt.h>
#include <memory/pmm.h>
#include <gfx/fb.h>
#include <gfx/logo.h>

void kernel_main(boot_info_t *boot_info)
{
    serial_init();
    init_gdt();
    init_idt();

    init_pmm(boot_info);

    fb_draw_logo_splash(boot_info, LOGO_pixels, LOGO_WIDTH, LOGO_HEIGHT);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
