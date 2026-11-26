// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

// DDR-011 Phase 0 (L-P0-04, L-P0-05): boundary handover between the non-RT
// coordinator and the resident TimerMaster RT thread, and the ttsched SHM
// writer for it. Internal to timpani-n. Kernel, clock and BPF access go
// through the port interfaces below so that the protocol can be host-tested.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "../../sample-apps/src/libttsched.h"
}

namespace timpani {
namespace node {
namespace phase0 {

struct NPrepared;
struct ShmImage;

// The coordinator starts the TT-free steps no later than this before T.
constexpr uint64_t kHandoverMarginNs = 1000000;
// A handover not started within this many boundaries is rejected.
constexpr unsigned kMaxHandoverBoundaries = 8;
constexpr uint64_t kCoordinatorPollNs = 100000;
// Below the margin, so an idle RT thread sees a commit before its boundary.
constexpr uint64_t kRtIdlePollNs = 500000;
constexpr uint64_t kCompletionTimeoutNs = 1000000000;

struct HandoverConfig {
    uint64_t margin_ns = kHandoverMarginNs;
    unsigned max_boundaries = kMaxHandoverBoundaries;
    uint64_t poll_ns = kCoordinatorPollNs;
    uint64_t completion_timeout_ns = kCompletionTimeoutNs;
};

// One TT slot as fired by the RT thread.
struct RtSlot {
    uint64_t offset_ns = 0;
    uint64_t end_ns = 0;  // offset_ns + duration_ns
    uint64_t deadline_ns = 0;
    uint64_t task_id_hash = 0;
    uint32_t cpu = 0;
    uint32_t slot_idx = 0;  // BPF tt_table key of the staged bank
    uint32_t shm_idx = 0;   // ttsched SHM slot of the task
};

// Built off the RT path and immutable once handed to the RT thread.
struct Context {
    uint64_t token = 0;
    uint64_t epoch_ns = 0;
    uint64_t hyperperiod_ns = 0;
    bool has_cpu = false;
    uint32_t cpu = 0;
    uint64_t tt_free_offset_ns = 0;  // G - S: end of the last slot in H
    std::vector<RtSlot> slots;       // ordered like NPrepared::slots
};

bool build_context(const NPrepared& prepared, uint64_t token, Context& out,
                   std::string& error);

// Which task owns each SHM slot in the currently published image.
struct ShmOwners {
    std::array<bool, TIMPANI_MAX_TASKS> occupied{};
    std::array<uint64_t, TIMPANI_MAX_TASKS> task_id_hash{};
};

ShmOwners owners_from(const ShmImage& image);

// Writes names and n_tasks of @image. Only counters of slots that change
// owner are reset; nothing is incremented or woken (L-P0-05 7.1).
void write_shm_image(volatile timpani_ttsched_shm* shm, const ShmImage& image,
                     const ShmOwners& owners);

// T = E + kH at or after @t.
bool boundary_at_or_after(uint64_t epoch_ns, uint64_t hyperperiod_ns,
                          uint64_t t, uint64_t& out);

enum class Phase : uint32_t {
    kIdle,
    kCommitting,  // gate closing/closed for boundary_ns, not ready yet
    kReady,       // candidate prepared; the RT thread applies it at T
    kApplying,    // RT owns the apply transition; timeout cannot cancel it
    kApplied,
    kFailed,  // after the point of no return; never restored
};

// Lock-free state shared by the coordinator and the RT thread.
struct HandoverState {
    std::atomic<Phase> phase{Phase::kIdle};
    std::atomic<uint64_t> boundary_ns{0};
    std::atomic<uint64_t> pending_token{0};
    std::atomic<const Context*> context{nullptr};
    std::atomic<uint32_t> shm_publishing{0};
    // ApplyCompletion: written by the RT thread before kApplied/kFailed.
    std::atomic<uint64_t> done_token{0};
    std::atomic<uint64_t> done_planned_ns{0};
    std::atomic<uint64_t> done_actual_ns{0};
    // Start of the last hyperperiod whose slots have all been fired.
    std::atomic<uint64_t> fired_hyperperiod_ns{0};
};

// RT-thread operations. Implementations must not allocate, lock or log.
class RtPlatform
{
  public:
    virtual ~RtPlatform() = default;
    virtual uint64_t now_ns() = 0;  // CLOCK_REALTIME, the clock of epoch_ns
    // Marks the slot current, sets the next TT start and wakes the task.
    virtual void fire(const RtSlot& slot, uint64_t target_ns,
                      uint64_t next_start_ns) = 0;
    virtual void open_tt_gate() = 0;
};

// The resident RT loop as events: sleep until next_wake_ns(), then call
// on_wake(). Fixed work per event; no allocation, locks or logging.
class RtEngine
{
  public:
    RtEngine(HandoverState& state, RtPlatform& platform,
             volatile timpani_ttsched_shm* shm,
             uint64_t idle_poll_ns = kRtIdlePollNs);

