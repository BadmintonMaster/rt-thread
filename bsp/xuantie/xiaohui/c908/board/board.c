/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-04-23     Wangshun     first version
 * 2026-08-13     chenguohao   add xiaohui C908 rt-smart support
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "board.h"
#include "tick.h"
#include "drv_uart.h"
#include <interrupt.h>

#ifdef RT_USING_SMART
#include <mmu.h>
#include <riscv_mmu.h>
#include <mm_aspace.h>
#include <mm_page.h>
#include <ioremap.h>
#include <lwp_arch.h>
#endif

#ifdef RT_USING_SMART

rt_region_t init_page_region = { (rt_size_t)RT_HW_PAGE_START, (rt_size_t)RT_HW_PAGE_END };

extern size_t MMUTable[];

/* map the whole rt-smart managed physical range */
struct mem_desc platform_mem_desc[] = {
    { KERNEL_VADDR_START + MEM_PHYS_BASE,
      KERNEL_VADDR_START + MEM_PHYS_BASE + MEM_RTSMART_SIZE - 1,
      (rt_size_t)ARCH_MAP_FAILED, NORMAL_MEM },
};

#define NUM_MEM_DESC (sizeof(platform_mem_desc) / sizeof(platform_mem_desc[0]))

#endif /* RT_USING_SMART */

#ifndef ARCH_REMAP_KERNEL
#define IOREMAP_VEND USER_VADDR_START
#else
#define IOREMAP_VEND 0ul
#endif

#define IOREMAP_SIZE (1ul << 30)

/* satp modes supported by hardware, probed by the M-mode boot shim */
unsigned long g_mmu_modes_supported = 0;

static volatile rt_uint32_t *stimecmp_reg = RT_NULL;

void sbi_init(void)
{
    /* no SBI firmware on this platform */
}

void sbi_print_version(void)
{
}

void sbi_set_timer(rt_uint64_t stime_value)
{
    if (stimecmp_reg)
    {
        /* CLINT supervisor timecmp (memory mapped, S-mode writable),
         * one 64-bit register per hart at STIMECMP + hartid * 8 */
        int cpu = rt_hw_cpu_id();

        stimecmp_reg[cpu * 2] = (rt_uint32_t)(stime_value & 0xFFFFFFFF);
        stimecmp_reg[cpu * 2 + 1] = (rt_uint32_t)(stime_value >> 32);
    }
}

void sbi_console_putchar(int ch)
{
    (void)ch;
}

int sbi_console_getchar(void)
{
    return -1;
}

void sbi_shutdown(void)
{
    while (1);
}

int sbi_remote_sfence_vma(const unsigned long *hart_mask,
                          const unsigned long hart_mask_base,
                          unsigned long start, unsigned long size)
{
    __asm__ volatile("sfence.vma" ::: "memory");
#ifdef RT_USING_SMP
    /* ask the other harts to flush their TLB via IPI */
    {
        unsigned int mask = ((1U << RT_CPUS_NR) - 1) & ~(1U << rt_hw_cpu_id());

        if (mask)
        {
            rt_hw_ipi_send(RT_MAX_IPI - 1, mask);
        }
    }
#endif
    return 0;
}

void sbi_send_ipi(const unsigned long *hart_mask)
{
#ifdef RT_USING_SMP
    /* IPIs are raised directly through the CLINT SSIP registers by
     * rt_hw_ipi_send(), nothing to do here. */
#endif
}

void sbi_remote_fence_i(const unsigned long *hart_mask)
{
    __asm__ volatile("fence.i" ::: "memory");
#ifdef RT_USING_SMP
    {
        unsigned int mask = ((1U << RT_CPUS_NR) - 1) & ~(1U << rt_hw_cpu_id());

        if (mask)
        {
            rt_hw_ipi_send(RT_MAX_IPI - 1, mask);
        }
    }
#endif
}

#ifdef RT_USING_SMP
/* IPI handler on the remote hart: flush local TLB and instruction stream */
static void _tlb_flush_ipi(int vector, void *param)
{
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("fence.i" ::: "memory");
}

