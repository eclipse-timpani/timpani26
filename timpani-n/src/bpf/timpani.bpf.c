// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT
// Dual MIT/GPL is required for BPF programs to load

#include <vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define _LINUX_TYPES_H  // Prevent linux/types.h redefinition clash with
                        // vmlinux.h
#include "maps.h"
#include "../../../sample-apps/src/tt_completion.h"

// SCX DSQ IDs
// TT tasks use SCX_DSQ_LOCAL_ON | cpu directly — no per-CPU TT DSQs.
#define DSQ_CBS 101
#define DSQ_THROTTLED 102
#define DSQ_BE 103
#define DSQ_TT_HELD_BASE 104
#define DSQ_TT_QUARANTINE (DSQ_TT_HELD_BASE + 64)
#define PROMOTE_BUDGET 16
#define TT_ACTIVE_RUNNING 0xffffffffffffffffULL
#define TT_HELD_RETIRED 0xffffffffU

/* Isolated CPU bitmask injected by BpfLoader before attach.
 * Used by select_cpu() and enqueue() to validate assigned_cpu. */
volatile const __u64 isolated_cpu_mask = 0;

/*
 * 2-bank handover control (DESIGN PHASE-4 / D-02).
 * Lives in .bss so BpfLoader can write it via the skeleton bss map.
 * Initialized to bank 0, gate open, no active refs.
 */
struct KernelControl kernel_control = {
    .active_bank_idx = 0,
    .tt_handover_gate = 0,
    .bank_active_refs = {0, 0},
};
volatile __u32 tt_kick_cpu_request = 64;
volatile __u64 tt_hold_failure_count = 0;

// SCX_DSQ_LOCAL and SCX_SLICE_DFL are enums in vmlinux.h
// Removing incorrect macro fallbacks that redefine them to 0

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct PartitionInfo);
} partition_map SEC(".maps");

