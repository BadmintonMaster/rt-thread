/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        C908 memory-hierarchy perf suite (TLB/TWU/Cache),
 *                             benchmarked against ARM Cortex-A53 methodology.
 */

/*
 * ============================================================================
 *  C908 (K230) memory-hierarchy micro-benchmark suite - common layer
 * ----------------------------------------------------------------------------
 *  Runs in RT-Smart S-mode as finsh/msh commands. All measurements use the
 *  three RISC-V read-only counters that are directly accessible in S-mode
 *  (no SBI / PMU driver required):
 *
 *      cycle    - CPU clock cycles   (real cost, converted to ns/ns-per-GHz)
 *      instret  - retired instructions (determinism check / "sanity mirror")
 *      time     - wall clock @ TIMER_CLK_FREQ (K230: 27 MHz), for calibration
 *
 *  Design goals (see PERF_TEST_GUIDE.md for the full rationale):
 *    - Control variables: identical machine code across every data point.
 *    - Interference isolation: interrupts masked inside each timing window.
 *    - Robust statistics: repeat N times, report min / median / p99 / max.
 *    - Overhead subtraction: the fixed cost of reading the counters is
 *      measured once at startup and subtracted from every sample.
 * ============================================================================
 */

#ifndef __PERF_COMMON_H__
#define __PERF_COMMON_H__

#include <rtthread.h>
#include <rthw.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Tunables (overridable from Kconfig via rtconfig.h or -D on the compiler)  */
/* ------------------------------------------------------------------------ */

/* K230 mtime (rdtime) time-base frequency, used to derive the CPU frequency.
 * board.h defines TIMER_CLK_FREQ == 27000000 for the same time-base. */
#ifndef PERF_TIMEBASE_HZ
#define PERF_TIMEBASE_HZ        (27000000UL)
#endif

/* Largest working-set the latency/bandwidth sweeps allocate (bytes).
 * Must comfortably fit in the RT-Smart heap (default 32 MB on K230). 8 MB
 * covers every knee of interest: L1 (32K), L2, and 4K-page TLB reach.
 * Driven by Kconfig BSP_PERF_MAX_WORKSET_MB when the suite is enabled. */
#if defined(BSP_PERF_MAX_WORKSET_MB) && !defined(PERF_MAX_WORKSET)
#define PERF_MAX_WORKSET        ((unsigned long)BSP_PERF_MAX_WORKSET_MB * 1024 * 1024)
#endif
#ifndef PERF_MAX_WORKSET
#define PERF_MAX_WORKSET        (8UL * 1024 * 1024)
#endif

/* Repetitions per data point; the reported figure is the minimum (least
 * disturbed) plus median/p99 for distribution insight. Odd number preferred.
 * Driven by Kconfig BSP_PERF_REPEAT when the suite is enabled. */
#if defined(BSP_PERF_REPEAT) && !defined(PERF_REPEAT)
#define PERF_REPEAT             (BSP_PERF_REPEAT)
#endif
#ifndef PERF_REPEAT
#define PERF_REPEAT             (7)
#endif

/* C908 architectural cache-line size (bytes). */
#define PERF_CACHELINE          (64)

/* Base page size (RV Sv39/48/57 all use 4 KiB base pages). */
#define PERF_PAGE_SIZE          (4096)

/* ------------------------------------------------------------------------ */
/* Hardware counters (S-mode readable pseudo-instructions)                   */
/* ------------------------------------------------------------------------ */

