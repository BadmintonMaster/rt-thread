/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-15     Qoder        Branch-misprediction penalty micro-benchmark
 */

/*
 * perf_branch : estimate the branch-misprediction penalty, comparable to the
 *               A53 "Branch misprediction penalty = 7 cycles" figure.
 *
 * Method: run the SAME small loop over an L1-resident byte array twice:
 *   - "predictable" data (all ones)      -> the predictor is always right;
 *   - "random"      data (50/50 ones)    -> the predictor is wrong ~50%.
 * The per-branch cycle difference is caused purely by mispredictions, so
 *   penalty ~= delta_cycles_per_branch / misprediction_rate  (rate ~= 0.5).
 *
 * The loop body is compiled at -O0 so the compiler keeps a real conditional
 * branch (it does not if-convert it into a branchless conditional-move). The
 * fixed -O0 overhead is identical for both data sets, so it cancels in the
 * difference.
 */

#include "perf_common.h"

#define BR_N        8192u   /* branches per window (8 KiB array, L1-resident) */

static uint8_t g_br_arr[BR_N];

static uint64_t g_br_rng = 0x1234567689abcdefULL;
static uint64_t br_rand(void)
{
    uint64_t x = g_br_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_br_rng = x;
    return x;
}

/* O0: keep the `if` as a genuine conditional branch. */
static uint64_t __attribute__((noinline, optimize("O0")))
branch_window(volatile uint8_t *arr, uint32_t n)
{
    rt_base_t level;
    uint64_t c0, c1, s = 0;
    uint32_t i;

    level = rt_hw_interrupt_disable();
    perf_barrier();
    c0 = perf_rd_cycle();
    for (i = 0; i < n; i++)
    {
        if (arr[i])
        {
            s += 3;
        }
        else
        {
            s -= 1;
        }
    }
    c1 = perf_rd_cycle();
    perf_barrier();
    rt_hw_interrupt_enable(level);

    __asm__ volatile("" ::"r"(s) : "memory");
    return c1 - c0;
}

static uint64_t branch_measure(void)
{
    uint64_t best = (uint64_t)-1;
    int r;
    for (r = 0; r < PERF_REPEAT; r++)
    {
        uint64_t d = branch_window(g_br_arr, BR_N);
        if (d < best)
        {
            best = d;
        }
    }
    return best;
}

long perf_branch(int argc, char **argv)
{
    uint64_t pred, rnd;
    double pc, rc, delta, penalty;
    uint32_t i;
    char f1[24], f2[24], f3[24];

    (void)argc;
    (void)argv;

    perf_print_banner();

    /* predictable data set: always taken */
    for (i = 0; i < BR_N; i++)
    {
        g_br_arr[i] = 1;
    }
    pred = branch_measure();

    /* random data set: ~50% taken */
    for (i = 0; i < BR_N; i++)
    {
        g_br_arr[i] = (uint8_t)(br_rand() & 1u);
    }
    rnd = branch_measure();

    pc = (double)pred / BR_N;
    rc = (double)rnd / BR_N;
    delta = (rc > pc) ? (rc - pc) : 0.0;
    penalty = delta * 2.0; /* divide by ~0.5 misprediction rate */

    rt_kprintf("[perf_branch] branch misprediction penalty (A53 ref = 7 cyc)\n");
    rt_kprintf("CSV,test,metric,cycles\n");
    rt_kprintf("CSV,branch,predictable_cyc_per_branch,%s\n",
               perf_ftoa(pc, f1, sizeof(f1)));
    rt_kprintf("CSV,branch,random_cyc_per_branch,%s\n",
               perf_ftoa(rc, f2, sizeof(f2)));
    rt_kprintf("CSV,branch,delta_cyc_per_branch,%s\n",
               perf_ftoa(delta, f3, sizeof(f3)));
    rt_kprintf("CSV,branch,est_mispredict_penalty_cyc,%s\n",
               perf_ftoa(penalty, f1, sizeof(f1)));
    rt_kprintf("[perf_branch] done.\n");
    return 0;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(perf_branch, C908 branch misprediction penalty estimate);
#endif