/*
 * 2-bank configuration maps (D-01). Each map exists as _0 and _1; the
 * active bank is selected at lookup time via kernel_control.active_bank_idx.
 * BpfLoader prepares the shadow bank, then flips active_bank_idx in .bss
 * as the single handover step (D-02).
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 2048);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_0 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 2048);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_1 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct CbsState);
} cbs_map_0 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct CbsState);
} cbs_map_1 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128);  // Max CPUs
    __type(key, __u32);
    __type(value, __u32);
} current_slot_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128);  // Max CPUs
    __type(key, __u32);
    __type(value, __u64);
} tt_active_until_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128);  // Max CPUs
    __type(key, __u32);
    __type(value, __u64);
} next_tt_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128);  // Max CPUs
    __type(key, __u32);
    __type(value, __u64);
} tt_runnable_count_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128);  // Max CPUs
    __type(key, __u32);
    __type(value, __u64);
} tt_held_count_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u32);  // assigned CPU, or TT_HELD_RETIRED
} tt_held_cpu_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);  // pid
    __type(value, struct TaskMeta);
} task_meta_map_0 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);  // pid
    __type(value, struct TaskMeta);
} task_meta_map_1 SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096 * 16);
} fault_ringbuf SEC(".maps");

// SCX helpers (kfuncs) usually predefined if correctly linked, or we can
// declare them as extern
extern s32 scx_bpf_create_dsq(__u64 dsq_id, __s32 node) __ksym;
#ifndef TIMPANI_SCX_USE_NEW_DSQ_API
#error "TIMPANI_SCX_USE_NEW_DSQ_API must be selected by CMake"
#endif
#if TIMPANI_SCX_USE_NEW_DSQ_API
extern void scx_bpf_dsq_insert(struct task_struct* p, __u64 dsq_id, u64 slice,
                               u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move(struct bpf_iter_scx_dsq* it,
                             struct task_struct* p, __u64 dsq_id,
                             __u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move_to_local(__u64 dsq_id) __ksym;
#define scx_bpf_dispatch scx_bpf_dsq_insert
#define scx_bpf_dispatch_from_dsq scx_bpf_dsq_move
#define scx_bpf_consume scx_bpf_dsq_move_to_local
#else
extern void scx_bpf_dispatch(struct task_struct* p, __u64 dsq_id, u64 slice,
                             u64 enq_flags) __ksym;
extern bool scx_bpf_dispatch_from_dsq(struct bpf_iter_scx_dsq* it,
                                      struct task_struct* p, __u64 dsq_id,
                                      __u64 enq_flags) __ksym;
extern bool scx_bpf_consume(__u64 dsq_id) __ksym;
#endif
extern void scx_bpf_kick_cpu(__s32 cpu, u64 flags) __ksym;
extern int bpf_iter_scx_dsq_new(struct bpf_iter_scx_dsq* it, __u64 dsq_id,
                                __u64 flags) __ksym;
extern struct task_struct* bpf_iter_scx_dsq_next(
    struct bpf_iter_scx_dsq* it) __ksym;
extern void bpf_iter_scx_dsq_destroy(struct bpf_iter_scx_dsq* it) __ksym;

static __always_inline __u64 cbs_key_from_meta(const struct TaskMeta* meta)
{
    return meta->workload_id_hash ^ meta->task_id_hash;
}

static __always_inline void cbs_lazy_replenish(struct CbsState* cbs, __u64 now)
{
    if (!cbs || cbs->period_us == 0 || now < cbs->replenish_at_ns)
        return;

    __u64 period_ns = (__u64)cbs->period_us * 1000ULL;
    __u64 elapsed = now - cbs->replenish_at_ns;
    __u64 periods = elapsed / period_ns + 1;

    cbs->remaining_us = cbs->budget_us;
    cbs->replenish_at_ns += periods * period_ns;
}

static __always_inline __u64 tt_runnable_load(__u64* count)
{
    return count ? __sync_fetch_and_add(count, 0) : 0;
}

static __always_inline bool tt_runnable_decrement(__u64* count)
{
    __u64 current;

    if (!count)
        return false;
    current = tt_runnable_load(count);
#pragma unroll
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (current == 0)
            return false;
        if (__sync_val_compare_and_swap(count, current, current - 1) == current)
            return true;
        current = tt_runnable_load(count);
    }
    return false;
}

static __always_inline bool tt_active_on_cpu(__u32 cpu, __u64 now)
{
    __u64* runnable_count = bpf_map_lookup_elem(&tt_runnable_count_map, &cpu);
    __u64* active_until = bpf_map_lookup_elem(&tt_active_until_map, &cpu);

    if (tt_runnable_load(runnable_count) > 0)
        return true;

    return active_until && *active_until > now;
}

static __always_inline void mark_tt_runnable(struct TaskMeta* meta, __u32 cpu)
{
    __u64* runnable_count;
    __u64 active_until = TT_ACTIVE_RUNNING;

    if (!meta->tt_runnable) {
        runnable_count = bpf_map_lookup_elem(&tt_runnable_count_map, &cpu);
        if (runnable_count) {
            __sync_fetch_and_add(runnable_count, 1);
            meta->tt_runnable = 1;
        }
    }

    bpf_map_update_elem(&tt_active_until_map, &cpu, &active_until, BPF_ANY);
}

static __always_inline void mark_tt_not_runnable(struct TaskMeta* meta,
                                                 __u32 cpu)
{
    __u64* runnable_count;
    __u64 inactive = 0;

    if (!meta->tt_runnable)
        return;

    runnable_count = bpf_map_lookup_elem(&tt_runnable_count_map, &cpu);
    if (tt_runnable_decrement(runnable_count))
        meta->tt_runnable = 0;

    if (tt_runnable_load(runnable_count) == 0)
        bpf_map_update_elem(&tt_active_until_map, &cpu, &inactive, BPF_ANY);
}

static __always_inline bool mark_tt_held(__u32 pid, struct TaskMeta* meta,
                                         __u32 cpu)
{
    __u32* held_cpu = bpf_map_lookup_elem(&tt_held_cpu_map, &pid);
    __u64* held_count;
    __u64* runnable_count;
    __u64 active_until = TT_ACTIVE_RUNNING;

    if (held_cpu)
        return *held_cpu != TT_HELD_RETIRED;
    held_count = bpf_map_lookup_elem(&tt_held_count_map, &cpu);
    runnable_count = bpf_map_lookup_elem(&tt_runnable_count_map, &cpu);
    if (!held_count || !runnable_count ||
        bpf_map_update_elem(&tt_held_cpu_map, &pid, &cpu, BPF_ANY) != 0) {
        __sync_fetch_and_add(&tt_hold_failure_count, 1);
        return false;
    }
    __sync_fetch_and_add(held_count, 1);
    if (!meta->tt_runnable)
        __sync_fetch_and_add(runnable_count, 1);
    meta->tt_runnable = 1;
    bpf_map_update_elem(&tt_active_until_map, &cpu, &active_until, BPF_ANY);
    return true;
}

static __always_inline bool release_tt_held(__u32 pid,
                                            struct TaskMeta* meta,
                                            __u32 cpu)
{
    __u32* held_cpu = bpf_map_lookup_elem(&tt_held_cpu_map, &pid);
    __u64 active_until = TT_ACTIVE_RUNNING;

    if (!held_cpu || *held_cpu == TT_HELD_RETIRED)
        return false;
    meta->tt_runnable = 1;
    bpf_map_update_elem(&tt_active_until_map, &cpu, &active_until, BPF_ANY);
    return true;
}

static __always_inline bool resolve_tt_held(__u32 pid)
{
    __u32* held_cpu = bpf_map_lookup_elem(&tt_held_cpu_map, &pid);
    __u64* held_count;

    if (!held_cpu || *held_cpu == TT_HELD_RETIRED)
        return false;
    held_count = bpf_map_lookup_elem(&tt_held_count_map, held_cpu);
    if (!held_count || bpf_map_delete_elem(&tt_held_cpu_map, &pid) != 0)
        return false;
    tt_runnable_decrement(held_count);
    return true;
}

static __always_inline bool retire_tt_held(__u32 pid)
{
    __u32* held_cpu = bpf_map_lookup_elem(&tt_held_cpu_map, &pid);
    __u64* held_count;
    __u64* runnable_count;
    __u32 cpu;
    __u32 retired = TT_HELD_RETIRED;
    __u64 inactive = 0;

    if (!held_cpu || *held_cpu == TT_HELD_RETIRED)
        return true;
    cpu = *held_cpu;
    held_count = bpf_map_lookup_elem(&tt_held_count_map, &cpu);
    runnable_count = bpf_map_lookup_elem(&tt_runnable_count_map, &cpu);
    if (!held_count || !runnable_count ||
        bpf_map_update_elem(&tt_held_cpu_map, &pid, &retired, BPF_ANY) != 0)
        return false;
    tt_runnable_decrement(held_count);
    tt_runnable_decrement(runnable_count);
    if (tt_runnable_load(runnable_count) == 0)
        bpf_map_update_elem(&tt_active_until_map, &cpu, &inactive, BPF_ANY);
    return true;
}

static __always_inline void cleanup_tt_held(__u32 pid)
{
    __u32* held_cpu = bpf_map_lookup_elem(&tt_held_cpu_map, &pid);

    if (!held_cpu)
        return;
    if (*held_cpu != TT_HELD_RETIRED) {
        __u64* held_count = bpf_map_lookup_elem(&tt_held_count_map, held_cpu);
        __u64* runnable_count =
            bpf_map_lookup_elem(&tt_runnable_count_map, held_cpu);
        __u64 inactive = 0;
        tt_runnable_decrement(held_count);
        tt_runnable_decrement(runnable_count);
        if (tt_runnable_load(runnable_count) == 0)
            bpf_map_update_elem(&tt_active_until_map, held_cpu, &inactive,
                                BPF_ANY);
    }
    bpf_map_delete_elem(&tt_held_cpu_map, &pid);
}

static __always_inline __u32 us_until_next_tt(__u32 cpu, __u64 now)
{
    __u64* next_tt_start = bpf_map_lookup_elem(&next_tt_start_map, &cpu);

    if (!next_tt_start || *next_tt_start == 0)
        return 0xffffffffU;
    if (*next_tt_start <= now)
        return 1;

    __u64 delta_us = (*next_tt_start - now) / 1000ULL;

    if (delta_us == 0)
        return 1;
    if (delta_us > 0xffffffffULL)
        return 0xffffffffU;

    return (__u32)delta_us;
}

static __always_inline __u64 cbs_enqueue_flags(__u64 enq_flags)
{
    return enq_flags & ~(SCX_ENQ_PREEMPT | SCX_ENQ_HEAD);
}

/*
 * 2-bank lookup helpers (D-01 / DESIGN PHASE-4).
 *
 * A callback acquires one bank reference before any lookup, then re-reads the
 * selector. A successful revalidation pins that bank for the callback even
 * if userspace switches the selector immediately afterwards. If a switch
 * races the first acquisition, the mismatched reference is released and the
 * callback retries before touching a banked map.
 */
