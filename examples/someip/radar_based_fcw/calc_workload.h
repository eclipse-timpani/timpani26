/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

/* loop_count passed to run_nsqrt_loops() for one nominal period/event. */
#define FCW_CALC_LOOP_COUNT 3

/* Copied from sample-apps/src/sample_apps.c stress_cpu_nsqrt(). */
static inline int stress_cpu_nsqrt(void) {
    const long double precision = 1.0e-12L;
    const int max_iter = 56;

    for (int i = 16300; i < 16384; i++) {
        const long double n = (long double)i;
        long double lo = (n < 1.0L) ? n : 1.0L;
        long double hi = (n < 1.0L) ? 1.0L : n;
        int j = 0;

        while ((j++ < max_iter) && ((hi - lo) > precision)) {
            const long double g = (lo + hi) / 2.0L;
            if ((g * g) > n)
                hi = g;
            else
                lo = g;
        }
        long double rt = (lo + hi) / 2.0L;
        const long double r2 = (long double)rint((double)(rt * rt));

        if (j >= max_iter || (int)r2 != i) {
            perror("nsqrt: Newton-Raphson sqrt computation failed");
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

static inline void run_nsqrt_loops(int loop_count) {
    for (int i = 0; i < loop_count; i++) {
        stress_cpu_nsqrt();
    }
}

/* Burns thread CPU time (not wall time) so throttling by the scheduler is not counted as work. */
static inline void run_nsqrt_for_cpu_ms(uint32_t ms) {
    auto cpu_ns = []() {
        struct timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    };
    const uint64_t start = cpu_ns();
    const uint64_t budget_ns = (uint64_t)ms * 1000000ULL;
    while (cpu_ns() - start < budget_ns) {
        stress_cpu_nsqrt();
    }
}

