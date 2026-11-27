// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

// DDR-011 Phase 0 (L-P0-06): one table attempt on the main non-RT loop:
// pre-check, boundary handover, completion checks, cleanup of the old state
// and the ACK decision. TimerMaster, BPF, SHM and FaultMonitor access go
// through AttemptPort so that the sequence can be host-tested.

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "phase0_handover.h"
#include "phase0_prepare.h"

namespace timpani {
namespace node {
namespace phase0 {

// (workload_id_hash, task_id_hash) -> current_limit, as FaultMonitor uses.
using FaultLimits = std::map<std::pair<uint64_t, uint64_t>, uint32_t>;

// The old bank is cleared once its BPF references drain, within this limit.
constexpr uint64_t kOldBankCleanupTimeoutNs = 100000000;
constexpr uint64_t kOldBankCleanupPollNs = 1000000;

class AttemptPort
{
  public:
    virtual ~AttemptPort() = default;
    virtual uint64_t now_ns() = 0;
    virtual void sleep_until(uint64_t ns) = 0;
    virtual int active_bank() = 0;  // -1 if unknown
    // TimerMaster::apply_phase0_table(): applied at T or rejected.
    virtual bool apply(const NPrepared& prepared, ApplyResult& result) = 0;
    virtual bool shm_state(uint32_t& generation, bool& magic_valid) = 0;
    // Clears the inactive (old) bank if no BPF callback references it.
    virtual bool clear_inactive_bank() = 0;
    virtual void update_tracking(const NPrepared& prepared) = 0;
    // Existing FaultMonitor rule for a new table: clear, then set limits.
    virtual void reset_faults(const FaultLimits& limits) = 0;
};

enum class AttemptOutcome {
    kApplied,      // send TableApplied(success=true)
    kRejected,     // send TableApplied(success=false); nothing was changed
    kTrialFailed,  // after the handover started: no ACK, stop the trial
};

struct AttemptResult {
    AttemptOutcome outcome = AttemptOutcome::kRejected;
    std::string reason;
    std::string log;  // one [RTU-P0] line for the non-RT log
};

FaultLimits fault_limits(const NPrepared& prepared);

AttemptResult rejected_attempt(const std::string& node_id,
                               const std::string& table_id,
                               const std::string& reason);

// Updates @baseline only when the table was applied and cleaned up.
AttemptResult run_attempt(const Table& table, const NodeEnvironment& env,
                          ActiveBaseline& baseline, AttemptPort& port);

// Reads magic and generation of /timpani_ttsched.
bool read_shm_state(uint32_t& generation, bool& magic_valid);

}  // namespace phase0
}  // namespace node
}  // namespace timpani