static __always_inline __u32 current_bank(void)
{
    __u32 bank = kernel_control.active_bank_idx;
    return bank < 2 ? bank : 0;
}

static __always_inline void release_bank_ref(__u32 bank)
{
    if (bank < 2)
        __sync_fetch_and_sub(&kernel_control.bank_active_refs[bank], 1);
}

static __always_inline bool acquire_current_bank(__u32* bank)
{
#pragma unroll
    for (int attempt = 0; attempt < 4; ++attempt) {
        __u32 selected = current_bank();

        __sync_fetch_and_add(&kernel_control.bank_active_refs[selected], 1);
        if (current_bank() == selected) {
            *bank = selected;
            return true;
        }
        release_bank_ref(selected);
    }
    return false;
}

static __always_inline struct TaskMeta* lookup_task_meta(__u32 pid, __u32 bank)
{
    if (bank >= 2)
        return NULL;
    if (bank == 0)
        return bpf_map_lookup_elem(&task_meta_map_0, &pid);
    return bpf_map_lookup_elem(&task_meta_map_1, &pid);
}

static __always_inline struct CbsState* lookup_cbs(__u64 key, __u32 bank)
{
    if (bank >= 2)
        return NULL;
    if (bank == 0)
        return bpf_map_lookup_elem(&cbs_map_0, &key);
    return bpf_map_lookup_elem(&cbs_map_1, &key);
}

