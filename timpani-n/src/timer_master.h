// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bpf_loader.h"
#include "phase0_handover.h"

// SHM layout shared with libttsched (C header — include as C struct)
extern "C" {
#include "../../sample-apps/src/libttsched.h"
}

namespace timpani {
namespace node {

class TimerMaster
{
  public:
    struct SlotEntry {
        uint32_t    cpu;
        uint32_t    slot_idx;
        uint64_t    offset_ns;
        uint64_t    duration_ns;
        uint64_t    task_id_hash;
        uint64_t    workload_id_hash;
        std::string task_name;   /* task comm name — used for targeted wake */
        uint32_t    deadline_us = 10000;
    };

    TimerMaster(BpfLoader& bpf_loader);
    ~TimerMaster();

    void set_schedule_table(const std::vector<SlotEntry>& slots,
                            uint64_t hyperperiod_us, uint64_t epoch_ns);

    void start();
    void stop();

    void remove_workload(uint64_t workload_id_hash);

    // DDR-011 Phase 0: call before start(). start() then runs one resident
    // RT thread that is never restarted, and set_schedule_table() and
    // remove_workload() are refused.
    bool enable_phase0();
    // Non-RT. Stages @prepared in the inactive BPF bank and hands it over at
    // the earliest feasible boundary T = E + kH (L-P0-04). Blocks until it
    // is applied at T or rejected before any active state changed.
    bool apply_phase0_table(const phase0::NPrepared& prepared,
                            phase0::ApplyResult& result);

  private:
    /* Per-CPU timer loop: each CPU gets its own thread running only its slots */
    void cpu_thread_loop(uint32_t cpu,
                         std::vector<SlotEntry> cpu_slots,
                         uint64_t hyperperiod_ns,
                         uint64_t epoch_ns);

    void wake_task(uint64_t task_id_hash, uint64_t deadline_ns);

    /* Compute next TT start time for a given CPU's slot list */
    static uint64_t compute_next_tt_start_for_cpu(
        const std::vector<SlotEntry>& cpu_slots,
        size_t current_slot_idx,
        uint64_t current_hyperperiod_start,
        uint64_t hyperperiod_ns);

    BpfLoader& bpf_loader_;
    std::atomic<bool> running_;
    std::atomic<bool> table_pending_;

    /* Per-CPU threads: one timer thread per isolated CPU */
    std::vector<std::thread> cpu_threads_;

    // POSIX shm for ttsched futex wake (shared with sample_apps libttsched)
    int shm_fd_;
    volatile struct timpani_ttsched_shm* ttsched_shm_;

    // task_id_hash → index in ttsched_shm_->tasks[]
    std::map<uint64_t, int> task_hash_to_shm_idx_;

    std::vector<SlotEntry> slot_table_;
    uint64_t hyperperiod_ns_;
    uint64_t epoch_ns_;
    std::mutex schedule_mutex_;
    std::recursive_mutex lifecycle_mutex_;

    void phase0_loop();

    // DDR-011 Phase 0. phase0_state_ is shared with the resident RT thread;
    // the rest is used by apply_phase0_table() under phase0_mutex_.
    bool phase0_ = false;
    bool phase0_failed_ = false;
    uint64_t phase0_token_ = 0;
    phase0::HandoverState phase0_state_;
    phase0::ShmOwners phase0_owners_;
    std::unique_ptr<phase0::Context> phase0_active_;
    // Published to the RT thread but not adopted (yet).
    std::unique_ptr<phase0::Context> phase0_pending_;
    std::mutex phase0_mutex_;
};

}  // namespace node
}  // namespace timpani
