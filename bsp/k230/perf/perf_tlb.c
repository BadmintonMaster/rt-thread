/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        TLB / TWU (page-walk) micro-benchmarks
 */

/*
 * perf_tlb : three complementary views of the C908 address-translation path,
 *            aligned with the ARM Cortex-A53 TLB section of the reference.
 *
 *   (A) page-stride latency sweep  - one dependent access per 4 KiB page over
 *       an increasing page count. Latency stays flat while the working set of
 *       *pages* fits the L1 D-TLB (17 entries -> 68 KiB), steps up when it
 *       spills to the L2 TLB (512/1024 entries -> 2/4 MiB), and steps up again
 *       when it exceeds the L2 TLB and every access needs a full page walk.
 *       The knee positions expose the TLB capacities; the plateau heights
 *       expose the per-level walk cost. This is the RISC-V analogue of the
 *       A53 "Micro Data TLB / TLB L2" behaviour.
 *
 *   (B) sfence.vma cost            - amortized cost of a full flush and a
 *       single-page flush (TLB maintenance overhead).
 *
 *   (C) cold vs hot walk (TWU)     - flush the TLB, then time the first (cold)
 *       traversal of N pages vs a subsequent (hot) traversal. (cold - hot) is
 *       the hardware Translation-Walk-Unit cost. Because SV39 walks 3 levels
 *       and SV48 walks 4, rebuilding the kernel in each mode and comparing
 *       this figure quantifies the page-table-depth penalty.
 */

#include "perf_common.h"
#include <stdlib.h>

#define CHASE1  p = (void **)*p;
#define CHASE4  CHASE1 CHASE1 CHASE1 CHASE1

/* ------------------------------------------------------------------------ */
/* (A) page-stride latency sweep                                             */
/* ------------------------------------------------------------------------ */

/* accesses per window for the hot page-stride sweep = TLBA_INNER * 4 */
#define TLBA_INNER 256

static uint64_t __attribute__((noinline, optimize("O2")))
tlb_hot_window(void **head)
{
    rt_base_t level;
    uint64_t c0, c1;
    register void **p = head;
    register uint32_t i;

    level = rt_hw_interrupt_disable();
    perf_barrier();
    c0 = perf_rd_cycle();
    perf_barrier();
    for (i = 0; i < TLBA_INNER; i++)
    {
        CHASE4
    }
    perf_barrier();
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);

    __asm__ volatile("" ::"r"(p) : "memory");
    return c1 - c0;
}

static uint64_t tlb_hot_measure(void **head)
{
    uint64_t best = (uint64_t)-1;
    int r;
    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t d = tlb_hot_window(head);
        if (d < best)
        {
            best = d;
        }
    }
    return best;
}

/* geometric page-count progression: 4,8,16,...,2048 pages */
static void tlb_page_sweep(void)
{
    perf_calib_t *cal = perf_calibrate();
    void *buf = perf_get_workbuf();
    size_t npages;
    size_t maxpages = PERF_MAX_WORKSET / PERF_PAGE_SIZE;
    char f1[24], f2[24], f3[24];
    const uint64_t loads = (uint64_t)TLBA_INNER * 4;

    rt_kprintf("[perf_tlb] (A) page-stride latency (1 access / 4KiB page)\n");
    rt_kprintf("CSV,test,pages,reach_bytes,cyc_per_access,ns_per_access,ns_per_ghz\n");

    for (npages = 4; npages <= maxpages; npages <<= 1)
    {
        size_t range = npages * PERF_PAGE_SIZE;
        void **head = perf_build_chase(buf, range, PERF_PAGE_SIZE);
        uint64_t d, net;
        double cyc_acc, ns_acc;

        if (head == RT_NULL)
        {
            continue;
        }
        /* warm so translations are resident up to TLB capacity */
        perf_warm_chase(head, npages, 3);

        d = tlb_hot_measure(head);
        net = (d > cal->empty_cycle) ? (d - cal->empty_cycle) : d;
        cyc_acc = (double)net / (double)loads;
        ns_acc = perf_cycles_to_ns((uint64_t)(cyc_acc * 1000.0)) / 1000.0;

        rt_kprintf("CSV,tlb_stride,%lu,%lu,%s,%s,%s\n",
                   (unsigned long)npages, (unsigned long)range,
                   perf_ftoa(cyc_acc, f1, sizeof(f1)),
                   perf_ftoa(ns_acc, f2, sizeof(f2)),
                   perf_ftoa(cyc_acc, f3, sizeof(f3)));
    }
}

/* ------------------------------------------------------------------------ */
/* (B) sfence.vma cost                                                       */
/* ------------------------------------------------------------------------ */

#define SFENCE_ITERS 4096

static uint64_t __attribute__((noinline, optimize("O2")))
sfence_all_window(void)
{
    rt_base_t level;
    uint64_t c0, c1;
    register uint32_t i;

    level = rt_hw_interrupt_disable();
    perf_barrier();
    c0 = perf_rd_cycle();
    for (i = 0; i < SFENCE_ITERS; i++)
    {
        __asm__ volatile("sfence.vma" ::: "memory");
    }
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);
    return c1 - c0;
}

static uint64_t __attribute__((noinline, optimize("O2")))
sfence_page_window(void *va)
{
    rt_base_t level;
    uint64_t c0, c1;
    register uint32_t i;

    level = rt_hw_interrupt_disable();
    perf_barrier();
    c0 = perf_rd_cycle();
    for (i = 0; i < SFENCE_ITERS; i++)
    {
        __asm__ volatile("sfence.vma %0, zero" ::"r"(va) : "memory");
    }
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);
    return c1 - c0;
}

