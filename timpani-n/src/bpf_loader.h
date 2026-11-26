// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <cstdint>

struct timpani_bpf;

namespace timpani {
namespace node {

namespace phase0 {
struct NPrepared;
} // namespace phase0

// ---------------------------------------------------------------------------
// P0-T03: pure bank-control state machine (host-testable, skel-independent).
//
// These operate on the shared KernelControl (.bss) and a map-write abstraction
// only, so they can be unit-tested on the host without the generated BPF
// skeleton or the protobuf table types. The BpfLoader methods below bind the
// real skeleton maps to these helpers.
// ---------------------------------------------------------------------------

// Clamp a bank index to the valid range [0, 1].
inline int clamp_bank(int idx) { return (idx >= 0 && idx <= 1) ? idx : 0; }

// Apply the handover transition to a KernelControl: raise the TT gate (block
// new TT dispatch), switch the active bank, then lower the gate. This is the
// single logical handover step written to the kernel .bss (D-02).
inline void apply_handover(::KernelControl* kc, int shadow_bank) {
    if (!kc) return;
    kc->tt_handover_gate = 1;
    kc->active_bank_idx = (uint32_t)clamp_bank(shadow_bank);
    kc->tt_handover_gate = 0;
}

// Map-write abstraction for shadow-bank staging. Each write_* returns 0 on
// success and non-zero on failure; `clear` removes all entries from the bank.
// The real BpfLoader binds these to the skeleton maps; tests supply a mock.
struct BankWriter {
    int (*clear)(void* ctx);
    int (*write_tt)(void* ctx, int i);
    int (*write_meta)(void* ctx, int i);
    int (*write_cbs)(void* ctx, int i);
    void* ctx;
};

// Stage a bank: clear it, then write all entries. If any write fails, re-clear
// the bank (leaving no candidate residual) and return false. Only the target
// (shadow) bank is touched; the active bank is never modified here. Returns
// true only if every write succeeded.
inline bool stage_bank(const BankWriter& w, int n_tt, int n_meta, int n_cbs) {
    if (!w.clear || !w.write_tt || !w.write_meta || !w.write_cbs) return false;
    if (w.clear(w.ctx) != 0) return false;
    for (int i = 0; i < n_tt; ++i)
        if (w.write_tt(w.ctx, i) != 0) { w.clear(w.ctx); return false; }
    for (int i = 0; i < n_meta; ++i)
        if (w.write_meta(w.ctx, i) != 0) { w.clear(w.ctx); return false; }
    for (int i = 0; i < n_cbs; ++i)
        if (w.write_cbs(w.ctx, i) != 0) { w.clear(w.ctx); return false; }
    return true;
}

class BpfLoader {
public:
    BpfLoader();
    ~BpfLoader();

    bool load_programs();
    void unload_programs();

    bool update_tt_slot(const TtSlotKey& key, const TtSlotBpf& slot);
    bool update_cbs_state(uint64_t hash, const CbsState& state);
    bool update_task_meta(uint32_t pid, const TaskMeta& meta);
    bool delete_task_meta(uint32_t pid);
    bool delete_tt_slot(const TtSlotKey& key);
    bool delete_cbs_state(uint64_t hash);
    bool update_current_slot(uint32_t cpu, uint32_t slot_idx);
    bool update_next_tt_start(uint32_t cpu, uint64_t next_start_ns);


    int get_fault_ringbuf_fd() const;

    // Shadow Map Swapping for hot updates
    void swap_shadow_maps();
    int get_active_map_idx() const;
    int get_shadow_map_idx() const;
    void apply_table_update();

    // P0-T03: stage a prepared table into the inactive (shadow) bank. Non-RT.
    // Verifies the shadow bank has no active kernel references, clears it, and
    // writes the prepared TT slots / TaskMeta (tt_runnable=0, activation_ns=0)
    // / CBS states. On any failure the shadow bank is re-cleared (no residual)
    // and false is returned; the active bank is never touched. Call
    // apply_table_update() at the boundary to switch the active bank.
    bool stage_table(const phase0::NPrepared& prepared);

    // P0-T04: the shared .bss control area (handover gate, active bank,
    // per-bank references); nullptr before load_programs(). Access the
    // fields with __atomic builtins.
    ::KernelControl* kernel_control();
    // P0-T04: tt_runnable_count_map[cpu]. Non-RT (map lookup syscall).
    bool tt_runnable_count(uint32_t cpu, uint64_t& count) const;
    bool reset_tt_hold_tracking();
    bool tt_hold_tracking_healthy() const;
    bool drain_tt_held();
    // P0-T04: make the staged (shadow) bank active in the kernel and in the
    // userspace mirror without touching tt_handover_gate. Non-RT; the
    // caller keeps the gate closed until the boundary.
    bool select_staged_bank();

private:
    // P0-T03: bank-routed FD accessors (route by the active bank).
    int tt_table_fd() const;
    int cbs_fd() const;
    int task_meta_fd() const;

    // P0-T03: clear all config entries in a bank; clean the old bank only after
    // handover and once its kernel references have drained to zero.
    bool clear_bank(int bank);
    void cleanup_old_bank(int old_bank);
    bool kick_tt_cpu(uint32_t cpu);
    bool tt_held_count(uint32_t cpu, uint64_t& count) const;

    int active_map_idx_;
    uint64_t isolated_cpu_mask_;
    struct timpani_bpf* skel_;
};

} // namespace node
} // namespace timpani
