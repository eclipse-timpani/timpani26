// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOT_NONE 0xFFFFFFFF

#define SCHED_TYPE_TT 0
#define SCHED_TYPE_CBS 1
#define SCHED_TYPE_BEST_EFFORT 2

struct PartitionInfo {
    __u64 task_id_hash;
    __u64 cgroup_id;
    __u8  layer;
    __u8  asil_level;
    __u64 cpu_mask;
};

struct TtSlotKey {
    __u32 cpu;
    __u32 slot_idx;
};

struct TtSlotBpf {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 offset_us;
    __u32 duration_us;
    __u32 deadline_us;
    __u32 cpu;
};

struct CbsState {
    __u64 task_id_hash;
    __u32 budget_us;
    __u32 period_us;
    __u32 remaining_us;
    __u64 exec_start_ns;
    __u64 replenish_at_ns;
    __u32 deadline_us;
};

struct TaskMeta {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 assigned_cpu;       /* CPU this TT/CBS task is scheduled on */
    __u32 slot_duration_us;   /* TT slot duration; 0 for non-TT tasks */
    __u8  scheduling_type;
    __u8  layer;
    __u8  tt_runnable;
    __u8  _pad;
    __u64 activation_ns;
    __u64 cgroup_id;
};

#define FAULT_DMISS 0
#define FAULT_BUDGET_EXCEED 1

struct FaultEvent {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 cpu;
    __u64 expected_deadline_ns;
    __u64 actual_completion_ns;
    __u8  fault_type;
};

/*
 * KernelControl — 2-bank handover state, placed in .bss by the BPF program
 * and written by BpfLoader (userspace) via the skeleton bss map.
 *
 *   active_bank_idx   : 0 or 1 — which bank the BPF program reads from.
 *                       BpfLoader writes the shadow bank index here as the
 *                       single atomic handover step (D-02).
 *   tt_handover_gate  : 1 while a handover is in progress. BPF enqueue()
 *                       holds new TT activations on their assigned-CPU
 *                       private queues while the gate is set; CBS and
 *                       best-effort dispatches are unaffected.
 *   bank_active_refs  : per-bank callback reference count. Each banked
 *                       callback acquires and revalidates one reference
 *                       before its lookups and releases it on every return.
 *                       The old bank is cleaned only after its ref count
 *                       reaches 0 after the handover.
 *
 * All fields are volatile: the BPF program reads them, userspace writes
 * them. Not `volatile const` — D-02 requires userspace-writable .bss.
 */
struct KernelControl {
    volatile __u32 active_bank_idx;
    volatile __u32 tt_handover_gate;
    /* 64-bit: the BPF target has no 32-bit atomic patterns, so the
     * callback-wide __sync_fetch_and_add/sub requires a 64-bit operand
     * (clang -target bpf crashes on s32). */
    volatile __u64 bank_active_refs[2];
};

#ifdef __cplusplus
}
#endif
