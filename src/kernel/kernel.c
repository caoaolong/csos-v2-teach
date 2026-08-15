#include <stddef.h>
#include <stdint.h>
#include <kernel.h>
#include <serial.h>
#include <assert.h>
#include <gdt.h>
#include <idt.h>
#include <memory/pmm.h>

static void test_pmm()
{
    uint64_t a;
    uint64_t b;
    uint64_t before;
    uint64_t mid;
    uint64_t after;

    before = pmm_free_pages();
    fput_string("[PMM] free pages before alloc: %llu\n", before);

    a = alloc_page();
    b = alloc_page();
    mid = pmm_free_pages();
    fput_string("[PMM] alloc a=0x%llx b=0x%llx free=%llu\n", a, b, mid);

    if (a == PMM_INVALID_ADDR || b == PMM_INVALID_ADDR || mid != before - 2)
    {
        put_string("[PMM] alloc_page test FAILED\n");
        return;
    }

    free_page(a);
    free_page(b);
    after = pmm_free_pages();
    fput_string("[PMM] free pages after free: %llu\n", after);

    if (after != before)
        put_string("[PMM] free_page test FAILED\n");
    else
        put_string("[PMM] alloc/free test OK\n");
}

void kernel_main(boot_info_t *boot_info)
{
    serial_init();
    init_gdt();
    init_idt();

    init_pmm(boot_info);
    test_pmm();

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
