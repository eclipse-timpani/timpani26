/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file timpani_rt_shm.h
 * @brief DDR-008 LLD §1.3 unified /timpani_rt SHM layout (CBS/Event/Fault
 *        regions) used by libtimpani_rt to simulate real timpani-n state
 *        instead of logging only. L1 TT still uses the existing, separate
 *        /timpani_ttsched SHM (see libttsched.h) owned by the real TimerMaster.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMPANI_RT_SHM_PATH "/timpani_rt"

#define TIMPANI_RT_MAGIC   0x54525400U /* "TRT\0" */
#define TIMPANI_RT_VERSION 0x0100U     /* 1.0 */

#define TIMPANI_FLAG_TT_ENABLED    (1U << 0)
#define TIMPANI_FLAG_CBS_ENABLED   (1U << 1)
#define TIMPANI_FLAG_EVENT_ENABLED (1U << 2)

#define TIMPANI_MAX_TT_TASKS  32
#define TIMPANI_MAX_CBS_TASKS 32
#define TIMPANI_MAX_EVENTS    64
#define TIMPANI_MAX_FAULTS    64

#define TIMPANI_EVENTS_RINGBUF_MASK (TIMPANI_MAX_EVENTS - 1)
#define TIMPANI_FAULT_RINGBUF_MASK  (TIMPANI_MAX_FAULTS - 1)

/* Reserved for future TT migration into the unified SHM; not written by
 * libtimpani_rt today (TT still goes through /timpani_ttsched). */
struct timpani_tt_slot {
    char name[16];
    uint64_t key;
    uint32_t counter;
    uint32_t _pad;
};

struct timpani_cbs_slot {
    uint64_t cbs_key; /* FNV-1a("workload_id:task_id") */
    uint32_t tid;      /* host TID currently bound, 0 if idle */
    uint32_t state;    /* enum timpani_cbs_state */
};

enum timpani_cbs_state {
    TIMPANI_CBS_IDLE = 0,
    TIMPANI_CBS_ACTIVE = 1,
    TIMPANI_CBS_THROTTLED = 2,
};

struct timpani_event_arrival {
    uint64_t event_key;
    uint64_t arrival_ns;
    uint32_t seq;
    uint32_t _pad;
};

enum timpani_fault_type {
    TIMPANI_FAULT_MIT_VIOLATION = 1,
    TIMPANI_FAULT_E2E_EXCEED = 2,
};

struct timpani_fault_event {
    uint32_t type;
    uint32_t _pad;
    uint64_t event_key;
    uint64_t measured_ns;
    uint64_t limit_ns;
    uint64_t timestamp_ns;
};

struct timpani_rt_shm {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t generation;

    struct {
        uint32_t n_tasks;
        uint32_t _pad;
        struct timpani_tt_slot slots[TIMPANI_MAX_TT_TASKS];
    } tt;

    struct {
        uint32_t n_tasks;
        uint32_t _pad;
        struct timpani_cbs_slot slots[TIMPANI_MAX_CBS_TASKS];
    } cbs;

    struct {
        uint32_t head; /* producer (mark_arrival), atomic RELEASE */
        uint32_t tail; /* consumer, optional */
        struct timpani_event_arrival arrivals[TIMPANI_MAX_EVENTS];
    } events;

    struct {
        uint32_t head; /* producer (libtimpani_rt), atomic RELEASE */
        uint32_t tail; /* consumer (Timpani-N FaultMonitor), atomic ACQUIRE */
        struct timpani_fault_event faults[TIMPANI_MAX_FAULTS];
    } fault_rb;
};

#define TIMPANI_RT_SHM_SIZE sizeof(struct timpani_rt_shm)

#ifdef __cplusplus
}
#endif