int sbi_hsm_hart_start(unsigned long hart, unsigned long saddr, unsigned long priv)
{
    static volatile void *sreset_base = RT_NULL;
    volatile unsigned long *release = (volatile unsigned long *)(KERNEL_VADDR_START + MEM_PHYS_BASE + MEM_BOOT_SCRATCH_OFF + 8);

    if (hart == 0 || hart >= RT_CPUS_NR)
    {
        return -1;
    }

    if (sreset_base == RT_NULL)
    {
        sreset_base = rt_ioremap((void *)0x18030000UL /* XIAOHUI_SRESET_BASE */, 0x1000);
        if (sreset_base == RT_NULL)
        {
            return -1;
        }
    }

    /* secondary entry = M-mode boot shim (Reset_Handler) at the DRAM base */
    *(volatile unsigned long *)((char *)sreset_base + 0x10 + ((hart - 1) << 3)) = MEM_PHYS_BASE;

    /* release the shim park loop */
    *release |= (1UL << hart);
    __asm__ volatile("fence rw, rw" ::: "memory");

    /* release the hart from reset (xiaohui RESET controller);
     * harmless if the hart is already spinning in the park loop */
    *(volatile unsigned int *)sreset_base = 0x7f;

    return 0;
}
#endif /* RT_USING_SMP */

rt_uint64_t rt_hw_get_clock_timer_freq(void)
{
    /* xiaohui coretim runs at 25MHz */
    return 25000000ULL;
}

void init_bss(void)
{
    unsigned int *dst;

    dst = &__bss_start;
    while ((rt_ubase_t)dst < (rt_ubase_t)&__bss_end)
    {
        *dst++ = 0;
    }
}

/* lock-free UART output for the assert path: rt_kprintf may block on the
 * console lock, which can be part of the failure being reported */
static volatile rt_uint32_t *raw_uart = RT_NULL;

static void _raw_putc(char c)
{
    if (raw_uart == RT_NULL)
    {
        return;
    }
    while ((raw_uart[5] & 0x20) == 0)
        ;
    raw_uart[0] = (rt_uint32_t)(rt_uint8_t)c;
}

static void _raw_puts(const char *s)
{
    while (*s)
    {
        if (*s == '\n')
        {
            _raw_putc('\r');
        }
        _raw_putc(*s++);
    }
}

static void _raw_putdec(rt_size_t v)
{
    char buf[24];
    int i = 0;

    do
    {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i < (int)sizeof(buf));
    while (i > 0)
    {
        _raw_putc(buf[--i]);
    }
}

static void __rt_assert_handler(const char *ex_string, const char *func, rt_size_t line)
{
    _raw_puts("(");
    _raw_puts(ex_string);
    _raw_puts(") assertion failed at function:");
    _raw_puts(func);
    _raw_puts(", line number:");
    _raw_putdec(line);
    _raw_puts(" (cpu");
    _raw_putdec((rt_size_t)rt_hw_cpu_id());
    _raw_puts(")\n");
    asm volatile("ebreak" ::: "memory");
}

void primary_cpu_entry(void)
{
    /* disable global interrupt */
    rt_hw_interrupt_disable();
    rt_assert_set_hook(__rt_assert_handler);
    entry();
}