static void tlb_sfence_cost(void)
{
    perf_calib_t *cal = perf_calibrate();
    void *buf = perf_get_workbuf();
    uint64_t a_best = (uint64_t)-1, p_best = (uint64_t)-1;
    int r;
    char f1[24], f2[24];

    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t a = sfence_all_window();
        uint64_t p = sfence_page_window(buf);
        if (a < a_best)
        {
            a_best = a;
        }
        if (p < p_best)
        {
            p_best = p;
        }
    }

    rt_kprintf("[perf_tlb] (B) sfence.vma amortized cost (per instruction)\n");
    rt_kprintf("CSV,test,variant,cyc_per_op,ns_per_op\n");
    {
        double ac = (double)a_best / SFENCE_ITERS;
        double pc = (double)p_best / SFENCE_ITERS;
        rt_kprintf("CSV,sfence,all,%s,%s\n",
                   perf_ftoa(ac, f1, sizeof(f1)),
                   perf_ftoa(perf_cycles_to_ns((uint64_t)(ac * 1000)) / 1000.0,
                             f2, sizeof(f2)));
        rt_kprintf("CSV,sfence,page,%s,%s\n",
                   perf_ftoa(pc, f1, sizeof(f1)),
                   perf_ftoa(perf_cycles_to_ns((uint64_t)(pc * 1000)) / 1000.0,
                             f2, sizeof(f2)));
    }
    (void)cal;
}

/* ------------------------------------------------------------------------ */
/* (C) cold vs hot page walk (TWU depth)                                     */
/* ------------------------------------------------------------------------ */

/* Traverse exactly `npages` dependent page-accesses. If `flush` is set, the
 * whole TLB is invalidated immediately before timing, so every access is a
 * guaranteed miss + full page walk (cold). */
static uint64_t __attribute__((noinline, optimize("O2")))
walk_window(void **head, uint32_t npages, int flush)
{
    rt_base_t level;
    uint64_t c0, c1;
    register void **p = head;
    register uint32_t i;

    level = rt_hw_interrupt_disable();
    if (flush)
    {
        perf_tlb_flush_all();
    }
    perf_barrier();
    c0 = perf_rd_cycle();
    for (i = 0; i < npages; i++)
    {
        p = (void **)*p;
    }
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);

    __asm__ volatile("" ::"r"(p) : "memory");
    return c1 - c0;
}

static void tlb_cold_walk(void)
{
    perf_calib_t *cal = perf_calibrate();
    void *buf = perf_get_workbuf();
    /* Use a page count that comfortably exceeds the L1 D-TLB but is bounded
     * so the interrupts-off window stays short. */
    const uint32_t npages = 64;
    size_t range = (size_t)npages * PERF_PAGE_SIZE;
    void **head = perf_build_chase(buf, range, PERF_PAGE_SIZE);
    uint64_t cold_best = (uint64_t)-1, hot_best = (uint64_t)-1;
    int r;
    char f1[24], f2[24], f3[24];
    int vw = perf_satp_vaddr_width();

    if (head == RT_NULL)
    {
        return;
    }

    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t hot, cold;

        /* hot: warm first, then measure with no flush */
        perf_warm_chase(head, npages, 3);
        hot = walk_window(head, npages, 0);
        if (hot < hot_best)
        {
            hot_best = hot;
        }

        /* cold: flush inside the window so the traversal starts empty */
        cold = walk_window(head, npages, 1);
        if (cold < cold_best)
        {
            cold_best = cold;
        }
    }

    rt_kprintf("[perf_tlb] (C) cold vs hot page walk, SV%d, %lu pages\n",
               vw, (unsigned long)npages);
    rt_kprintf("CSV,test,mode,cyc_per_page,ns_per_page\n");
    {
        double hotc = (double)hot_best / npages;
        double coldc = (double)cold_best / npages;
        double walkc = (coldc > hotc) ? (coldc - hotc) : 0.0;
        rt_kprintf("CSV,walk,hot,%s,%s\n",
                   perf_ftoa(hotc, f1, sizeof(f1)),
                   perf_ftoa(perf_cycles_to_ns((uint64_t)(hotc * 1000)) / 1000.0,
                             f2, sizeof(f2)));
        rt_kprintf("CSV,walk,cold,%s,%s\n",
                   perf_ftoa(coldc, f1, sizeof(f1)),
                   perf_ftoa(perf_cycles_to_ns((uint64_t)(coldc * 1000)) / 1000.0,
                             f2, sizeof(f2)));
        rt_kprintf("CSV,walk,twu_delta,%s,%s\n",
                   perf_ftoa(walkc, f1, sizeof(f1)),
                   perf_ftoa(perf_cycles_to_ns((uint64_t)(walkc * 1000)) / 1000.0,
                             f3, sizeof(f3)));
    }
    (void)cal;
}

long perf_tlb(int argc, char **argv)
{
    void *buf = perf_get_workbuf();

    (void)argc;
    (void)argv;

    if (buf == RT_NULL)
    {
        rt_kprintf("perf_tlb: cannot allocate work buffer\n");
        return -RT_ENOMEM;
    }

    perf_print_banner();
    tlb_page_sweep();
    tlb_sfence_cost();
    tlb_cold_walk();
    rt_kprintf("[perf_tlb] done.\n");
    return 0;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(perf_tlb, C908 TLB and page-walk (TWU) micro-benchmarks);
#endif
