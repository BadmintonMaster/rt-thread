/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        Memory latency curve + bandwidth (A53-comparable)
 */

/*
 * perf_lat : lmbench lat_mem_rd-style pointer-chase latency-vs-size curve.
 *            Reveals the L1/L2/DRAM latency plateaus AND the TLB-reach knees,
 *            directly comparable to the ARM Cortex-A53 "Latency vs Size" table.
 *
 * perf_bw  : sequential read / write / copy bandwidth across sizes, comparable
 *            to the A53 "B/W" section (cycles-per-line and MB/s).
 */

#include "perf_common.h"
#include <stdlib.h>

/* One dependent load: the next address comes from the current one, so the
 * pipeline cannot overlap successive loads -> we measure true load-use
 * latency including any TLB miss + cache miss for that working-set size. */
#define CHASE1 p = (void **)*p;
#define CHASE4 CHASE1 CHASE1 CHASE1 CHASE1
#define CHASE16 CHASE4 CHASE4 CHASE4 CHASE4

/* loads per measurement window = LAT_INNER * 16 (kept short so the
 * interrupts-off window stays well under one tick even at RAM latency). */
#define LAT_INNER   256

/* lmbench-style geometric size progression. */
static size_t lat_step(size_t k)
{
    if (k < 1024)
    {
        k = k * 2;
    }
    else if (k < 4 * 1024)
    {
        k += 1024;
    }
    else
    {
        size_t s;
        for (s = 32 * 1024; s <= k; s *= 2)
            ;
        k += s / 16;
    }
    return k;
}

/* Time one window of (LAT_INNER*16) dependent loads with interrupts masked.
 * noinline + O2 so the unrolled chain is emitted verbatim and identically for
 * every size (control-variable requirement). */
static uint64_t __attribute__((noinline, optimize("O2")))
lat_window(void **head)
{
    rt_base_t level;
    uint64_t c0, c1;
    register void **p = head;
    register uint32_t i;

    level = rt_hw_interrupt_disable();
    perf_barrier();
    c0 = perf_rd_cycle();
    perf_barrier();
    for (i = 0; i < LAT_INNER; i++)
    {
        CHASE16
    }
    perf_barrier();
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);

    /* consume p so the chain is not dead-code-eliminated */
    __asm__ volatile("" ::"r"(p) : "memory");
    return c1 - c0;
}

/* Minimum window latency over PERF_REPEAT trials (min = least disturbed). */
static uint64_t lat_measure(void **head)
{
    uint64_t best = (uint64_t)-1;
    int r;
    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t d = lat_window(head);
        if (d < best)
        {
            best = d;
        }
    }
    return best;
}

