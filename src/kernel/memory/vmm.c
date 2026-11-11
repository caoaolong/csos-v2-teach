#include <kernel/kernel_layout.h>
#include <kernel/memory/vmm.h>
#include <kernel/serial.h>
#include <kernel/string.h>

#define ENTRIES_PER_TABLE 512

static uint64_t *kernel_pml4;

/* map/unmap 的目标根；调度器随任务切换，启动期为 NULL（回退 master） */
static uint64_t *g_active_pml4 = NULL;

uint64_t *vmm_master_pml4(void);
uint64_t *vmm_active_pml4(void);
void vmm_switch_active(uint64_t *root);
int map_page_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

static inline uint64_t pte_addr(uint64_t entry)
{
    return entry & PTE_ADDR_MASK;
}

static void flush_tlb_all()
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

static int map_kernel_image()
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

int map_page_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t i4 = (vaddr >> 39) & 0x1FF;
    uint64_t i3 = (vaddr >> 30) & 0x1FF;
    uint64_t i2 = (vaddr >> 21) & 0x1FF;
    uint64_t i1 = (vaddr >> 12) & 0x1FF;

    if (pml4 == NULL)
        return -1;
    if ((vaddr | paddr) & (PAGE_SIZE - 1))
        return -1;

    flags |= PTE_PRESENT;
    flags &= ~PTE_HUGE;

    pdpt = get_or_alloc_table(pml4, i4);
    if (pdpt == NULL)
        return -1;

    pd = get_or_alloc_table(pdpt, i3);
    if (pd == NULL)
        return -1;

    pt = get_or_alloc_table(pd, i2);
    if (pt == NULL)
        return -1;

    if (flags & PTE_USER)
    {
        pml4[i4] |= PTE_USER;
        pdpt[i3] |= PTE_USER;
        pd[i2] |= PTE_USER;
    }

    pt[i1] = (paddr & PTE_ADDR_MASK) | flags;
    invlpg(vaddr);
    return 0;
}

int map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return map_page_in(vmm_active_pml4(), vaddr, paddr, flags);
}

uint64_t *vmm_master_pml4(void)
{
    return kernel_pml4;
}

uint64_t *vmm_active_pml4(void)
{
    return g_active_pml4 != NULL ? g_active_pml4 : kernel_pml4;
}

void vmm_switch_active(uint64_t *root)
{
    if (root == NULL)
        root = kernel_pml4;
    if (root == g_active_pml4)
        return;
    /* 内核半与 identity 映射处处相同，切 CR3 安全 */
    load_cr3((uint64_t)(uintptr_t)root);
    g_active_pml4 = root;
}

/* 拆除 [start, end) 内 4K 用户页映射并归还物理页，exec 替换映像前调用。
 * 只动 leaf 且 PTE_USER 置位的表项；页表页本身暂不回收（exec 低频）。 */
void unmap_user_range(uint64_t start, uint64_t end)
{
    uint64_t *root = vmm_active_pml4();
    uint64_t va;

    if (root == NULL)
        return;
    start &= ~(PAGE_SIZE - 1);
    for (va = start; va < end; va += PAGE_SIZE)
    {
        uint64_t pml4e = root[(va >> 39) & 0x1FF];
        uint64_t *pdpt;
        uint64_t pdpte;
        uint64_t *pd;
        uint64_t pde;
        uint64_t *pt;
        uint64_t e;

        if (va + PAGE_SIZE < va)
            break; /* 回绕 */
        if (!(pml4e & PTE_PRESENT) || (pml4e & PTE_HUGE))
            continue;
        pdpt = table_from_entry(pml4e);
        if (pdpt == NULL)
            continue;
        pdpte = pdpt[(va >> 30) & 0x1FF];
        if (!(pdpte & PTE_PRESENT) || (pdpte & PTE_HUGE))
            continue;
        pd = table_from_entry(pdpte);
        if (pd == NULL)
            continue;
        pde = pd[(va >> 21) & 0x1FF];
        if (!(pde & PTE_PRESENT) || (pde & PTE_HUGE))
            continue;
        pt = table_from_entry(pde);
        if (pt == NULL)
            continue;
        e = pt[(va >> 12) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER) || (e & PTE_HUGE))
            continue;
        free_page(e & PTE_ADDR_MASK);
        pt[(va >> 12) & 0x1FF] = 0;
        invlpg(va);
    }
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
    g_active_pml4 = kernel_pml4;

    fput_string("[VMM] CR3=0x%llx identity 0x0-0x%llx kernel virt=0x%llx phys=0x%llx-0x%llx\n",
                read_cr3(), phys_end,
                kernel_virt_start(), kernel_phys_start(), kernel_phys_end());
}