rt_inline uint64_t perf_rd_cycle(void)
{
    uint64_t v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

rt_inline uint64_t perf_rd_instret(void)
{
    uint64_t v;
    __asm__ volatile("rdinstret %0" : "=r"(v));
    return v;
}

rt_inline uint64_t perf_rd_time(void)
{
    uint64_t v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

/* Serialize the pipeline so the counter read brackets exactly the code under
 * test. fence + fence.i keep loads/stores and prior instructions ordered
 * around the measurement window. */
rt_inline void perf_barrier(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

/* Full local TLB flush (sfence.vma with no operands invalidates everything). */
rt_inline void perf_tlb_flush_all(void)
{
    __asm__ volatile("sfence.vma" ::: "memory");
}

/* Single-page local TLB flush for a given virtual address. */
rt_inline void perf_tlb_flush_page(void *va)
{
    __asm__ volatile("sfence.vma %0, zero" ::"r"(va) : "memory");
}

/* Read the active virtual-address width from satp.MODE (39/48/57), or 0 for
 * bare mode. Lets the suite report SV39 vs SV48 at runtime. */
rt_inline int perf_satp_vaddr_width(void)
{
    uint64_t satp;
    int mode;
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    mode = (int)(satp >> 60);
    switch (mode)
    {
    case 8:  return 39; /* SV39 */
    case 9:  return 48; /* SV48 */
    case 10: return 57; /* SV57 */
    default: return 0;  /* bare */
    }
}

/* ------------------------------------------------------------------------ */
/* Calibration context (filled once by perf_calibrate())                     */
/* ------------------------------------------------------------------------ */

typedef struct perf_calib
{
    uint64_t cpu_hz;        /* derived CPU frequency (cycle / time)          */
    uint64_t timebase_hz;   /* rdtime frequency (PERF_TIMEBASE_HZ)           */
    uint64_t empty_cycle;   /* fixed cost of an empty timing window (cycles) */
    uint64_t empty_instret; /* instret of an empty timing window             */
    int      valid;
} perf_calib_t;

/* Returns a pointer to the singleton calibration context, running the
 * calibration on first use. Safe to call repeatedly. */
perf_calib_t *perf_calibrate(void);

/* Convert a cycle count to nanoseconds using the calibrated CPU frequency. */
double perf_cycles_to_ns(uint64_t cycles);

/* Convert cycles to the frequency-normalized ns/GHz figure (primary metric
 * for cross-architecture comparison against the A53 baseline). */
double perf_cycles_to_ns_per_ghz(uint64_t cycles);

/* Format a non-negative double with 3 decimals into buf; returns buf. */
const char *perf_ftoa(double v, char *buf, rt_size_t len);

/* ------------------------------------------------------------------------ */
/* Statistics over a set of samples                                          */
/* ------------------------------------------------------------------------ */

typedef struct perf_stat
{
    uint64_t min;
    uint64_t median;
    uint64_t p99;
    uint64_t max;
    uint64_t n;
} perf_stat_t;

/* Sort (ascending, in place) and compute min/median/p99/max. */
void perf_stat_compute(uint64_t *samples, uint64_t n, perf_stat_t *out);

/* ------------------------------------------------------------------------ */
/* Working-set buffer (allocated once, shared by the sweeps)                 */
/* ------------------------------------------------------------------------ */

/* Allocate a page-aligned buffer of PERF_MAX_WORKSET bytes (idempotent).
 * Returns RT_NULL on failure. */
void *perf_get_workbuf(void);
void  perf_put_workbuf(void);

/* Build a randomized pointer-chase ring inside [buf, buf+range), one link
 * every `stride` bytes, so that following the chain touches every line/page
 * exactly once before wrapping. Returns the head pointer. */
void **perf_build_chase(void *buf, size_t range, size_t stride);

/* Warm caches/TLB by walking the chain `laps` times (result discarded). */
void perf_warm_chase(void **head, size_t nlinks, int laps);

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

/* Human-readable size, e.g. 32768 -> "32K", 2097152 -> "2M". Writes into buf. */
const char *perf_human_size(size_t bytes, char *buf, size_t buflen);

/* Print the standard CSV/report banner (env, frequency, A53 reference note). */
void perf_print_banner(void);

/* When non-zero, perf_print_banner() becomes a no-op. perf_all sets this so
 * the environment banner is printed exactly once for a full run. */
extern int perf_banner_suppress;

/* Individual test entry points (each also exported as an msh command). */
long perf_lat(int argc, char **argv);
long perf_bw(int argc, char **argv);
long perf_tlb(int argc, char **argv);
long perf_branch(int argc, char **argv);
long perf_all(int argc, char **argv);

#endif /* __PERF_COMMON_H__ */
