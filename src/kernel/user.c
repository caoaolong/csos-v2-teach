#include <user.h>
#include <elf.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/heap.h>
#include <serial.h>
#include <string.h>
#include <syscall.h>
#include <tss.h>
#include <fs/vfs.h>
#include <kernel_layout.h>

extern void enter_user(uint64_t rip, uint64_t rsp);

static void *g_kernel_stack_page;

uint64_t syscall_kstack_top;
uint64_t syscall_user_rsp;

/* 最小 loader 上限：静态可控，不做大文件/动态链接 */
#define ELF_MAX_FILE_SIZE (8ULL * 1024 * 1024)
#define ELF_MAX_SEGS 16
#define ELF_MAX_SEG_MEMSZ (16ULL * 1024 * 1024)

static void user_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

/* 按字节拷贝：绕开 kernel_memcpy 的 uint32_t 长度限制；目标为内核可写的物理页 */
static void raw_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static uint64_t align_down_u64(uint64_t v, uint64_t a)
{
    return v & ~(a - 1);
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* 用户地址合法性：低半部、非空页、不进内核高半核 */
static int user_range_ok(uint64_t vaddr, uint64_t size)
{
    if (size == 0)
        return 0;
    if (vaddr < PAGE_SIZE)
        return 0;
    if (vaddr + size < vaddr)
        return 0; /* 回绕 */
    if (vaddr + size > (uint64_t)0x0000800000000000ULL)
        return 0; /* 须在 canonical 低半部 */
    if (vaddr + size > KERNEL_VIRT_BASE)
        return 0;
    return 1;
}

/* 逐页分配、按最终权限映射；文件内容先写入物理页再映射，
 * 避免经只读用户虚地址拷贝触发 WP fault（CR0.WP=1 时内核也不能写只读页）。 */
static int map_one_segment(uint64_t vaddr, uint64_t file_off,
                           uint64_t filesz, uint64_t memsz,
                           int writable, const uint8_t *img)
{
    uint64_t base = align_down_u64(vaddr, PAGE_SIZE);
    uint64_t end = align_up_u64(vaddr + memsz, PAGE_SIZE);
    uint64_t va;
    uint64_t flags = PTE_PRESENT | PTE_USER | (writable ? PTE_WRITABLE : 0);
    uint64_t seg_file_end = vaddr + filesz;

    for (va = base; va < end; va += PAGE_SIZE)
    {
        void *page;
        uint64_t copy_start;
        uint64_t copy_end;
        uint64_t page_va_end = va + PAGE_SIZE;

        page = (void *)alloc_page();
        if (page == NULL)
            return -1;
        kernel_memset(page, 0, (uint32_t)PAGE_SIZE);

        /* 本页与 [vaddr, vaddr+filesz) 的交集即需拷贝部分 */
        copy_start = va > vaddr ? va : vaddr;
        copy_end = page_va_end < seg_file_end ? page_va_end : seg_file_end;
        if (copy_end > copy_start)
        {
            uint64_t img_off = file_off + (copy_start - vaddr);
            uint64_t page_off = copy_start - va;

            raw_copy((uint8_t *)page + page_off,
                     img + img_off, copy_end - copy_start);
        }

        if (map_page(va, (uint64_t)(uintptr_t)page, flags) != 0)
            return -1;
    }
    return 0;
}

static int check_ehdr(const elf64_ehdr_t *e, uint64_t file_size)
{
    if (e->e_ident[0] != ELF_MAGIC0 || e->e_ident[1] != ELF_MAGIC1 ||
        e->e_ident[2] != ELF_MAGIC2 || e->e_ident[3] != ELF_MAGIC3)
    {
        put_string("[USER] ELF bad magic\n");
        return -1;
    }
    if (e->e_ident[4] != ELFCLASS64 || e->e_ident[5] != ELFDATA2LSB ||
        e->e_ident[6] != EV_CURRENT)
    {
        put_string("[USER] ELF only support 64-bit LE v1\n");
        return -1;
    }
    if (e->e_type != ET_EXEC)
    {
        fput_string("[USER] ELF type=%u not EXEC (no DYN/PIE yet)\n", e->e_type);
        return -1;
    }
    if (e->e_machine != EM_X86_64)
    {
        put_string("[USER] ELF machine != x86-64\n");
        return -1;
    }
    if (e->e_phentsize != sizeof(elf64_phdr_t))
    {
        put_string("[USER] ELF bad phentsize\n");
        return -1;
    }
    if (e->e_phnum == 0 || e->e_phnum > ELF_MAX_SEGS)
    {
        fput_string("[USER] ELF bad phnum=%u\n", e->e_phnum);
        return -1;
    }
    if (e->e_phoff + (uint64_t)e->e_phnum * sizeof(elf64_phdr_t) > file_size)
    {
        put_string("[USER] ELF phdr past EOF\n");
        return -1;
    }
    if (e->e_entry < PAGE_SIZE || e->e_entry >= (uint64_t)0x0000800000000000ULL)
    {
        put_string("[USER] ELF bad entry\n");
        return -1;
    }
    return 0;
}

/* 整文件读入 kmalloc 缓冲；返回 0 成功，buf/size 输出 */
static int load_file_to_mem(const char *path, uint8_t **out_buf, uint64_t *out_size)
{
    vfs_file_t *f = NULL;
    uint64_t size = 0;
    uint32_t is_dir = 0;
    uint8_t *buf;
    uint64_t off = 0;
    int rc;

    if (path == NULL || path[0] == '\0' || out_buf == NULL || out_size == NULL)
        return -1;

    rc = vfs_stat(path, &size, &is_dir);
    if (rc != VFS_OK)
    {
        fput_string("[USER] stat '%s' rc=%d\n", path, rc);
        return -1;
    }
    if (is_dir || size < sizeof(elf64_ehdr_t) || size > ELF_MAX_FILE_SIZE)
    {
        fput_string("[USER] bad init size=%llu\n", (unsigned long long)size);
        return -1;
    }

    buf = (uint8_t *)kmalloc((size_t)size);
    if (buf == NULL)
    {
        put_string("[USER] kmalloc for init failed\n");
        return -1;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != VFS_OK)
    {
        fput_string("[USER] open '%s' rc=%d\n", path, rc);
        kfree(buf);
        return -1;
    }

    while (off < size)
    {
        uint64_t chunk = size - off;
        int64_t n;

        if (chunk > 0x7FFFFFFFULL)
            chunk = 0x7FFFFFFFULL;
        n = vfs_read(f, buf + off, chunk);
        if (n <= 0)
        {
            fput_string("[USER] read '%s' failed n=%lld off=%llu\n",
                        path, (long long)n, (unsigned long long)off);
            vfs_close(f);
            kfree(buf);
            return -1;
        }
        off += (uint64_t)n;
    }
    vfs_close(f);

    *out_buf = buf;
    *out_size = size;
    return 0;
}

static void setup_kstack_and_enter(uint64_t rip)
{
    void *stack_page = (void *)alloc_page();
    uint64_t kstack_top;

    if (stack_page == NULL)
        user_halt("FATAL: user stack alloc failed\n");
    kernel_memset(stack_page, 0, (uint32_t)PAGE_SIZE);
    if (map_page(USER_STACK_VADDR, (uint64_t)(uintptr_t)stack_page,
                 PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0)
        user_halt("FATAL: map user stack failed\n");

    if (g_kernel_stack_page == NULL)
    {
        g_kernel_stack_page = (void *)alloc_page();
        if (g_kernel_stack_page == NULL)
            user_halt("FATAL: syscall kstack alloc failed\n");
        kernel_memset(g_kernel_stack_page, 0, (uint32_t)PAGE_SIZE);
    }

    kstack_top = (uint64_t)(uintptr_t)g_kernel_stack_page + PAGE_SIZE;
    syscall_kstack_top = kstack_top; /* 供 syscall_entry 切栈 */
    tss_set_rsp0(kstack_top);

    fput_string("[USER] enter rip=0x%llx rsp=0x%llx rsp0=0x%llx\n",
                (unsigned long long)rip,
                (unsigned long long)USER_STACK_TOP,
                (unsigned long long)kstack_top);

    enter_user(rip, USER_STACK_TOP);
    user_halt("FATAL: enter_user returned\n");
}

/* 从已加载的 ELF 内存镜像映射 PT_LOAD；返回入口地址，失败返回 0 */
static uint64_t map_elf_image(const uint8_t *img, uint64_t file_size)
{
    const elf64_ehdr_t *e = (const elf64_ehdr_t *)img;
    const elf64_phdr_t *ph;
    uint16_t i;
    int n_load = 0;

    if (check_ehdr(e, file_size) != 0)
        return 0;

    ph = (const elf64_phdr_t *)(img + e->e_phoff);
    for (i = 0; i < e->e_phnum; i++)
    {
        if (ph[i].p_type != PT_LOAD)
        {
            if (ph[i].p_type == PT_INTERP)
                put_string("[USER] ELF needs INTERP (dynamic), not supported\n");
            continue;
        }
        n_load++;

        if (ph[i].p_filesz > ph[i].p_memsz)
        {
            put_string("[USER] ELF bad filesz>memsz\n");
            return 0;
        }
        if (ph[i].p_memsz > ELF_MAX_SEG_MEMSZ)
        {
            put_string("[USER] ELF segment too large\n");
            return 0;
        }
        if (ph[i].p_offset + ph[i].p_filesz > file_size ||
            ph[i].p_offset + ph[i].p_filesz < ph[i].p_offset)
        {
            put_string("[USER] ELF segment past EOF\n");
            return 0;
        }
        if (!user_range_ok(ph[i].p_vaddr, ph[i].p_memsz))
        {
            fput_string("[USER] ELF bad vaddr=0x%llx memsz=0x%llx\n",
                        (unsigned long long)ph[i].p_vaddr,
                        (unsigned long long)ph[i].p_memsz);
            return 0;
        }

        if (map_one_segment(ph[i].p_vaddr, ph[i].p_offset,
                            ph[i].p_filesz, ph[i].p_memsz,
                            (ph[i].p_flags & PF_W) != 0, img) != 0)
            return 0;

        fput_string("[USER] LOAD vaddr=0x%llx filesz=0x%llx memsz=0x%llx flags=%u\n",
                    (unsigned long long)ph[i].p_vaddr,
                    (unsigned long long)ph[i].p_filesz,
                    (unsigned long long)ph[i].p_memsz,
                    (unsigned)ph[i].p_flags);
    }

    if (n_load == 0)
    {
        put_string("[USER] ELF has no LOAD segments\n");
        return 0;
    }

    /* 入口必须落在某个 LOAD 段内 */
    {
        int ok = 0;

        for (i = 0; i < e->e_phnum; i++)
        {
            if (ph[i].p_type != PT_LOAD)
                continue;
            if (e->e_entry >= ph[i].p_vaddr &&
                e->e_entry < ph[i].p_vaddr + ph[i].p_memsz)
            {
                ok = 1;
                break;
            }
        }
        if (!ok)
        {
            put_string("[USER] ELF entry outside LOAD\n");
            return 0;
        }
    }

    return e->e_entry;
}

void user_enter_init(const char *init_path)
{
    uint8_t *img = NULL;
    uint64_t file_size = 0;
    uint64_t entry;

    if (init_path == NULL || init_path[0] == '\0')
        user_halt("FATAL: empty init path\n");

    fput_string("[USER] loading init '%s'\n", init_path);
    if (load_file_to_mem(init_path, &img, &file_size) != 0)
        user_halt("FATAL: load init failed\n");

    entry = map_elf_image(img, file_size);
    kfree(img);

    if (entry == 0)
        user_halt("FATAL: ELF parse/map init failed\n");

    fput_string("[USER] init '%s' mapped entry=0x%llx size=%llu\n",
                init_path, (unsigned long long)entry,
                (unsigned long long)file_size);
    setup_kstack_and_enter(entry);
}