static __always_inline struct TtSlotBpf* lookup_tt_slot(struct TtSlotKey* key,
                                                        __u32 bank)
{
    if (bank >= 2)
        return NULL;
    if (bank == 0)
        return bpf_map_lookup_elem(&tt_table_map_0, key);
    return bpf_map_lookup_elem(&tt_table_map_1, key);
}

static __always_inline void release_held_tt_tasks(__u32 bank, __u32 cpu)
{
    struct bpf_iter_scx_dsq it;
    struct task_struct* p;
    int scanned = 0;

    if (cpu >= 64)
        return;
    bpf_iter_scx_dsq_new(&it, DSQ_TT_HELD_BASE + cpu, 0);
    while ((p = bpf_iter_scx_dsq_next(&it))) {
        struct TaskMeta* meta;
        __u32 pid;

        if (scanned++ >= PROMOTE_BUDGET)
            break;
        pid = p->pid;
        meta = lookup_task_meta(pid, bank);
        if (meta && meta->scheduling_type == SCHED_TYPE_TT) {
            __u32 assigned_cpu = meta->assigned_cpu;

            if (assigned_cpu == cpu &&
                (isolated_cpu_mask & (1ULL << cpu)) &&
                release_tt_held(pid, meta, assigned_cpu)) {
                if (scx_bpf_dispatch_from_dsq(
                        &it, p, SCX_DSQ_LOCAL_ON | assigned_cpu,
                        SCX_ENQ_PREEMPT)) {
                    if (resolve_tt_held(pid)) {
                        scx_bpf_kick_cpu(assigned_cpu, SCX_KICK_PREEMPT);
                        continue;
                    }
                }
                __sync_fetch_and_add(&tt_hold_failure_count, 1);
            }
        }
        if (retire_tt_held(pid) &&
            !scx_bpf_dispatch_from_dsq(&it, p, DSQ_TT_QUARANTINE, 0)) {
            __sync_fetch_and_add(&tt_hold_failure_count, 1);
        }
    }
    bpf_iter_scx_dsq_destroy(&it);
}

