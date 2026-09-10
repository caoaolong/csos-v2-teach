#include <apic.h>
#include <apic/lapic.h>
#include <memory/vmm.h>
#include <serial.h>
#include <pic.h>
#include <pit.h>
#include <cpu.h>
#include <timer.h>

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

#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310

#define ICR_VECTOR_MASK 0xFFu
#define ICR_DELIVERY_INIT (5u << 8)
#define ICR_DELIVERY_SIPI (6u << 8)
#define ICR_DELIVERY_STATUS (1u << 12)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_TRIGGER_LEVEL (1u << 15)

/* BSP 校准后的周期初值；AP 直接复用 */
static uint32_t g_apic_timer_init_count;

uint32_t g_bsp_apic_id;

static volatile uint32_t *g_lapic;

static void lapic_halt(const char *msg)
{
    put_string(msg);
    for (;;)
        __asm__ volatile("hlt");
}

static void timer_halt(const char *msg)
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

void lapic_eoi()
{
    if (g_lapic != NULL)
        lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id()
{
    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_id before init_lapic\n");
    return lapic_read(LAPIC_ID) >> 24;
}

static void lapic_wait_icr()
{
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS)
        cpu_pause();
}

static void lapic_icr_write(uint32_t dest_apic_id, uint32_t low)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
}

/* INIT-SIPI-SIPI：vector_page 为 trampoline 物理页号（物理地址 >> 12） */
void lapic_start_ap(uint32_t apic_id, uint8_t vector_page)
{
    uint32_t i;

    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_start_ap before init_lapic\n");

    /* INIT assert */
    lapic_icr_write(apic_id,
                    ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    /* Intel 建议： */
    for (i = 0; i < 1000000; i++)
        cpu_pause();

    /* INIT deassert */
    lapic_icr_write(apic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
    /* Intel 建议： */
    for (i = 0; i < 1000000; i++)
        cpu_pause();

    /* SIPI × 2 */
    lapic_icr_write(apic_id, ICR_DELIVERY_SIPI | (vector_page & ICR_VECTOR_MASK));
    /* Intel 建议：SIPI 后等待约 200 微秒 */
    for (i = 0; i < 20000; i++)
        cpu_pause();
    lapic_icr_write(apic_id, ICR_DELIVERY_SIPI | (vector_page & ICR_VECTOR_MASK));
}

void init_apic_timer(uint32_t freq_hz)
{
    uint32_t end_count;
    uint32_t elapsed;
    uint32_t init_count;
    uint64_t ticks_per_sec;

    if (freq_hz == 0)
        timer_halt("FATAL: init_apic_timer freq_hz=0\n");

    /* 校准在 sti 之前；再 cli 一次做防御 */
    __asm__ volatile("cli");

    lapic_timer_stop();

    if (pit_ch2_oneshot_start(PIT_CALIBRATE_MS) != 0)
        timer_halt("FATAL: PIT Ch2 oneshot start failed\n");

    /* 紧接 PIT 启动后开始倒计时，缩小窗口误差 */
    lapic_timer_calib_start();

    while (!pit_ch2_expired())
        __asm__ volatile("pause");

    end_count = lapic_timer_current();
    pit_ch2_stop();
    lapic_timer_stop();

    elapsed = 0xFFFFFFFFu - end_count;
    if (elapsed == 0)
        timer_halt("FATAL: APIC timer calibration elapsed=0\n");

    /* 10ms 窗口：ticks/sec = elapsed * 1000 / ms */
    ticks_per_sec = ((uint64_t)elapsed * 1000ull) / (uint64_t)PIT_CALIBRATE_MS;
    init_count = (uint32_t)(ticks_per_sec / (uint64_t)freq_hz);
    if (init_count == 0)
        timer_halt("FATAL: APIC timer init_count=0\n");

    g_apic_timer_init_count = init_count;
    timer_set_hz(freq_hz);
    lapic_timer_start(init_count, APIC_TIMER_VECTOR);

    fput_string("[LAPIC] timer periodic vector=%u init_count=%u hz=%u\n",
                (unsigned)APIC_TIMER_VECTOR, (unsigned)init_count, (unsigned)freq_hz);
}

void lapic_timer_start(uint32_t init_count, uint8_t vector)
{
    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_timer_start before init_lapic\n");

    /* 先停定时器 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_INIT_COUNT, 0);

    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | (uint32_t)vector);
    lapic_write(LAPIC_INIT_COUNT, init_count);

    fput_string("[LAPIC] timer periodic vector=%u init_count=%u (qemu hardcoded)\n",
                (unsigned)vector, (unsigned)init_count);
}

void lapic_timer_stop()
{
    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_timer_stop before init_lapic\n");

    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_INIT_COUNT, 0);
}

void lapic_timer_calib_start()
{
    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_timer_calib_start before init_lapic\n");

    /* 屏蔽 LVT，避免校准期间投递；oneshot（不置 PERIODIC） */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_INIT_COUNT, 0);
    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_INIT_COUNT, 0xFFFFFFFFu);
}

uint32_t lapic_timer_current()
{
    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_timer_current before init_lapic\n");
    return lapic_read(LAPIC_CUR_COUNT);
}

void apic_timer_start_calibrated()
{
    if (g_apic_timer_init_count == 0)
        timer_halt("FATAL: apic_timer_start_calibrated before init_apic_timer\n");
    lapic_timer_start(g_apic_timer_init_count, APIC_TIMER_VECTOR);
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
    g_bsp_apic_id = id;
    fput_string("[LAPIC] base=0x%llx id=%u version=0x%x\n",
                (unsigned long long)phys, (unsigned)id, (unsigned)ver);
}

void lapic_ap_init()
{
    uint64_t msr;

    if (g_lapic == NULL)
        lapic_halt("FATAL: lapic_ap_init: LAPIC not mapped\n");

    msr = rdmsr(IA32_APIC_BASE_MSR);
    msr |= APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, msr);

    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
}