/* 用户区必须落在单个 PDPT 槽内（当前 USER_IMAGE 窗口满足），否则拒绝 */
static int user_slot_ok(uint64_t start, uint64_t end, uint64_t *slot_out)
{
    uint64_t s = (start >> 30) & 0x1FF;
    uint64_t e = ((end - 1) >> 30) & 0x1FF;

    if (end <= start || s != e)
        return -1;
    *slot_out = s;
    return 0;
}

uint64_t *vmm_new_user_pml4(uint64_t start, uint64_t end)
{
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *master_pdpt;
    uint64_t slot;
    int i;

    if (kernel_pml4 == NULL || user_slot_ok(start, end, &slot) != 0)
        return NULL;
    pml4 = (uint64_t *)alloc_page();
    pdpt = (uint64_t *)alloc_page();
    pd = (uint64_t *)alloc_page();
    if (pml4 == NULL || pdpt == NULL || pd == NULL)
    {
        if (pml4 != NULL)
            free_page((uint64_t)(uintptr_t)pml4);
        if (pdpt != NULL)
            free_page((uint64_t)(uintptr_t)pdpt);
        if (pd != NULL)
            free_page((uint64_t)(uintptr_t)pd);
        return NULL;
    }
    kernel_memset(pml4, 0, (uint32_t)PAGE_SIZE);
    kernel_memset(pd, 0, (uint32_t)PAGE_SIZE);
    for (i = 1; i < 512; i++)
        pml4[i] = kernel_pml4[i];
    if (!(kernel_pml4[0] & PTE_PRESENT))
    {
        free_page((uint64_t)(uintptr_t)pml4);
        free_page((uint64_t)(uintptr_t)pdpt);
        free_page((uint64_t)(uintptr_t)pd);
        return NULL;
    }
    master_pdpt = table_from_entry(kernel_pml4[0]);
    if (master_pdpt == NULL)
    {
        free_page((uint64_t)(uintptr_t)pml4);
        free_page((uint64_t)(uintptr_t)pdpt);
        free_page((uint64_t)(uintptr_t)pd);
        return NULL;
    }
    kernel_memcpy(pdpt, master_pdpt, (uint32_t)PAGE_SIZE);
    if (pdpt[slot] & PTE_PRESENT)
        fput_string("[VMM] new_user_pml4: master slot in use, shadowed\n");
    pdpt[slot] = ((uint64_t)(uintptr_t)pd) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    pml4[0] = (kernel_pml4[0] & ~PTE_ADDR_MASK) |
              ((uint64_t)(uintptr_t)pdpt & PTE_ADDR_MASK) | PTE_USER;
    return pml4;
}