static __always_inline void promote_throttled_if_replenished(
    __u64 now, __u32 bank)
{
    struct bpf_iter_scx_dsq it;
    struct task_struct* p;
    int scanned = 0;

    bpf_iter_scx_dsq_new(&it, DSQ_THROTTLED, 0);
    while ((p = bpf_iter_scx_dsq_next(&it))) {
        struct TaskMeta* meta;
        struct CbsState* cbs;
        __u32 pid;
        __u64 key;

        if (scanned++ >= PROMOTE_BUDGET)
            break;

        pid = p->pid;
        meta = lookup_task_meta(pid, bank);
        if (!meta || meta->scheduling_type != SCHED_TYPE_CBS)
            continue;

        key = cbs_key_from_meta(meta);
        cbs = lookup_cbs(key, bank);
        if (!cbs)
            continue;

        cbs_lazy_replenish(cbs, now);
        if (cbs->remaining_us > 0) {
            scx_bpf_dispatch_from_dsq(&it, p, DSQ_CBS, 0);
            scx_bpf_kick_cpu(meta->assigned_cpu, SCX_KICK_IDLE);
        }
    }
    bpf_iter_scx_dsq_destroy(&it);
}

SEC("struct_ops.s/init_task")
int BPF_PROG(init_task, struct task_struct* p, struct scx_init_task_args* args)
{
    return 0;
}

SEC("struct_ops/select_cpu")
s32 BPF_PROG(select_cpu, struct task_struct* p, s32 prev_cpu, u64 wake_flags)
{
    __u32 pid = p->pid;
    __u32 bank;
    struct TaskMeta* meta;

    if (!acquire_current_bank(&bank))
        return prev_cpu;
    meta = lookup_task_meta(pid, bank);
    if (meta && (meta->scheduling_type == SCHED_TYPE_TT ||
                 meta->scheduling_type == SCHED_TYPE_CBS)) {
        if (meta->scheduling_type == SCHED_TYPE_CBS)
            meta->activation_ns = bpf_ktime_get_ns();
        /* Steer to the CPU assigned in the schedule table */
        __u32 cpu = meta->assigned_cpu;
        if (cpu < 64 && (isolated_cpu_mask & (1ULL << cpu))) {
            release_bank_ref(bank);
            return (s32)cpu;
        }
    }
    release_bank_ref(bank);
    return prev_cpu;
}

SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct* p, u64 enq_flags)
{
    __u32 pid = p->pid;
    __u32 bank;
    struct TaskMeta* meta;

    if (!acquire_current_bank(&bank)) {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }
    meta = lookup_task_meta(pid, bank);
    if (!meta) {
        release_bank_ref(bank);
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        if (kernel_control.tt_handover_gate) {
            __u32 acpu = meta->assigned_cpu;

            if (acpu < 64 && (isolated_cpu_mask & (1ULL << acpu))) {
                if (!mark_tt_held(pid, meta, acpu))
                    __sync_fetch_and_add(&tt_hold_failure_count, 1);
                scx_bpf_dispatch(p, DSQ_TT_HELD_BASE + acpu, SCX_SLICE_DFL,
                                 enq_flags);
                scx_bpf_kick_cpu(acpu, SCX_KICK_IDLE);
            } else {
                scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL,
                                 enq_flags);
            }
        } else {
            /* Dispatch directly to the assigned CPU's local DSQ, then kick
             * that CPU with preemption so TT always wins over running CBS
             * work. */
            __u32 acpu = meta->assigned_cpu;
            if (acpu < 64 && (isolated_cpu_mask & (1ULL << acpu))) {
                mark_tt_runnable(meta, acpu);
                scx_bpf_dispatch(p, SCX_DSQ_LOCAL_ON | acpu, SCX_SLICE_DFL,
                                 enq_flags | SCX_ENQ_PREEMPT);
                scx_bpf_kick_cpu(acpu, SCX_KICK_PREEMPT);
            } else {
                /* Fallback: global DSQ (should not happen in normal
                 * operation) */
                scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL,
                                 enq_flags);
            }
        }
    } else if (meta->scheduling_type == SCHED_TYPE_CBS) {
        /* N1: CBS budget check — throttle if budget exhausted */
        __u64 key = cbs_key_from_meta(meta);
        __u64 cbs_flags = cbs_enqueue_flags(enq_flags);
        struct CbsState* cbs = lookup_cbs(key, bank);
        if (cbs) {
            __u64 now = bpf_ktime_get_ns();
            cbs_lazy_replenish(cbs, now);
            if (cbs->remaining_us == 0) {
                /* Budget exhausted, throttle to DSQ_THROTTLED */
                scx_bpf_dispatch(p, DSQ_THROTTLED, SCX_SLICE_DFL,
                                 cbs_flags);
            } else {
                scx_bpf_dispatch(p, DSQ_CBS, SCX_SLICE_DFL, cbs_flags);
            }
        } else {
            scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, cbs_flags);
        }
    } else {
        scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
    }
    release_bank_ref(bank);
}

SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct* prev)
{
    /* TT tasks: dispatched via SCX_DSQ_LOCAL_ON in enqueue() — already on
     * this CPU's local DSQ, no additional consume needed here. */
    __u64 now = bpf_ktime_get_ns();
    __u32 bank;

    if (!acquire_current_bank(&bank))
        return;
    if (!kernel_control.tt_handover_gate)
        release_held_tt_tasks(bank, (__u32)cpu);
    if (tt_active_on_cpu((__u32)cpu, now)) {
        release_bank_ref(bank);
        return;
    }
    promote_throttled_if_replenished(now, bank);
    release_bank_ref(bank);

    /* 2nd priority: CBS */
    if (scx_bpf_consume(DSQ_CBS))
        return;
}

SEC("struct_ops/running")
void BPF_PROG(running, struct task_struct* p)
{
    __u32 pid = p->pid;
    __u32 bank;
    struct TaskMeta* meta;

    if (!acquire_current_bank(&bank))
        return;
    meta = lookup_task_meta(pid, bank);
    if (meta) {
        __u64 now = bpf_ktime_get_ns();
        /* N1: Record CBS execution start */
        if (meta->scheduling_type == SCHED_TYPE_CBS) {
            __u64 key = cbs_key_from_meta(meta);
            struct CbsState* cbs = lookup_cbs(key, bank);
            if (cbs) {
                __u32 effective_us;
                __u32 next_tt_us;

                cbs_lazy_replenish(cbs, now);
                effective_us = cbs->remaining_us;
                next_tt_us = us_until_next_tt((__u32)bpf_get_smp_processor_id(),
                                              now);
                if (next_tt_us < effective_us)
                    effective_us = next_tt_us;
                if (effective_us == 0)
                    effective_us = 1;

                cbs->exec_start_ns = now;
                p->scx.slice = (__u64)effective_us * 1000ULL;
            }
        }
    }
    release_bank_ref(bank);
}

SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct* p, bool runnable)
{
    __u32 pid = p->pid;
    __u32 bank;
    struct TaskMeta* meta;

    if (!acquire_current_bank(&bank))
        return;
    meta = lookup_task_meta(pid, bank);
    if (!meta) {
        release_bank_ref(bank);
        return;
    }

    __u64 now = bpf_ktime_get_ns();

    /* N1: CBS budget deduction */
    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        __u64 key = cbs_key_from_meta(meta);
        struct CbsState* cbs = lookup_cbs(key, bank);
        if (cbs && cbs->exec_start_ns > 0) {
            __u64 elapsed_ns = now - cbs->exec_start_ns;
            __u32 elapsed_us = (__u32)(elapsed_ns / 1000ULL);
            if (elapsed_us >= cbs->remaining_us)
                cbs->remaining_us = 0;
            else
                cbs->remaining_us -= elapsed_us;
            cbs->exec_start_ns = 0;

            /* If deadline missed, emit DMISS fault */
	    /* CBS deadline miss: task finished after its relative deadline */
            if  (meta->activation_ns > 0 && cbs->deadline_us > 0) {
                __u64 abs_deadline = meta->activation_ns + cbs->deadline_us * 1000ULL;
                if (now > abs_deadline) {
                    struct FaultEvent* fault;
                    fault = bpf_ringbuf_reserve(&fault_ringbuf, sizeof(*fault), 0);
                    if (fault) {
                        fault->fault_type = FAULT_DMISS;
                        fault->task_id_hash = meta->task_id_hash;
                        fault->workload_id_hash = meta->workload_id_hash;
                        fault->cpu = bpf_get_smp_processor_id();
                        fault->expected_deadline_ns = abs_deadline;
                        fault->actual_completion_ns = now;
                        bpf_ringbuf_submit(fault, 0);
                    }
                    meta->activation_ns = 0;  // Clear to avoid duplicate faults
                }
            }

            /* If budget exhausted, emit BUDGET_EXCEED fault */
            if (cbs->remaining_us == 0) {
                if (cbs->period_us > 0) {
                    __u64 period_ns = (__u64)cbs->period_us * 1000ULL;

                    if (cbs->replenish_at_ns == 0) {
                        cbs->replenish_at_ns = now + period_ns;
                    } else if (now >= cbs->replenish_at_ns) {
                        __u64 elapsed = now - cbs->replenish_at_ns;
                        __u64 periods = elapsed / period_ns + 1;

                        cbs->replenish_at_ns += periods * period_ns;
                    }
                }
                struct FaultEvent* fault;
                fault = bpf_ringbuf_reserve(&fault_ringbuf, sizeof(*fault), 0);
                if (fault) {
                    fault->fault_type = FAULT_BUDGET_EXCEED;
                    fault->task_id_hash = meta->task_id_hash;
                    fault->workload_id_hash = meta->workload_id_hash;
                    fault->cpu = bpf_get_smp_processor_id();
                    fault->expected_deadline_ns = cbs->replenish_at_ns;
                    bpf_ringbuf_submit(fault, 0);
                }
            }
        }
    }

    if (meta->scheduling_type == SCHED_TYPE_TT && !runnable)
        mark_tt_not_runnable(meta, bpf_get_smp_processor_id());
    release_bank_ref(bank);
}

