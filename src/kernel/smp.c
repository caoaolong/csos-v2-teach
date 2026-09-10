#include <acpi.h>
#include <smp.h>
#include <cpu.h>
#include <apic.h>
#include <apic/lapic.h>
#include <serial.h>
#include <string.h>
#include <memory/vmm.h>
#include <gdt.h>
#include <idt.h>

#define ACPI_LAPIC_ENABLED (1u << 0)
#define SMP_TRAMPOLINE_PHYS 0x8000ULL
#define SMP_TRAMPOLINE_PAGE (SMP_TRAMPOLINE_PHYS >> 12)

extern char smp_trampoline_start[];
extern char smp_trampoline_end[];
extern char tramp_bootinfo[];

/* 与 smp_trampoline.S 中 tramp_bootinfo 布局一致 */
typedef struct smp_bootinfo
{
    volatile uint32_t started;
    uint32_t pad;
    uint64_t cr3;
    uint64_t stack;
    uint64_t entry;
} smp_bootinfo_t;

static smp_bootinfo_t *bootinfo_at_trampoline()
{
    size_t off = (size_t)(tramp_bootinfo - smp_trampoline_start);
    return (smp_bootinfo_t *)(uintptr_t)(SMP_TRAMPOLINE_PHYS + off);
}

static int start_one_ap(uint32_t apic_id)
{
    void *stack_page;
    uint8_t *stack_top;
    smp_bootinfo_t *bi;
    size_t tramp_size;
    uint32_t spins;

    tramp_size = (size_t)(smp_trampoline_end - smp_trampoline_start);
    if (tramp_size > PAGE_SIZE)
    {
        put_string("FATAL: smp trampoline > 4K\n");
        return -1;
    }

    stack_page = (void *)alloc_page();
    if (stack_page == NULL)
    {
        put_string("FATAL: smp AP stack alloc failed\n");
        return -1;
    }
    kernel_memset(stack_page, 0, (uint32_t)PAGE_SIZE);
    stack_top = (uint8_t *)stack_page + PAGE_SIZE;

    kernel_memcpy((void *)(uintptr_t)SMP_TRAMPOLINE_PHYS,
                  smp_trampoline_start, (uint32_t)tramp_size);

    bi = bootinfo_at_trampoline();
    bi->started = 0;
    bi->pad = 0;
    bi->cr3 = read_cr3();
    bi->stack = (uint64_t)(uintptr_t)stack_top;
    bi->entry = (uint64_t)(uintptr_t)ap_main;

    if (bi->cr3 > 0xFFFFFFFFull)
    {
        put_string("FATAL: CR3 above 4GiB; trampoline 32-bit CR3 load unsupported\n");
        return -1;
    }

    lapic_start_ap(apic_id, (uint8_t)SMP_TRAMPOLINE_PAGE);

    for (spins = 0; spins < 10000000u; spins++)
    {
        if (bi->started)
        {
            fput_string("[SMP] AP apic_id=%u acknowledged\n", (unsigned)apic_id);
            return 0;
        }
        cpu_pause();
    }

    fput_string("FATAL: AP apic_id=%u start timeout\n", (unsigned)apic_id);
    return -1;
}

void init_smp()
{
    const madt_info_t *madt;
    uint32_t bsp_id;
    uint32_t i;
    uint32_t started = 0;

    madt = acpi_madt();
    if (madt == NULL)
    {
        put_string("[SMP] no MADT, skip\n");
        return;
    }

    bsp_id = lapic_id();
    fput_string("[SMP] BSP apic_id=%u cpus=%u\n",
                (unsigned)bsp_id, (unsigned)madt->cpu_count);

    for (i = 0; i < madt->cpu_count; i++)
    {
        const acpi_cpu_t *cpu = &madt->cpus[i];

        if ((cpu->flags & ACPI_LAPIC_ENABLED) == 0)
            continue;
        if (cpu->apic_id == bsp_id)
            continue;

        if (start_one_ap(cpu->apic_id) == 0)
            started++;
    }

    fput_string("[SMP] brought up %u AP(s)\n", (unsigned)started);
}

void ap_main()
{
    gdt_reload();
    idt_reload();
    lapic_ap_init();
    sched_cpu_init();
    apic_timer_start_calibrated();

    fput_string("[SMP] AP apic_id=%u online\n", (unsigned)lapic_id());
    bootinfo_at_trampoline()->started = 1;

    __asm__ volatile("sti");

    for (;;)
        __asm__ volatile("hlt");
}