    uint64_t next_wake_ns() const { return next_wake_ns_; }
    void on_wake(uint64_t now);  // requires now >= next_wake_ns()
    const Context* active() const { return ctx_; }
    bool failed() const { return failed_; }

  private:
    void idle_step(uint64_t now);
    bool check_boundary(uint64_t boundary, uint64_t now);
    void apply(uint64_t boundary);
    void record(uint64_t token, uint64_t planned, uint64_t actual);
    void enter_failed(uint64_t now);
    void begin_slots();
    void fire_next();
    void finish_hyperperiod();

    HandoverState& state_;
    RtPlatform& platform_;
    volatile timpani_ttsched_shm* shm_;
    uint64_t idle_poll_ns_;
    const Context* ctx_ = nullptr;
    uint64_t hp_ = 0;  // start S of the current hyperperiod
    size_t next_slot_ = 0;
    bool at_boundary_ = false;
    bool failed_ = false;
    uint64_t next_wake_ns_ = 0;
};

// Non-RT kernel and clock operations used by the coordinator.
class CoordinatorPort
{
  public:
    virtual ~CoordinatorPort() = default;
    virtual uint64_t now_ns() = 0;
    virtual void sleep_until(uint64_t ns) = 0;
    virtual bool stage_bank(const NPrepared& prepared) = 0;  // inactive bank
    virtual void discard_staged_bank() = 0;
    // No TT task running or queued on @cpu and no BPF callback holding a
    // reference on the active bank.
    virtual bool tt_idle(uint32_t cpu) = 0;
    virtual bool set_tt_gate(bool closed) = 0;
    virtual bool select_staged_bank() = 0;  // the gate stays closed
    virtual int active_bank() = 0;
    // current slot = SLOT_NONE, next TT start = @first_start_ns (0: none).
    virtual bool reset_slot_state(uint32_t cpu, uint64_t first_start_ns) = 0;
};

enum class ArmOutcome {
    kReady,     // the RT thread switches at boundary_ns
    kRejected,  // nothing active was changed
    kFailed,    // failed after the gate was closed; not restored
};

struct ArmResult {
    ArmOutcome outcome = ArmOutcome::kRejected;
    uint64_t boundary_ns = 0;
    uint32_t shm_generation = 0;  // stable generation published at T
    std::string error;
};

// Stages @next and performs the TT-free handover steps for the earliest
// feasible boundary. @active is null before the first table. @next must
// stay alive until the handover is applied or has failed.
ArmResult arm_handover(HandoverState& state, CoordinatorPort& port,
                       volatile timpani_ttsched_shm* shm, const Context* active,
                       const Context& next, const NPrepared& prepared,
                       const ShmOwners& owners,
                       const HandoverConfig& config = HandoverConfig());

struct Completion {
    bool applied = false;
    uint64_t token = 0;
    uint64_t planned_ns = 0;
    uint64_t actual_ns = 0;
    std::string error;
};

// Waits until the RT thread applied or failed the armed handover.
Completion await_completion(HandoverState& state, CoordinatorPort& port,
                            uint64_t boundary_ns,
                            const HandoverConfig& config = HandoverConfig());

// Outcome of TimerMaster::apply_phase0_table().
struct ApplyResult {
    bool applied = false;
    bool rejected = false;  // before any active state was changed
    uint64_t token = 0;
    uint64_t planned_boundary_ns = 0;
    uint64_t actual_apply_ns = 0;
    uint32_t shm_generation = 0;
    int active_bank = -1;
    std::string error;
};

}  // namespace phase0
}  // namespace node
}  // namespace timpani
