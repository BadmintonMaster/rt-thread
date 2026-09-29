/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        C908 memory-hierarchy perf suite - common layer
 */

#include "perf_common.h"
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Float formatting (rt_kprintf has no %f; format via integer arithmetic)    */
/* ------------------------------------------------------------------------ */

/* Format a non-negative double with 3 decimals into buf, return buf. */
const char *perf_ftoa(double v, char *buf, rt_size_t len)
{
    uint64_t scaled, ip, fp;

    if (v < 0)
    {
        v = 0;
    }
    /* round to milli-units */
    scaled = (uint64_t)(v * 1000.0 + 0.5);
    ip = scaled / 1000;
    fp = scaled % 1000;
    rt_snprintf(buf, len, "%llu.%03llu", (unsigned long long)ip,
                (unsigned long long)fp);
    return buf;
}

/* ------------------------------------------------------------------------ */
/* Calibration                                                               */
/* ------------------------------------------------------------------------ */

static perf_calib_t g_calib;

int perf_banner_suppress = 0;

perf_calib_t *perf_calibrate(void)
{
    int i;
    rt_base_t level;
    uint64_t c0, c1, i0, i1;
    uint64_t best_c = (uint64_t)-1, best_i = (uint64_t)-1;
    uint64_t t_start, t_end, c_start, c_end, t_target;

    if (g_calib.valid)
    {
        return &g_calib;
    }

    g_calib.timebase_hz = PERF_TIMEBASE_HZ;

    /*
     * (1) Empty-window overhead: the pure cost of opening and closing a
     *     timing window. This MUST bracket exactly like the real measurement
     *     windows (which read only `cycle`), so the subtracted overhead is
     *     accurate. Minimum over many trials = the undisturbed cost.
     */
    for (i = 0; i < 2048; i++)
    {
        level = rt_hw_interrupt_disable();
        perf_barrier();
        c0 = perf_rd_cycle();
        perf_barrier();
        perf_barrier();
        c1 = perf_rd_cycle();
        perf_barrier();
        rt_hw_interrupt_enable(level);

        if ((c1 - c0) < best_c)
        {
            best_c = c1 - c0;
        }
    }
    g_calib.empty_cycle = best_c;

    /* Informational: the instret cost of an empty rdinstret bracket. */
    for (i = 0; i < 256; i++)
    {
        level = rt_hw_interrupt_disable();
        perf_barrier();
        i0 = perf_rd_instret();
        perf_barrier();
        perf_barrier();
        i1 = perf_rd_instret();
        perf_barrier();
        rt_hw_interrupt_enable(level);

        if ((i1 - i0) < best_i)
        {
            best_i = i1 - i0;
        }
    }
    g_calib.empty_instret = best_i;

    /*
     * (2) CPU frequency: cycle_delta / time_delta * timebase_hz. Interrupts
     *     are left enabled on purpose - both cycle and time advance during
     *     any interrupt, so their ratio is the true average frequency
     *     regardless of interference. Measure over ~20 ms of wall time.
     */
    t_target = PERF_TIMEBASE_HZ / 50; /* ~20 ms */
    perf_barrier();
    t_start = perf_rd_time();
    c_start = perf_rd_cycle();
    do
    {
        t_end = perf_rd_time();
    } while ((t_end - t_start) < t_target);
    c_end = perf_rd_cycle();
    perf_barrier();

    if ((t_end - t_start) != 0)
    {
        g_calib.cpu_hz = (c_end - c_start) * (uint64_t)PERF_TIMEBASE_HZ /
                         (t_end - t_start);
    }
    else
    {
        g_calib.cpu_hz = 0;
    }

    g_calib.valid = 1;
    return &g_calib;
}

double perf_cycles_to_ns(uint64_t cycles)
{
    perf_calib_t *c = perf_calibrate();
    if (c->cpu_hz == 0)
    {
        return 0.0;
    }
    return (double)cycles * 1e9 / (double)c->cpu_hz;
}

double perf_cycles_to_ns_per_ghz(uint64_t cycles)
{
    /* ns/GHz == cycles / (freq_in_GHz) * (1/freq) ... simplifies to:
     * normalized_ns = cycles / 1e9-Hz-equivalent = cycles nanoseconds at 1GHz.
     * i.e. 1 cycle @ 1 GHz == 1 ns. So ns/GHz == number of cycles. We still
     * express it as ns to keep the same unit as the A53 reference table. */
    return (double)cycles;
}

/* ------------------------------------------------------------------------ */
/* Statistics                                                                */
/* ------------------------------------------------------------------------ */

