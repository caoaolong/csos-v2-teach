#include <gdt.h>
#include <kernel.h>
#include <kernel_layout.h>
#include <serial.h>
#include <syscall.h>
#include <timer.h>
#include <user.h>
#include <gfx/console.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE (1ULL << 0) /* 允许 SYSCALL/SYSRET */

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

void init_syscall()
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR,
          ((uint64_t)USER32_CS_SEG << 48) |
              ((uint64_t)KERNEL_CODE_SEG << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, 0);

    fput_string("[SYSCALL] syscall/sysret ready lstar=0x%llx\n",
                (unsigned long long)(uintptr_t)syscall_entry);
}

static int user_mem_ok(uint64_t vaddr, uint64_t size)
{
    if (size == 0)
        return 1;
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

static uint64_t do_sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    const void *p = (const void *)(uintptr_t)ubuf;

    (void)fd;
    if (len == 0)
        return 0;
    if (!user_mem_ok(ubuf, len))
        return (uint64_t)-1;

    fb_console_write(p, len);
    return len;
}

static uint64_t do_sys_exit(uint64_t code, uint64_t dummy1, uint64_t dummy2)
{
    (void)dummy1;
    (void)dummy2;
    fput_string("[USER] exit(%llu), parked\n", (unsigned long long)code);
    fb_console_puts("\n[init exited]\n");
    for (;;)
        __asm__ volatile("hlt");
    return 0; /* 不可达 */
}

static uint64_t do_sys_sleep(uint64_t ms, uint64_t dummy1, uint64_t dummy2)
{
    (void)dummy1;
    (void)dummy2;
    msleep(ms);
    return 0;
}

typedef uint64_t (*sys_fn_t)(uint64_t rdi, uint64_t rsi, uint64_t rdx);

static const sys_fn_t g_sys_table[SYS_SLEEP + 1] = {
    [SYS_WRITE] = do_sys_write,
    [SYS_EXIT] = do_sys_exit,
    [SYS_SLEEP] = do_sys_sleep,
};

void handler_syscall(exception_frame_t *frame)
{
    uint64_t num = frame->rax;
    sys_fn_t fn;

    if (num > SYS_SLEEP || g_sys_table[num] == NULL)
    {
        fput_string("[SYSCALL] unknown num=%llu\n",
                    (unsigned long long)num);
        frame->rax = (uint64_t)-1; /* ENOSYS */
        return;
    }

    fn = g_sys_table[num];
    frame->rax = fn(frame->rdi, frame->rsi, frame->rdx);
}