long perf_lat(int argc, char **argv)
{
    perf_calib_t *cal = perf_calibrate();
    void *buf = perf_get_workbuf();
    size_t stride = PERF_CACHELINE;
    size_t maxset = PERF_MAX_WORKSET;
    size_t range;
    char sz[24], f1[24], f2[24], f3[24];
    const uint64_t loads = (uint64_t)LAT_INNER * 16;

    if (buf == RT_NULL)
    {
        rt_kprintf("perf_lat: cannot allocate %lu-byte work buffer\n",
                   (unsigned long)PERF_MAX_WORKSET);
        return -RT_ENOMEM;
    }

    if (argc >= 2)
    {
        size_t kb = (size_t)atoi(argv[1]);
        if (kb)
        {
            maxset = kb * 1024;
            if (maxset > PERF_MAX_WORKSET)
            {
                maxset = PERF_MAX_WORKSET;
            }
        }
    }
    if (argc >= 3)
    {
        size_t s = (size_t)atoi(argv[2]);
        if (s >= sizeof(void *))
        {
            stride = s;
        }
    }

    perf_print_banner();
    rt_kprintf("[perf_lat] pointer-chase latency, stride=%lu B\n",
               (unsigned long)stride);
    rt_kprintf("CSV,test,size_bytes,size,cyc_per_access,ns_per_access,ns_per_ghz\n");

    for (range = 4096; range <= maxset; range = lat_step(range))
    {
        void **head;
        uint64_t d, net;
        double cyc_acc, ns_acc, nsg;

        if (range < stride * 2)
        {
            continue;
        }

        head = perf_build_chase(buf, range, stride);
        if (head == RT_NULL)
        {
            continue;
        }
        /* warm the whole ring into cache/TLB so we measure steady state */
        perf_warm_chase(head, range / stride, 2);

        d = lat_measure(head);
        net = (d > cal->empty_cycle) ? (d - cal->empty_cycle) : d;

        cyc_acc = (double)net / (double)loads;
        ns_acc = perf_cycles_to_ns((uint64_t)(cyc_acc * 1000.0)) / 1000.0;
        nsg = cyc_acc; /* ns/GHz == cycles/access */

        perf_human_size(range, sz, sizeof(sz));
        rt_kprintf("CSV,lat,%lu,%s,%s,%s,%s\n",
                   (unsigned long)range, sz,
                   perf_ftoa(cyc_acc, f1, sizeof(f1)),
                   perf_ftoa(ns_acc, f2, sizeof(f2)),
                   perf_ftoa(nsg, f3, sizeof(f3)));
    }

    rt_kprintf("[perf_lat] done. Compare 'ns_per_ghz' vs the A53 table.\n");
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Bandwidth                                                                 */
/* ------------------------------------------------------------------------ */

#define BW_UNROLL 16

/* Sequential read bandwidth: sum every 64-bit word in [buf, buf+range).
 * Returns cycles for one full pass. Interrupts are left ENABLED: a full pass
 * over several MiB is a throughput measurement lasting milliseconds, so the
 * tiny per-tick overhead is negligible and min-of-repeats filters outliers.
 * Disabling interrupts here would keep them off for far too long. */
static uint64_t __attribute__((noinline, optimize("O2")))
bw_read_pass(volatile uint64_t *buf, size_t words)
{
    uint64_t c0, c1, acc = 0;
    size_t i;

    perf_barrier();
    c0 = perf_rd_cycle();
    perf_barrier();
    for (i = 0; i + BW_UNROLL <= words; i += BW_UNROLL)
    {
        acc += buf[i + 0];  acc += buf[i + 1];  acc += buf[i + 2];  acc += buf[i + 3];
        acc += buf[i + 4];  acc += buf[i + 5];  acc += buf[i + 6];  acc += buf[i + 7];
        acc += buf[i + 8];  acc += buf[i + 9];  acc += buf[i + 10]; acc += buf[i + 11];
        acc += buf[i + 12]; acc += buf[i + 13]; acc += buf[i + 14]; acc += buf[i + 15];
    }
    perf_barrier();
    c1 = perf_rd_cycle();
    perf_barrier();

    __asm__ volatile("" ::"r"(acc) : "memory");
    return c1 - c0;
}

/* Sequential write bandwidth: store a constant to every 64-bit word.
 * Interrupts left enabled for the same reason as bw_read_pass(). */
static uint64_t __attribute__((noinline, optimize("O2")))
bw_write_pass(volatile uint64_t *buf, size_t words)
{
    uint64_t c0, c1;
    size_t i;

    perf_barrier();
    c0 = perf_rd_cycle();
    perf_barrier();
    for (i = 0; i + BW_UNROLL <= words; i += BW_UNROLL)
    {
        buf[i + 0] = i;  buf[i + 1] = i;  buf[i + 2] = i;  buf[i + 3] = i;
        buf[i + 4] = i;  buf[i + 5] = i;  buf[i + 6] = i;  buf[i + 7] = i;
        buf[i + 8] = i;  buf[i + 9] = i;  buf[i + 10] = i; buf[i + 11] = i;
        buf[i + 12] = i; buf[i + 13] = i; buf[i + 14] = i; buf[i + 15] = i;
    }
    perf_barrier();
    c1 = perf_rd_cycle();
    perf_barrier();
    return c1 - c0;
}

static uint64_t bw_min(uint64_t (*fn)(volatile uint64_t *, size_t),
                       volatile uint64_t *buf, size_t words)
{
    uint64_t best = (uint64_t)-1;
    int r;
    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t d = fn(buf, words);
        if (d < best)
        {
            best = d;
        }
    }
    return best;
}

/* Convert cycles for `bytes` transferred into MB/s using calibrated freq. */
static double bw_mbps(uint64_t cycles, size_t bytes)
{
    double ns = perf_cycles_to_ns(cycles);
    if (ns <= 0.0)
    {
        return 0.0;
    }
    /* bytes / ns = GB/s; * 1000 = MB/s */
    return (double)bytes / ns * 1000.0;
}

long perf_bw(int argc, char **argv)
{
    void *buf = perf_get_workbuf();
    size_t maxset = PERF_MAX_WORKSET;
    size_t range;
    char sz[24], f1[24], f2[24], f3[24];

    (void)argc;
    (void)argv;

    if (buf == RT_NULL)
    {
        rt_kprintf("perf_bw: cannot allocate work buffer\n");
        return -RT_ENOMEM;
    }

    perf_print_banner();
    rt_kprintf("[perf_bw] sequential read/write bandwidth (64B line)\n");
    rt_kprintf("CSV,test,size_bytes,size,rd_cyc_per_line,rd_MBps,wr_MBps\n");

    for (range = 4096; range <= maxset; range = lat_step(range))
    {
        volatile uint64_t *p = (volatile uint64_t *)buf;
        size_t words = range / sizeof(uint64_t);
        uint64_t rd, wr;
        double rd_line, rd_bw, wr_bw;
        size_t lines = range / PERF_CACHELINE;

        /* warm */
        (void)bw_read_pass(p, words);

        rd = bw_min(bw_read_pass, p, words);
        wr = bw_min(bw_write_pass, p, words);

        rd_line = lines ? (double)rd / (double)lines : 0.0;
        rd_bw = bw_mbps(rd, range);
        wr_bw = bw_mbps(wr, range);

        perf_human_size(range, sz, sizeof(sz));
        rt_kprintf("CSV,bw,%lu,%s,%s,%s,%s\n",
                   (unsigned long)range, sz,
                   perf_ftoa(rd_line, f1, sizeof(f1)),
                   perf_ftoa(rd_bw, f2, sizeof(f2)),
                   perf_ftoa(wr_bw, f3, sizeof(f3)));
    }

    rt_kprintf("[perf_bw] done.\n");
    return 0;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(perf_lat, C908 memory latency-vs-size curve [maxKB] [strideB]);
MSH_CMD_EXPORT(perf_bw, C908 sequential read/write bandwidth sweep);
#endif
