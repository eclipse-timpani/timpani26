// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

// DDR-011 Phase 0 (L-P0-02): node-side pre-checks and NPrepared assembly.
// Internal to timpani-n; everything here runs on non-RT threads only.

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "node_control.pb.h"

extern "C" {
#include "../../sample-apps/src/libttsched.h"
}

namespace timpani {
namespace node {
namespace phase0 {

using Table = timpani::node::v1::HierarchicalScheduleTable;

// tt_table_map max_entries in bpf/timpani.bpf.c.
constexpr size_t kMaxTtSlots = 2048;
// min(task_meta_map 4096, SHM TIMPANI_MAX_TASKS).
constexpr size_t kMaxTasks = TIMPANI_MAX_TASKS;
constexpr size_t kMaxIdBytes = 15;
// Width of the isolated CPU mask injected into the BPF scheduler.
constexpr uint32_t kMaxCpus = 64;

struct PreparedSlot {
    uint32_t offset_us = 0;
    uint32_t duration_us = 0;
    uint32_t deadline_us = 0;
    uint64_t offset_ns = 0;
    uint64_t duration_ns = 0;
    uint32_t cpu = 0;
    uint32_t slot_idx = 0;  // position in the O snapshot
    uint64_t workload_id_hash = 0;
    uint64_t task_id_hash = 0;
    uint32_t current_limit = 0;
    int32_t layer_index = 0;
};

struct TaskRecord {
    std::string task_id;
    std::string workload_id;
    uint64_t workload_id_hash = 0;
    int32_t pid = -1;
    uint32_t assigned_cpu = 0;
    uint32_t shm_index = 0;
};

struct ShmEntry {
    bool occupied = false;
    uint64_t task_id_hash = 0;
    std::array<char, sizeof(timpani_task_slot::name)> name{};
};

// Names and n_tasks to publish. Counters are deliberately absent so the
// writer never overwrites them.
struct ShmImage {
    std::array<ShmEntry, kMaxTasks> entries{};
    uint32_t n_tasks = 0;
    uint32_t base_generation = 0;
    uint32_t next_generation = 0;
};

// Earliest future boundary T = E + k * H on the fixed grid.
struct Boundary {
    uint64_t epoch_ns = 0;
    uint64_t hyperperiod_ns = 0;
    uint64_t index = 0;
    uint64_t target_ns = 0;
};

// Last table accepted by the Phase 0 path. Empty until the first table.
struct ActiveBaseline {
    bool has_table = false;
    uint64_t epoch_ns = 0;
    uint32_t hyperperiod_us = 0;
    bool has_cpu = false;
    uint32_t cpu = 0;
    std::set<uint32_t> cpus;
    std::vector<PreparedSlot> slots;
    std::map<uint64_t, TaskRecord> tasks;       // task_id_hash -> task
    std::map<uint64_t, std::string> workloads;  // workload_id_hash -> ID
};

struct NPrepared {
    std::string table_id;
    uint64_t epoch_ns = 0;
    uint32_t hyperperiod_us = 0;
    bool has_cpu = false;
    uint32_t cpu = 0;
    std::set<uint32_t> cpus;
    // Sorted by (offset_ns, cpu, workload_id_hash, task_id_hash, slot_idx).
    std::vector<PreparedSlot> slots;
    std::map<uint64_t, TaskRecord> tasks;
    std::map<uint64_t, std::string> workloads;
    std::map<uint64_t, TaskRecord> removed_tasks;
    std::map<uint64_t, std::string> removed_workloads;
    ShmImage shm;
    Boundary boundary;
};

// Returns every live PID whose comm equals each requested name.
using PidResolver = std::function<std::map<std::string, std::vector<int32_t>>(
    const std::set<std::string>& names)>;

struct NodeEnvironment {
    std::string node_id;
    uint64_t isolated_cpu_mask = 0;
    uint64_t now_ns = 0;  // CLOCK_REALTIME, the clock of epoch_ns
    uint32_t shm_generation = 0;
    PidResolver resolve_pids;
};

// Checks node applicability without schedulability analysis. On success the
// candidate is moved into @out; on failure only @error is written.
bool prepare_table(const Table& table, const ActiveBaseline& active,
                   const NodeEnvironment& env, NPrepared& out,
                   std::string& error);

ActiveBaseline baseline_from(const NPrepared& prepared);

bool slot_fits_hyperperiod(uint32_t offset_us, uint32_t duration_us,
                           uint32_t hyperperiod_us);
bool select_boundary(uint64_t epoch_ns, uint64_t hyperperiod_ns,
                     uint64_t now_ns, Boundary& out);
bool next_stable_generation(uint32_t generation, uint32_t& next);
bool parse_cpu_list(const std::string& text, uint64_t& mask);

uint64_t detect_isolated_cpu_mask();
std::map<std::string, std::vector<int32_t>> scan_proc_pids(
    const std::set<std::string>& names);
bool read_shm_generation(uint32_t& generation);
uint64_t realtime_now_ns();

// Hands one full table from the gRPC reader to the main non-RT loop. The
// slot stays busy until finish(); it is not a queue for later requests.
class AttemptSlot
{
  public:
    bool offer(const Table& table);
    bool wait_take(Table& out, std::chrono::milliseconds timeout);
    void finish();
    bool busy() const;

  private:
    mutable std::mutex mutex_;
    std::condition_variable ready_cv_;
    bool busy_ = false;
    bool ready_ = false;
    Table table_;
};

}  // namespace phase0
}  // namespace node
}  // namespace timpani