/* fentry instead of a syscall tracepoint, which slows every syscall system-wide */
SEC("fentry/futex_wake")
int BPF_PROG(tt_job_complete, u32* uaddr, unsigned int flags, int nr_wake,
             u32 bitset)
{
    if (bitset != TIMPANI_TT_COMPLETE_BITSET || nr_wake != 1)
        return 0;

    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u32 bank;
    struct TaskMeta* meta;

    if (!acquire_current_bank(&bank))
        return 0;
    meta = lookup_task_meta(tid, bank);
    if (!meta || meta->scheduling_type != SCHED_TYPE_TT) {
        release_bank_ref(bank);
        return 0;
    }

    struct timpani_tt_completion completion;
    __u64 now = bpf_ktime_get_tai_ns();
    if (bpf_probe_read_user(&completion, sizeof(completion), uaddr) ||
        !completion.deadline_ns || now <= completion.deadline_ns) {
        release_bank_ref(bank);
        return 0;
    }

    struct FaultEvent fault = {};
    fault.fault_type = FAULT_DMISS;
    fault.task_id_hash = meta->task_id_hash;
    fault.workload_id_hash = meta->workload_id_hash;
    fault.cpu = bpf_get_smp_processor_id();
    fault.expected_deadline_ns = completion.deadline_ns;
    fault.actual_completion_ns = now;
    bpf_ringbuf_output(&fault_ringbuf, &fault, sizeof(fault), 0);
    release_bank_ref(bank);
    return 0;
}

SEC("syscall")
int BPF_PROG(kick_tt_cpu)
{
    __u32 cpu = tt_kick_cpu_request;

    if (cpu < 64)
        scx_bpf_kick_cpu((__s32)cpu, SCX_KICK_PREEMPT);
    return 0;
}

SEC("struct_ops.s/init")
s32 BPF_PROG(init)
{
    scx_bpf_create_dsq(DSQ_CBS, -1);
    scx_bpf_create_dsq(DSQ_THROTTLED, -1);
    scx_bpf_create_dsq(DSQ_BE, -1);
    #pragma unroll
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (isolated_cpu_mask & (1ULL << cpu))
            scx_bpf_create_dsq(DSQ_TT_HELD_BASE + (__u32)cpu, -1);
    }
    /* Quarantined removals are never dispatched; kernel task exit removes
     * them from this private DSQ and exit_task clears their counters. */
    scx_bpf_create_dsq(DSQ_TT_QUARANTINE, -1);

    return 0;
}

SEC("struct_ops/exit_task")
void BPF_PROG(exit_task, struct task_struct* p,
              struct scx_exit_task_args* args)
{
    cleanup_tt_held(p->pid);
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {.flags = SCX_OPS_SWITCH_PARTIAL,
                                    .init = (void*)init,
                                    .init_task = (void*)init_task,
                                    .exit_task = (void*)exit_task,
                                    .select_cpu = (void*)select_cpu,
                                    .enqueue = (void*)enqueue,
                                    .dispatch = (void*)dispatch,
                                    .running = (void*)running,
                                    .stopping = (void*)stopping,
                                    .name = "timpani"};

char _license[] SEC("license") = "Dual MIT/GPL";
