#include <memory/pmm.h>
#include <memory/vmm.h>
#include <kernel_layout.h>
#include <stdint.h>
#include <stddef.h>

#define BOOT_IDENTITY_BYTES (512ULL * 1024 * 1024)
#define BOOT_PT_POOL_PAGES 16u

/* 引导阶段须在物理地址执行，代码/数据均放在 bootstrap 段 */
#define BOOT_SECTION __attribute__((section(".text.boot_paging")))
#define BOOT_BSS __attribute__((section(".bss.boot_paging")))

extern void kernel_main(boot_info_t *boot);
extern char __bss_start[];
extern char __bss_end[];

static uint64_t boot_pml4[512]
    __attribute__((section(".bss.boot_paging"), aligned(4096)));
static uint64_t boot_pdpt_low[512]
    __attribute__((section(".bss.boot_paging"), aligned(4096)));
static uint64_t boot_pd_low[512]
    __attribute__((section(".bss.boot_paging"), aligned(4096)));
static uint64_t boot_pt_pool[BOOT_PT_POOL_PAGES][512]
    __attribute__((section(".bss.boot_paging"), aligned(4096)));

static unsigned boot_pt_used BOOT_BSS;

static BOOT_SECTION void boot_memset(void *dst, int value, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n-- > 0)
        *p++ = (uint8_t)value;
}

static BOOT_SECTION uint64_t *boot_alloc_pt(void)
{
    if (boot_pt_used >= BOOT_PT_POOL_PAGES)
        return NULL;
    return boot_pt_pool[boot_pt_used++];
}

static BOOT_SECTION void boot_load_cr3(uint64_t pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static BOOT_SECTION int boot_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t i4 = (vaddr >> 39) & 0x1FF;
    uint64_t i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF;
    uint64_t i1 = (vaddr >> 12) & 0x1FF;

    flags |= PTE_PRESENT;
    flags &= ~PTE_HUGE;

    if (boot_pml4[i4] & PTE_PRESENT)
        pdpt = (uint64_t *)(uintptr_t)(boot_pml4[i4] & PTE_ADDR_MASK);
    else
    {
        pdpt = boot_alloc_pt();
        if (pdpt == NULL)
            return -1;
        boot_memset(pdpt, 0, PAGE_SIZE);
        boot_pml4[i4] = ((uint64_t)(uintptr_t)pdpt) | PTE_PRESENT | PTE_WRITABLE;
    }

    if (pdpt[i3] & PTE_PRESENT)
    {
        if (pdpt[i3] & PTE_HUGE)
            return -2;
        pd = (uint64_t *)(uintptr_t)(pdpt[i3] & PTE_ADDR_MASK);
    }
    else
    {
        pd = boot_alloc_pt();
        if (pd == NULL)
            return -3;
        boot_memset(pd, 0, PAGE_SIZE);
        pdpt[i3] = ((uint64_t)(uintptr_t)pd) | PTE_PRESENT | PTE_WRITABLE;
    }

    if (pd[i2] & PTE_PRESENT)
    {
        if (pd[i2] & PTE_HUGE)
            return -4;
        pt = (uint64_t *)(uintptr_t)(pd[i2] & PTE_ADDR_MASK);
    }
    else
    {
        pt = boot_alloc_pt();
        if (pt == NULL)
            return -5;
        boot_memset(pt, 0, PAGE_SIZE);
        pd[i2] = ((uint64_t)(uintptr_t)pt) | PTE_PRESENT | PTE_WRITABLE;
    }

    pt[i1] = (paddr & PTE_ADDR_MASK) | flags;
    return 0;
}

static BOOT_SECTION int boot_map_identity_huge(void)
{
    uint64_t pa;
    uint64_t pml4_phys = (uint64_t)(uintptr_t)boot_pml4;
    uint64_t pdpt_phys = (uint64_t)(uintptr_t)boot_pdpt_low;
    uint64_t pd_phys = (uint64_t)(uintptr_t)boot_pd_low;

    boot_memset(boot_pml4, 0, PAGE_SIZE);
    boot_memset(boot_pdpt_low, 0, PAGE_SIZE);
    boot_memset(boot_pd_low, 0, PAGE_SIZE);
    boot_pt_used = 0;

    boot_pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
    boot_pdpt_low[0] = pd_phys | PTE_PRESENT | PTE_WRITABLE;

    for (pa = 0; pa < BOOT_IDENTITY_BYTES; pa += (2ULL * 1024 * 1024))
    {
        uint64_t idx = (pa >> 21) & 0x1FF;
        boot_pd_low[idx] = (pa & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
    }

    (void)pml4_phys;
    return 0;
}

static BOOT_SECTION int boot_map_kernel_image(void)
{
    /*
     * 禁止调用 kernel_layout.h 中的 inline 辅助函数：-O0 下可能生成
     * 高半核 .text 符号，在 load CR3 之前调用会访问未映射地址。
     */
    uint64_t phys = (uint64_t)(uintptr_t)__kernel_phys_start;
    uint64_t virt = (uint64_t)(uintptr_t)__kernel_virt_start;
    uint64_t end = (uint64_t)(uintptr_t)__kernel_phys_end;

    while (phys < end)
    {
        if (boot_map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE) != 0)
            return -1;
        phys += PAGE_SIZE;
        virt += PAGE_SIZE;
    }
    return 0;
}

BOOT_SECTION void boot_enter_higher_half(boot_info_t *boot)
{
    uint64_t bss_start;
    uint64_t bss_end;
    uint64_t size;

    if (boot_map_identity_huge() != 0)
        for (;;)
            __asm__ volatile("hlt");

    if (boot_map_kernel_image() != 0)
        for (;;)
            __asm__ volatile("hlt");

    boot_load_cr3((uint64_t)(uintptr_t)boot_pml4);

    bss_start = (uint64_t)(uintptr_t)__bss_start;
    bss_end = (uint64_t)(uintptr_t)__bss_end;
    if (bss_end > bss_start)
    {
        size = (size_t)(bss_end - bss_start);
        boot_memset((void *)(uintptr_t)bss_start, 0, size);
    }

    kernel_main(boot);

    for (;;)
        __asm__ volatile("hlt");
}