int vmm_clone_user_range(uint64_t *dst_pml4, uint64_t *src_pml4,
                         uint64_t start, uint64_t end)
{
    uint64_t slot;
    uint64_t *src_pd;
    uint64_t *dst_pd;
    uint64_t *src_pdpt;
    uint64_t *dst_pdpt;
    int pdi;

    if (dst_pml4 == NULL || src_pml4 == NULL ||
        user_slot_ok(start, end, &slot) != 0)
        return -1;
    if (!(dst_pml4[0] & PTE_PRESENT) || !(src_pml4[0] & PTE_PRESENT))
        return -1;
    src_pdpt = table_from_entry(src_pml4[0]);
    dst_pdpt = table_from_entry(dst_pml4[0]);
    if (src_pdpt == NULL || dst_pdpt == NULL)
        return -1;
    if (!(src_pdpt[slot] & PTE_PRESENT))
        return 0; /* 源为空，无需拷贝 */
    if (!(dst_pdpt[slot] & PTE_PRESENT) || (dst_pdpt[slot] & PTE_HUGE))
        return -1;
    src_pd = table_from_entry(src_pdpt[slot]);
    dst_pd = table_from_entry(dst_pdpt[slot]);
    if (src_pd == NULL || dst_pd == NULL)
        return -1;

    for (pdi = 0; pdi < 512; pdi++)
    {
        uint64_t se = src_pd[pdi];
        uint64_t *spt;
        uint64_t *dpt;
        int li;

        if (!(se & PTE_PRESENT))
            continue;
        if (se & PTE_HUGE)
            return -1;
        spt = table_from_entry(se);
        if (spt == NULL)
            return -1;
        dpt = (uint64_t *)alloc_page();
        if (dpt == NULL)
            return -1;
        kernel_memset(dpt, 0, (uint32_t)PAGE_SIZE);
        /* 先挂链再拷页：中途失败由 vmm_free_user_pml4 整体回滚 */
        dst_pd[pdi] = ((uint64_t)(uintptr_t)dpt) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        for (li = 0; li < 512; li++)
        {
            uint64_t le = spt[li];

            if (!(le & PTE_PRESENT) || (le & PTE_HUGE))
                continue;
            if (!(le & PTE_USER))
            {
                dpt[li] = le; /* 非用户页：共享映射，不归还 */
                continue;
            }
            {
                void *np = (void *)alloc_page();

                if (np == NULL)
                    return -1;
                kernel_memcpy(np, (void *)(uintptr_t)(le & PTE_ADDR_MASK),
                              (uint32_t)PAGE_SIZE);
                dpt[li] = (le & ~PTE_ADDR_MASK) | ((uint64_t)(uintptr_t)np & PTE_ADDR_MASK);
            }
        }
    }
    return 0;
}

void vmm_free_user_pml4(uint64_t *pml4, uint64_t start, uint64_t end)
{
    uint64_t slot;
    uint64_t *pdpt;
    uint64_t *pd;
    int pdi;

    if (pml4 == NULL)
        return;
    if (user_slot_ok(start, end, &slot) != 0)
    {
        fput_string("[VMM] free_user_pml4: bad range\n");
        return;
    }
    if ((pml4[0] & PTE_PRESENT) != 0)
    {
        pdpt = table_from_entry(pml4[0]);
        if (pdpt != NULL && (pdpt[slot] & PTE_PRESENT) && !(pdpt[slot] & PTE_HUGE))
        {
            pd = table_from_entry(pdpt[slot]);
            if (pd != NULL)
            {
                for (pdi = 0; pdi < 512; pdi++)
                {
                    uint64_t pde = pd[pdi];

                    if (!(pde & PTE_PRESENT) || (pde & PTE_HUGE))
                        continue;
                    {
                        uint64_t *pt = table_from_entry(pde);
                        int li;

                        if (pt == NULL)
                            continue;
                        for (li = 0; li < 512; li++)
                        {
                            uint64_t le = pt[li];

                            if ((le & PTE_PRESENT) && (le & PTE_USER) && !(le & PTE_HUGE))
                                free_page(le & PTE_ADDR_MASK);
                        }
                        free_page((uint64_t)(uintptr_t)pt);
                    }
                    pd[pdi] = 0;
                }
            }
            free_page((uint64_t)(uintptr_t)pd);
        }
        free_page((uint64_t)(uintptr_t)pdpt);
    }
    free_page((uint64_t)(uintptr_t)pml4);
}