void rt_hw_board_init(void)
{
#ifdef RT_USING_SMART
    /* init data structure */
    rt_hw_mmu_map_init(&rt_kernel_space, (void *)(IOREMAP_VEND - IOREMAP_SIZE), IOREMAP_SIZE, (rt_size_t *)MMUTable, PV_OFFSET);

    /* init page allocator */
    rt_page_init(init_page_region);

    /* setup region, and enable MMU */
    rt_hw_mmu_setup(&rt_kernel_space, platform_mem_desc, NUM_MEM_DESC);
#endif

#ifdef RT_USING_HEAP
    /* initialize memory system */
    rt_system_heap_init(RT_HW_HEAP_BEGIN, RT_HW_HEAP_END);
#endif

    /* initialize interrupt controller */
    rt_hw_interrupt_init();

#ifdef RT_USING_SMP
    /* boot hart IPI init (maps the CLINT SSIP page); secondary harts call
     * rt_hw_ipi_init() themselves in secondary_cpu_entry() */
    rt_hw_ipi_init();
    rt_hw_ipi_handler_install(RT_MAX_IPI - 1, _tlb_flush_ipi);
#endif

#ifdef RT_USING_SMART
    /* map CLINT for the direct-hw sbi_set_timer */
    stimecmp_reg = (volatile rt_uint32_t *)rt_ioremap(
        (void *)(XIAOHUI_CLINT_PHY_ADDR + XIAOHUI_STIMECMP_OFF), 0x1000);

    /* private UART mapping for the lock-free assert output path */
    raw_uart = (volatile rt_uint32_t *)rt_ioremap((void *)XIAOHUI_UART0_PHY_ADDR, 0x1000);

    /* fetch the satp mode probe result left by the M-mode boot shim */
    g_mmu_modes_supported = *(volatile unsigned long *)(KERNEL_VADDR_START + MEM_PHYS_BASE + MEM_BOOT_SCRATCH_OFF);
#endif

    /* uart driver */
    rt_hw_uart_init();

    /* tick timer */
    rt_hw_tick_init();

#ifdef RT_USING_CONSOLE
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif /* RT_USING_CONSOLE */

#ifdef RT_USING_SMART
    /* report active/supported MMU translation modes */
    {
        rt_ubase_t satp;

        __asm__ volatile("csrr %0, satp" : "=r"(satp));
        rt_kprintf("MMU: active Sv%d, hw support:%s%s%s\n",
                   ((satp >> 60) & 0xF) == 8 ? 39 : ((satp >> 60) & 0xF) == 9 ? 48
                                                : ((satp >> 60) & 0xF) == 10  ? 57
                                                                              : 0,
                   (g_mmu_modes_supported >> 8) & 1 ? " Sv39" : "",
                   (g_mmu_modes_supported >> 9) & 1 ? " Sv48" : "",
                   (g_mmu_modes_supported >> 10) & 1 ? " Sv57" : "");
    }
#endif

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}

void rt_hw_cpu_reset(void)
{
    while (1);
}
MSH_CMD_EXPORT_ALIAS(rt_hw_cpu_reset, reboot, reset machine);

/* UNUSED, but keep for reference */
#if 0
#ifdef RT_USING_SMP