static void u64_sort(uint64_t *a, uint64_t n)
{
    /* insertion sort: n is tiny (== PERF_REPEAT) */
    uint64_t i, j, key;
    for (i = 1; i < n; i++)
    {
        key = a[i];
        j = i;
        while (j > 0 && a[j - 1] > key)
        {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

void perf_stat_compute(uint64_t *samples, uint64_t n, perf_stat_t *out)
{
    uint64_t idx;

    if (n == 0)
    {
        rt_memset(out, 0, sizeof(*out));
        return;
    }
    u64_sort(samples, n);
    out->n = n;
    out->min = samples[0];
    out->max = samples[n - 1];
    out->median = samples[n / 2];
    idx = (n * 99) / 100;
    if (idx >= n)
    {
        idx = n - 1;
    }
    out->p99 = samples[idx];
}

/* ------------------------------------------------------------------------ */
/* Working-set buffer                                                        */
/* ------------------------------------------------------------------------ */

static void *g_workbuf;

void *perf_get_workbuf(void)
{
    if (g_workbuf == RT_NULL)
    {
        /* page-aligned so page-stride TLB walks land on distinct pages */
        g_workbuf = rt_malloc_align(PERF_MAX_WORKSET, PERF_PAGE_SIZE);
        if (g_workbuf)
        {
            rt_memset(g_workbuf, 0, PERF_MAX_WORKSET);
        }
    }
    return g_workbuf;
}

void perf_put_workbuf(void)
{
    if (g_workbuf)
    {
        rt_free_align(g_workbuf);
        g_workbuf = RT_NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Pointer-chase ring builder                                                */
/* ------------------------------------------------------------------------ */

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;

static uint64_t xorshift64(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

void **perf_build_chase(void *buf, size_t range, size_t stride)
{
    size_t nlinks = range / stride;
    size_t i;
    size_t *order;
    char *base = (char *)buf;

    if (nlinks < 2)
    {
        return RT_NULL;
    }

    order = (size_t *)rt_malloc(nlinks * sizeof(size_t));
    if (order == RT_NULL)
    {
        return RT_NULL;
    }

    for (i = 0; i < nlinks; i++)
    {
        order[i] = i;
    }
    /* Fisher-Yates shuffle -> a single Hamiltonian cycle over all links,
     * defeating the hardware stride/next-line prefetcher. */
    for (i = nlinks - 1; i > 0; i--)
    {
        size_t j = (size_t)(xorshift64() % (uint64_t)(i + 1));
        size_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (i = 0; i < nlinks; i++)
    {
        char *cur = base + order[i] * stride;
        char *nxt = base + order[(i + 1) % nlinks] * stride;
        *(void **)cur = (void *)nxt;
    }

    {
        void **head = (void **)(base + order[0] * stride);
        rt_free(order);
        return head;
    }
}

void perf_warm_chase(void **head, size_t nlinks, int laps)
{
    volatile void **p = (volatile void **)head;
    size_t total = nlinks * (size_t)laps;
    size_t k;
    for (k = 0; k < total; k++)
    {
        p = (volatile void **)*p;
    }
    /* keep p live so the loop is not optimized away */
    __asm__ volatile("" ::"r"(p) : "memory");
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

const char *perf_human_size(size_t bytes, char *buf, size_t buflen)
{
    if (bytes >= (1UL << 30) && (bytes % (1UL << 30)) == 0)
    {
        rt_snprintf(buf, buflen, "%luG", (unsigned long)(bytes >> 30));
    }
    else if (bytes >= (1UL << 20) && (bytes % (1UL << 20)) == 0)
    {
        rt_snprintf(buf, buflen, "%luM", (unsigned long)(bytes >> 20));
    }
    else if (bytes >= (1UL << 10) && (bytes % (1UL << 10)) == 0)
    {
        rt_snprintf(buf, buflen, "%luK", (unsigned long)(bytes >> 10));
    }
    else
    {
        rt_snprintf(buf, buflen, "%lu", (unsigned long)bytes);
    }
    return buf;
}

void perf_print_banner(void)
{
    perf_calib_t *c = perf_calibrate();
    char fbuf[24];
    int vw = perf_satp_vaddr_width();

    if (perf_banner_suppress)
    {
        return;
    }

    rt_kprintf("\n");
    rt_kprintf("========================================================\n");
    rt_kprintf(" C908 (K230) memory-hierarchy micro-benchmark suite\n");
    rt_kprintf(" baseline reference: ARM Cortex-A53 (7-cpu.com, 1536MHz)\n");
    rt_kprintf("--------------------------------------------------------\n");
    if (vw)
    {
        rt_kprintf(" SATP mode        : SV%d\n", vw);
    }
    else
    {
        rt_kprintf(" SATP mode        : bare (no paging)\n");
    }
    rt_kprintf(" cache line       : %d B\n", PERF_CACHELINE);
    rt_kprintf(" timebase (rdtime): %llu Hz\n",
               (unsigned long long)c->timebase_hz);
    rt_kprintf(" derived CPU freq : %llu Hz (%s GHz)\n",
               (unsigned long long)c->cpu_hz,
               perf_ftoa((double)c->cpu_hz / 1e9, fbuf, sizeof(fbuf)));
    rt_kprintf(" empty window     : %llu cyc / %llu instret\n",
               (unsigned long long)c->empty_cycle,
               (unsigned long long)c->empty_instret);
    rt_kprintf(" max working set  : %lu bytes\n",
               (unsigned long)PERF_MAX_WORKSET);
    rt_kprintf(" repeats/point    : %d (report min & median)\n", PERF_REPEAT);
    rt_kprintf("========================================================\n");
    rt_kprintf(" note: 'ns/GHz' == cycles (freq-normalized). Compare this\n");
    rt_kprintf("       column against the A53 latency-vs-size table.\n");
    rt_kprintf("========================================================\n\n");
}
