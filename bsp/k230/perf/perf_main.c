/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        perf_all orchestrator + A53 reference table
 */

/*
 * perf_all : run the full C908 memory-hierarchy suite in one shot and print
 *            the ARM Cortex-A53 reference table so the operator can line the
 *            two up. The environment banner is printed exactly once.
 *
 * Capture the whole run over the serial console, then:
 *     grep '^CSV,' <serial.log> > result.csv
 * and load result.csv into the report template (see PERF_TEST_GUIDE.md).
 */

#include "perf_common.h"

/*
 * ARM Cortex-A53 reference (Amlogic S905, 1536 MHz, 4 KiB pages, 64-bit Linux)
 * Source: https://www.7-cpu.com/cpu/Cortex-A53.html
 * Latency is expressed as "cycles + ns"; we also give the frequency-normalized
 * ns/GHz == cycles-at-1GHz figure to compare directly with our 'ns_per_ghz'.
 */
static void perf_print_a53_reference(void)
{
    rt_kprintf("\n");
    rt_kprintf("======== ARM Cortex-A53 reference (4KiB pages) =========\n");
    rt_kprintf(" L1d = 32K/4-way, L2 = 512K/16-way, line = 64B\n");
    rt_kprintf(" L1 latency = 3 cyc, L2 = ~15 cyc, RAM = 15 cyc + 128 ns\n");
    rt_kprintf(" Micro D-TLB(4K) = 10 entries, L2 TLB = 512 entries/4-way\n");
    rt_kprintf(" branch misprediction penalty = 7 cyc\n");
    rt_kprintf("--------------------------------------------------------\n");
    rt_kprintf(" size      A53 latency (cyc + ns)   note\n");
    rt_kprintf("  32K      3                        L1 hit\n");
    rt_kprintf("  64K      10                       +L2 +uTLB miss\n");
    rt_kprintf(" 128K      14\n");
    rt_kprintf(" 256K      16\n");
    rt_kprintf(" 512K      17 + 4ns\n");
    rt_kprintf("   1M      17 + 70ns                RAM\n");
    rt_kprintf("   2M      17 + 100ns\n");
    rt_kprintf("   4M      25 + 114ns               +L2 TLB miss (22 cyc)\n");
    rt_kprintf("   8M      31 + 121ns\n");
    rt_kprintf("  16M      35 + 124ns\n");
    rt_kprintf(" bandwidth: RAM read ~2000 MB/s, RAM write ~3700 MB/s (8B step)\n");
    rt_kprintf("========================================================\n\n");
}

long perf_all(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    perf_print_banner();

    /* print env/A53 banner once; suppress the per-test banners below */
    perf_banner_suppress = 1;

    perf_lat(1, RT_NULL);
    perf_bw(1, RT_NULL);
    perf_tlb(1, RT_NULL);
    perf_branch(1, RT_NULL);

    perf_banner_suppress = 0;

    perf_print_a53_reference();

    rt_kprintf("[perf_all] complete. Collect with: grep '^CSV,' log > result.csv\n");
    return 0;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(perf_all, run the full C908 memory-hierarchy perf suite);
#endif
