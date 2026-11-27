// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "phase0_attempt.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <set>
#include <sstream>

namespace timpani {
namespace node {
namespace phase0 {

namespace {

const char* outcome_name(AttemptOutcome outcome)
{
    switch (outcome) {
        case AttemptOutcome::kApplied:
            return "applied";
        case AttemptOutcome::kRejected:
            return "rejected";
        case AttemptOutcome::kTrialFailed:
            return "trial_failed";
    }
    return "unknown";
}

std::string join(const std::set<std::string>& ids)
{
    std::string result;
    for (const auto& id : ids) {
        if (!result.empty()) result += ',';
        result += id;
    }
    return result.empty() ? "-" : result;
}

AttemptResult finish(std::ostringstream& log, AttemptOutcome outcome,
                     const std::string& reason)
{
    AttemptResult result;
    result.outcome = outcome;
    result.reason = reason;
    log << " result=" << outcome_name(outcome);
    if (!reason.empty()) log << " reason=" << reason;
    result.log = log.str();
    return result;
}

}  // namespace

FaultLimits fault_limits(const NPrepared& prepared)
{
    // As in the legacy apply, the last positive limit in table order wins.
    std::map<std::pair<uint64_t, uint64_t>, const PreparedSlot*> last;
    for (const auto& slot : prepared.slots) {
        if (slot.current_limit == 0) continue;
        auto [it, inserted] = last.emplace(
            std::make_pair(slot.workload_id_hash, slot.task_id_hash), &slot);
        if (!inserted && it->second->slot_idx < slot.slot_idx) {
            it->second = &slot;
        }
    }
    FaultLimits limits;
    for (const auto& [key, slot] : last) limits[key] = slot->current_limit;
    return limits;
}

AttemptResult rejected_attempt(const std::string& node_id,
                               const std::string& table_id,
                               const std::string& reason)
{
    std::ostringstream log;
    log << "[RTU-P0] node_id=" << node_id << " table_id=" << table_id;
    return finish(log, AttemptOutcome::kRejected, reason);
}

AttemptResult run_attempt(const Table& table, const NodeEnvironment& env,
                          ActiveBaseline& baseline, AttemptPort& port)
{
    std::ostringstream log;
    log << "[RTU-P0] node_id=" << env.node_id
        << " table_id=" << table.table_id();

    NPrepared prepared;
    std::string error;
    if (!prepare_table(table, baseline, env, prepared, error)) {
        return finish(log, AttemptOutcome::kRejected, error);
    }
    std::set<std::string> added;
    std::set<std::string> removed;
    for (const auto& [hash, id] : prepared.workloads) {
        if (baseline.workloads.count(hash) == 0) added.insert(id);
    }
    for (const auto& item : prepared.removed_workloads) {
        removed.insert(item.second);
    }
    log << " added_workloads=" << join(added)
        << " removed_workloads=" << join(removed);

    const int old_bank = port.active_bank();
    if (old_bank < 0) {
        return finish(log, AttemptOutcome::kRejected,
                      "the active BPF bank is unknown");
    }
    ApplyResult applied;
    const bool ok = port.apply(prepared, applied);
    log << " planned_boundary_ns=" << applied.planned_boundary_ns
        << " actual_apply_ns=" << applied.actual_apply_ns;
    if (!ok) {
        return finish(log,
                      applied.rejected ? AttemptOutcome::kRejected
                                       : AttemptOutcome::kTrialFailed,
                      applied.error);
    }
    log << " active_bank=" << applied.active_bank
        << " shm_gen=" << applied.shm_generation;

    // Report success only after the switch itself is confirmed.
    uint32_t generation = 0;
    bool magic_valid = false;
    if (applied.active_bank < 0 || applied.active_bank == old_bank) {
        return finish(log, AttemptOutcome::kTrialFailed,
                      "the active BPF bank did not change");
    }
    if (!port.shm_state(generation, magic_valid) || !magic_valid ||
        (generation & 1u) != 0 || generation != applied.shm_generation) {
        return finish(log, AttemptOutcome::kTrialFailed,
                      "SHM is not the published stable generation");
    }
    if (applied.planned_boundary_ns == 0 ||
        applied.actual_apply_ns < applied.planned_boundary_ns) {
        return finish(log, AttemptOutcome::kTrialFailed,
                      "actual apply time precedes the boundary");
    }

    const uint64_t deadline = port.now_ns() + kOldBankCleanupTimeoutNs;
    bool cleared = port.clear_inactive_bank();
    while (!cleared && port.now_ns() < deadline) {
        port.sleep_until(port.now_ns() + kOldBankCleanupPollNs);
        cleared = port.clear_inactive_bank();
    }
    if (!cleared) {
        return finish(log, AttemptOutcome::kTrialFailed,
                      "the old BPF bank is still referenced");
    }

    port.update_tracking(prepared);
    port.reset_faults(fault_limits(prepared));
    baseline = baseline_from(prepared);
    return finish(log, AttemptOutcome::kApplied, "");
}

bool read_shm_state(uint32_t& generation, bool& magic_valid)
{
    const int fd = shm_open("/timpani_ttsched", O_RDONLY, 0);
    if (fd < 0) return false;
    void* addr = mmap(nullptr, sizeof(struct timpani_ttsched_shm), PROT_READ,
                      MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) return false;
    const auto* shm = static_cast<const struct timpani_ttsched_shm*>(addr);
    magic_valid =
        __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) == TIMPANI_TTSCHED_MAGIC;
    generation = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    munmap(addr, sizeof(struct timpani_ttsched_shm));
    return true;
}

}  // namespace phase0
}  // namespace node
}  // namespace timpani
