#include <apic.h>
#include <apic/lapic.h>
#include <memory/vmm.h>
#include <serial.h>
#include <pic.h>

#define LAPIC_ID 0x020
#define LAPIC_VER 0x030
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_INIT_COUNT 0x380
#define LAPIC_CUR_COUNT 0x390
#define LAPIC_DIVIDE 0x3E0

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_ADDR_MASK 0xFFFFF000ULL

#define LAPIC_SVR_ENABLE (1u << 8)
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_LVT_MASKED (1u << 16)
#define LAPIC_DIVIDE_BY_16 0x3u

static volatile uint32_t *g_lapic;

static void lapic_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

static uint32_t lapic_read(uint32_t reg)
{
    return g_lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value)
{
    g_lapic[reg / 4] = value;
    /* 读 ID 寄存器作同步，避免部分硬件上的写缓冲延迟 */
    (void)lapic_read(LAPIC_ID);
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void init_lapic()
{
    const madt_info_t *madt;
    uint64_t phys;
    uint64_t msr;
    uint32_t id;
    uint32_t ver;

    madt = acpi_madt();
    if (madt == NULL || madt->lapic_addr == 0)
        lapic_halt("FATAL: LAPIC address unavailable\n");

    phys = madt->lapic_addr & ~(PAGE_SIZE - 1);
    int map_rc = map_page(phys, phys,
                          PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    if (map_rc != 0)
    {
        fput_string("FATAL: map LAPIC MMIO failed rc=%d phys=0x%llx free=%llu\n",
                    map_rc,
                    (unsigned long long)phys,
                    (unsigned long long)pmm_free_pages());
        for (;;)
            __asm__ volatile("hlt");
    }

    g_lapic = (volatile uint32_t *)(uintptr_t)phys;

    msr = rdmsr(IA32_APIC_BASE_MSR);
    msr |= APIC_BASE_ENABLE;
    /* 保持/写回基址（与 MADT 一致时通常已正确） */
    if ((msr & APIC_BASE_ADDR_MASK) == 0)
        msr |= phys;
    wrmsr(IA32_APIC_BASE_MSR, msr);

    /* 先 remap + 全屏蔽，避免与异常向量冲突的旧 PIC 中断 */
    init_pic();
    pic_disable();

    // APIC_SPURIOUS_VECTOR = 0xFF，避免与 APIC_TIMER_VECTOR 冲突（伪中断向量）
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);

    id = lapic_read(LAPIC_ID) >> 24;
    ver = lapic_read(LAPIC_VER) & 0xFF;
    fput_string("[LAPIC] base=0x%llx id=%u version=0x%x\n",
                (unsigned long long)phys, (unsigned)id, (unsigned)ver);
}