#define c9xx_csr_read(num)                              \
    ({                                                  \
        unsigned long __v;                              \
        __asm__ volatile("csrr %0, " #num : "=r"(__v)); \
        __v;                                            \
    })

#define c9xx_csr_write(num, val)                            \
    do                                                      \
    {                                                       \
        unsigned long __v = (unsigned long)(val);           \
        __asm__ volatile("csrw " #num ", %0" : : "r"(__v)); \
    } while (0)

extern void Reset_Handler(void);

static struct c9xx_regs_struct {
    uint64_t pmpaddr0;
    uint64_t pmpaddr1;
    uint64_t pmpaddr2;
    uint64_t pmpaddr3;
    uint64_t pmpaddr4;
    uint64_t pmpaddr5;
    uint64_t pmpaddr6;
    uint64_t pmpaddr7;
    uint64_t pmpcfg0;
    uint64_t mcor;
    uint64_t mhcr;
    uint64_t mccr2;
    uint64_t mhint;
    uint64_t msmpr;
    uint64_t mie;
    uint64_t mxstatus;
    uint64_t mtvec;
    uint64_t plic_base_addr;
    uint64_t clint_base_addr;
} c9xx_regs;

#define C9xx_PLIC_CLINT_OFFSET     0x04000000  /* 64M */
#define C9xx_PLIC_DELEG_OFFSET     0x001ffffc
#define C9xx_PLIC_DELEG_ENABLE     0x1

#define XIAOHUI_SRESET_BASE                 0x18030000
#define XIAOHUI_SRESET_ADDR_OFFSET          0x10
#define PRIMARY_STARTUP_CORE_ID                0

static void c9xx_csr_copy(void)
{
    if (PRIMARY_STARTUP_CORE_ID == c9xx_csr_read(mhartid))
    {
        /* Load from boot core */
        c9xx_regs.pmpaddr0 = c9xx_csr_read(pmpaddr0);
        c9xx_regs.pmpaddr1 = c9xx_csr_read(pmpaddr1);
        c9xx_regs.pmpaddr2 = c9xx_csr_read(pmpaddr2);
        c9xx_regs.pmpaddr3 = c9xx_csr_read(pmpaddr3);
        c9xx_regs.pmpaddr4 = c9xx_csr_read(pmpaddr4);
        c9xx_regs.pmpaddr5 = c9xx_csr_read(pmpaddr5);
        c9xx_regs.pmpaddr6 = c9xx_csr_read(pmpaddr6);
        c9xx_regs.pmpaddr7 = c9xx_csr_read(pmpaddr7);
        c9xx_regs.pmpcfg0  = c9xx_csr_read(pmpcfg0);
        c9xx_regs.mcor     = c9xx_csr_read(mcor);
        c9xx_regs.msmpr    = c9xx_csr_read(msmpr);
        c9xx_regs.mhcr     = c9xx_csr_read(mhcr);
        c9xx_regs.mccr2    = c9xx_csr_read(mccr2);
        c9xx_regs.mhint    = c9xx_csr_read(mhint);
        c9xx_regs.mtvec    = c9xx_csr_read(mtvec);
        c9xx_regs.mie      = c9xx_csr_read(mie);
        c9xx_regs.mxstatus = c9xx_csr_read(mxstatus);

        c9xx_regs.plic_base_addr = c9xx_csr_read(mapbaddr);
        c9xx_regs.clint_base_addr = c9xx_regs.plic_base_addr + C9xx_PLIC_CLINT_OFFSET;
    } else {
        /* Store to other core */
        // c9xx_csr_write(pmpaddr0, c9xx_regs.pmpaddr0);
        // c9xx_csr_write(pmpaddr1, c9xx_regs.pmpaddr1);
        // c9xx_csr_write(pmpaddr2, c9xx_regs.pmpaddr2);
        // c9xx_csr_write(pmpaddr3, c9xx_regs.pmpaddr3);
        // c9xx_csr_write(pmpaddr4, c9xx_regs.pmpaddr4);
        // c9xx_csr_write(pmpaddr5, c9xx_regs.pmpaddr5);
        // c9xx_csr_write(pmpaddr6, c9xx_regs.pmpaddr6);
        // c9xx_csr_write(pmpaddr7, c9xx_regs.pmpaddr7);
        // c9xx_csr_write(pmpcfg0,  c9xx_regs.pmpcfg0);
        c9xx_csr_write(mcor, c9xx_regs.mcor);
        c9xx_csr_write(msmpr, c9xx_regs.msmpr);
        c9xx_csr_write(mhcr, c9xx_regs.mhcr);
        c9xx_csr_write(mhint, c9xx_regs.mhint);
        // c9xx_csr_write(mtvec, c9xx_regs.mtvec);
        // c9xx_csr_write(mie,   c9xx_regs.mie);
        c9xx_csr_write(mxstatus, c9xx_regs.mxstatus);
    }
}

void riscv_soc_start_cpu(int cpu_num)
{
    if (cpu_num < 1) {
        return;
    }

    c9xx_csr_copy();

    *(unsigned long *)((unsigned long)XIAOHUI_SRESET_BASE + XIAOHUI_SRESET_ADDR_OFFSET + ((cpu_num - 1) << 3)) = (unsigned long)Reset_Handler;
#if __riscv_xtheadsync
    __ASM("sync");
#endif

    /* Release secondary cpus, Determined by the xiaohui FPGA platform */
    *(uint32_t *)(XIAOHUI_SRESET_BASE) = 0x7f;
    // uint32_t mrmr = *(uint32_t *)(XIAOHUI_SRESET_BASE);
    // *(uint32_t *)(XIAOHUI_SRESET_BASE) = mrmr | (0x1 << (cpu_num - 1));
#if __riscv_xtheadsync
    __ASM("sync");
#endif
}
#endif /* RT_USING_SMP */
#endif /* 0, UNUSED reference code kept above */
