#include <kernel_layout.h>
#include <memory/vmm.h>
#include <serial.h>
#include <string.h>

#define ENTRIES_PER_TABLE 512

static uint64_t *kernel_pml4;

static inline uint64_t pte_addr(uint64_t entry)
{
    return entry & PTE_ADDR_MASK;
}

static void flush_tlb_all(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static void invlpg(uint64_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static void load_cr3(uint64_t pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static int map_kernel_image(void)
{
    uint64_t phys = kernel_phys_start();
    uint64_t virt = kernel_virt_start();
    uint64_t end = kernel_phys_end();

    while (phys < end)
    {
        if (map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE) != 0)
            return -1;
        phys += PAGE_SIZE;
        virt += PAGE_SIZE;
    }
    return 0;
}

uint64_t read_cr2()
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

uint64_t read_cr3()
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static uint64_t *table_from_entry(uint64_t entry)
{
    if (!(entry & PTE_PRESENT))
        return NULL;
    /* identity map 下物理地址可直接当指针 */
    return (uint64_t *)(uintptr_t)pte_addr(entry);
}

static uint64_t *get_or_alloc_table(uint64_t *parent, uint64_t index)
{
    void *page;

    if (parent[index] & PTE_PRESENT)
    {
        /* 大页不可再下钻 */
        if (parent[index] & PTE_HUGE)
            return NULL;
        return table_from_entry(parent[index]);
    }

    page = (void *)alloc_page();
    if (page == NULL)
        return NULL;

    kernel_memset(page, 0, (uint32_t)PAGE_SIZE);
    parent[index] = ((uint64_t)(uintptr_t)page) | PTE_PRESENT | PTE_WRITABLE;
    return (uint64_t *)page;
}

static int map_framebuffer(boot_info_t *boot)
{
    uint64_t base;
    uint64_t size;
    uint64_t pa;
    uint64_t end;

    if (boot == NULL || boot->framebuffer_base == 0 ||
        boot->framebuffer_width == 0 || boot->framebuffer_height == 0)
    {
        return 0;
    }

    base = boot->framebuffer_base & ~(PAGE_SIZE - 1);
    size = (uint64_t)boot->framebuffer_pixels_per_scanline *
           (uint64_t)boot->framebuffer_height * 4ULL;
    end = (boot->framebuffer_base + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (pa = base; pa < end; pa += PAGE_SIZE)
    {
        if (map_page(pa, pa, PTE_PRESENT | PTE_WRITABLE | PTE_WRITE_THROUGH) != 0)
        {
            fput_string("[VMM] map framebuffer failed at 0x%llx\n", pa);
            return -1;
        }
    }

    fput_string("[VMM] framebuffer 0x%llx size=0x%llx (%ux%u)\n",
                boot->framebuffer_base, size,
                boot->framebuffer_width, boot->framebuffer_height);
    return 0;
}

int map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t i4 = (vaddr >> 39) & 0x1FF;
    uint64_t i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF;
    uint64_t i1 = (vaddr >> 12) & 0x1FF;

    if (kernel_pml4 == NULL)
        return -1;
    if ((vaddr | paddr) & (PAGE_SIZE - 1))
        return -1;

    flags |= PTE_PRESENT;
    flags &= ~PTE_HUGE;

    pdpt = get_or_alloc_table(kernel_pml4, i4);
    if (pdpt == NULL)
        return -1;

    pd = get_or_alloc_table(pdpt, i3);
    if (pd == NULL)
        return -1;

    pt = get_or_alloc_table(pd, i2);
    if (pt == NULL)
        return -1;

    pt[i1] = (paddr & PTE_ADDR_MASK) | flags;
    invlpg(vaddr);
    return 0;
}

void init_vmm(boot_info_t *boot)
{
    uint64_t phys_end;
    uint64_t pa;
    void *pml4_page;

    phys_end = pmm_total_pages() * PAGE_SIZE;
    if (phys_end == 0)
    {
        put_string("[VMM] init failed: PMM not ready\n");
        return;
    }

    // PML4：长模式下4级分页的最顶层目录
    pml4_page = (void *)alloc_page();
    if (pml4_page == NULL)
    {
        put_string("[VMM] init failed: no page for PML4\n");
        return;
    }

    kernel_memset(pml4_page, 0, (uint32_t)PAGE_SIZE);
    kernel_pml4 = (uint64_t *)pml4_page;

    /* 在仍使用 UEFI 页表时建表；完成后切换 CR3 */
    for (pa = 0; pa < phys_end; pa += PAGE_SIZE)
    {
        if (map_page(pa, pa, PTE_PRESENT | PTE_WRITABLE) != 0)
        {
            fput_string("[VMM] identity map failed at 0x%llx\n", pa);
            return;
        }
    }

    if (map_framebuffer(boot) != 0)
        return;

    if (map_kernel_image() != 0)
    {
        put_string("[VMM] kernel higher-half map failed\n");
        return;
    }

    load_cr3((uint64_t)(uintptr_t)kernel_pml4);
    flush_tlb_all();

    fput_string("[VMM] CR3=0x%llx identity 0x0-0x%llx kernel virt=0x%llx phys=0x%llx-0x%llx\n",
                read_cr3(), phys_end,
                kernel_virt_start(), kernel_phys_start(), kernel_phys_end());
}
