// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "phase0_handover.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "../../sample-apps/src/ttsched_shm.h"
#include "phase0_prepare.h"

namespace timpani {
namespace node {
namespace phase0 {

static_assert(std::atomic<Phase>::is_always_lock_free &&
                  std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<const Context*>::is_always_lock_free,
              "the RT thread needs lock-free handover state");

namespace {

ArmResult rejected(std::string error)
{
    ArmResult result;
    result.outcome = ArmOutcome::kRejected;
    result.error = std::move(error);
    return result;
}

ArmResult failed(std::string error, uint64_t boundary)
{
    ArmResult result;
    result.outcome = ArmOutcome::kFailed;
    result.boundary_ns = boundary;
    result.error = std::move(error);
    return result;
}

}  // namespace

bool build_context(const NPrepared& prepared, uint64_t token, Context& out,
                   std::string& error)
{
    Context ctx;
    ctx.token = token;
    ctx.epoch_ns = prepared.epoch_ns;
    ctx.hyperperiod_ns =
        static_cast<uint64_t>(prepared.hyperperiod_us) * 1000ULL;
    ctx.has_cpu = prepared.has_cpu;
    ctx.cpu = prepared.cpu;
    if (ctx.hyperperiod_ns == 0) {
        error = "hyperperiod must be positive";
        return false;
    }

    ctx.slots.reserve(prepared.slots.size());
    for (const auto& slot : prepared.slots) {
        const auto task = prepared.tasks.find(slot.task_id_hash);
        if (task == prepared.tasks.end() ||
            task->second.shm_index >= TIMPANI_MAX_TASKS) {
            error = "TT slot without a prepared task";
            return false;
        }
        RtSlot rt;
        rt.offset_ns = slot.offset_ns;
        rt.end_ns = slot.offset_ns + slot.duration_ns;
        rt.deadline_ns = static_cast<uint64_t>(slot.deadline_us) * 1000ULL;
        rt.task_id_hash = slot.task_id_hash;
        rt.cpu = slot.cpu;
        rt.slot_idx = slot.slot_idx;
        rt.shm_idx = task->second.shm_index;
        if (rt.end_ns > ctx.hyperperiod_ns) {
            error = "TT slot ends after the hyperperiod";
            return false;
        }
        if (!ctx.slots.empty() && rt.offset_ns < ctx.slots.back().offset_ns) {
            error = "TT slots are not ordered by offset";
            return false;
        }
        ctx.tt_free_offset_ns = std::max(ctx.tt_free_offset_ns, rt.end_ns);
        ctx.slots.push_back(rt);
    }
    out = std::move(ctx);
    return true;
}

ShmOwners owners_from(const ShmImage& image)
{
    ShmOwners owners;
    for (size_t i = 0; i < owners.occupied.size(); ++i) {
        owners.occupied[i] = image.entries[i].occupied;
        owners.task_id_hash[i] =
            image.entries[i].occupied ? image.entries[i].task_id_hash : 0;
    }
    return owners;
}

void write_shm_image(volatile timpani_ttsched_shm* shm, const ShmImage& image,
                     const ShmOwners& owners)
{
    for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; ++i) {
        const ShmEntry& entry = image.entries[i];
        if (!entry.occupied) {
            ttsched_shm_store_name(shm, i, "");
            continue;
        }
        ttsched_shm_store_name(shm, i, entry.name.data());
        if (!owners.occupied[i] ||
            owners.task_id_hash[i] != entry.task_id_hash) {
            ttsched_shm_store_counter(shm, i, 0);
        }
    }
    ttsched_shm_store_n_tasks(shm, image.n_tasks);
}

bool boundary_at_or_after(uint64_t epoch_ns, uint64_t hyperperiod_ns,
                          uint64_t t, uint64_t& out)
{
    if (hyperperiod_ns == 0) return false;
    if (t <= epoch_ns) {
        out = epoch_ns;
        return true;
    }
    const uint64_t elapsed = t - epoch_ns;
    const uint64_t k =
        elapsed / hyperperiod_ns + (elapsed % hyperperiod_ns != 0 ? 1 : 0);
    if (k > (std::numeric_limits<uint64_t>::max() - epoch_ns) / hyperperiod_ns)
        return false;
    out = epoch_ns + k * hyperperiod_ns;
    return true;
}

// ---------------------------------------------------------------------------
// RT engine
// ---------------------------------------------------------------------------

RtEngine::RtEngine(HandoverState& state, RtPlatform& platform,
                   volatile timpani_ttsched_shm* shm, uint64_t idle_poll_ns)
    : state_(state), platform_(platform), shm_(shm), idle_poll_ns_(idle_poll_ns)
{
}

void RtEngine::on_wake(uint64_t now)
{
    if (failed_) {
        next_wake_ns_ = now + idle_poll_ns_;
        return;
    }
    if (!ctx_) {
        idle_step(now);
        return;
    }
    if (!at_boundary_) {
        fire_next();
        return;
    }
    at_boundary_ = false;
    if (check_boundary(hp_, now)) begin_slots();
}

// No grid yet: poll for a committed first table and wake at its boundary.
void RtEngine::idle_step(uint64_t now)
{
    const Phase phase = state_.phase.load(std::memory_order_acquire);
    if (phase == Phase::kFailed) {
        enter_failed(now);
        return;
    }
    if (phase == Phase::kCommitting || phase == Phase::kReady) {
        const uint64_t boundary =
            state_.boundary_ns.load(std::memory_order_relaxed);
        if (boundary > now) {
            next_wake_ns_ = boundary;
            return;
        }
        if (!check_boundary(boundary, now)) return;
        if (ctx_) {
            begin_slots();
            return;
        }
    }
    next_wake_ns_ = now + idle_poll_ns_;
}

// Returns false if the trial failed. Bounded: the coordinator changes the
// phase at most once more for a committed boundary.
bool RtEngine::check_boundary(uint64_t boundary, uint64_t now)
{
    Phase phase = state_.phase.load(std::memory_order_acquire);
    for (;;) {
        if (phase == Phase::kFailed) {
            enter_failed(now);
            return false;
        }
        if (phase != Phase::kCommitting && phase != Phase::kReady) return true;
        const uint64_t planned =
            state_.boundary_ns.load(std::memory_order_relaxed);
        if (planned > boundary) return true;
        if (phase == Phase::kReady && planned == boundary &&
            state_.context.load(std::memory_order_relaxed) != nullptr) {
            apply(boundary);
            return true;
        }

        // Committed but not ready at its boundary, or the boundary was
        // missed: the gate is closed and the SHM may be invalid.
        record(state_.pending_token.load(std::memory_order_relaxed), planned,
               now);
        if (phase == Phase::kReady) {
            state_.phase.store(Phase::kFailed, std::memory_order_release);
            enter_failed(now);
            return false;
        }
        if (state_.phase.compare_exchange_strong(phase, Phase::kFailed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            enter_failed(now);
            return false;
        }
        // The coordinator moved first (to kReady or back to kIdle).
    }
}

void RtEngine::apply(uint64_t boundary)
{
    const Context* next = state_.context.load(std::memory_order_relaxed);
    if (shm_) {
        ttsched_shm_end_publish(
            shm_, state_.shm_publishing.load(std::memory_order_relaxed));
    }
    platform_.open_tt_gate();
    record(next->token, boundary, platform_.now_ns());
    state_.phase.store(Phase::kApplied, std::memory_order_release);
    ctx_ = next;
    hp_ = boundary;
}

void RtEngine::record(uint64_t token, uint64_t planned, uint64_t actual)
{
    state_.done_token.store(token, std::memory_order_relaxed);
    state_.done_planned_ns.store(planned, std::memory_order_relaxed);
    state_.done_actual_ns.store(actual, std::memory_order_relaxed);
}

// The table state is inconsistent after a failed handover: stop firing.
void RtEngine::enter_failed(uint64_t now)
{
    failed_ = true;
    ctx_ = nullptr;
    next_wake_ns_ = now + idle_poll_ns_;
}

void RtEngine::begin_slots()
{
    next_slot_ = 0;
    if (ctx_->slots.empty()) {
        finish_hyperperiod();
        return;
    }
    next_wake_ns_ = hp_ + ctx_->slots[0].offset_ns;
}

void RtEngine::fire_next()
{
    const std::vector<RtSlot>& slots = ctx_->slots;
    const RtSlot& slot = slots[next_slot_];
    const uint64_t next_start =
        next_slot_ + 1 < slots.size()
            ? hp_ + slots[next_slot_ + 1].offset_ns
            : hp_ + ctx_->hyperperiod_ns + slots[0].offset_ns;
    platform_.fire(slot, hp_ + slot.offset_ns, next_start);
    if (++next_slot_ < slots.size()) {
        next_wake_ns_ = hp_ + slots[next_slot_].offset_ns;
        return;
    }
    finish_hyperperiod();
}

void RtEngine::finish_hyperperiod()
{
    state_.fired_hyperperiod_ns.store(hp_, std::memory_order_release);
    hp_ += ctx_->hyperperiod_ns;
    at_boundary_ = true;
    next_wake_ns_ = hp_;
}

// ---------------------------------------------------------------------------
// Coordinator (non-RT)
// ---------------------------------------------------------------------------

ArmResult arm_handover(HandoverState& state, CoordinatorPort& port,
                       volatile timpani_ttsched_shm* shm, const Context* active,
                       const Context& next, const NPrepared& prepared,
                       const ShmOwners& owners, const HandoverConfig& config)
{
    if (state.phase.load(std::memory_order_acquire) != Phase::kIdle) {
        return rejected("a handover is already in progress");
    }
    if (!shm) return rejected("ttsched SHM unavailable");
    const uint64_t epoch = next.epoch_ns;
    const uint64_t hp = next.hyperperiod_ns;
    if (hp == 0) return rejected("hyperperiod must be positive");
    if (active) {
        if (active->epoch_ns != epoch || active->hyperperiod_ns != hp) {
            return rejected(
                "epoch or hyperperiod differs from the active table");
        }
        if (hp - active->tt_free_offset_ns < config.margin_ns) {
            return rejected(
                "TT-free window is shorter than the handover margin");
        }
    }
    const uint32_t generation =
        __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    if ((generation & 1u) != 0 || generation != prepared.shm.base_generation) {
        return rejected("SHM generation changed after preparation");
    }
    if (!port.stage_bank(prepared)) {
        return rejected("staging the inactive BPF bank failed");
    }
    auto reject_staged = [&port](std::string error) {
        port.discard_staged_bank();
        return rejected(std::move(error));
    };

    const bool has_cpu = next.has_cpu || (active && active->has_cpu);
    const uint32_t cpu = next.has_cpu ? next.cpu : (active ? active->cpu : 0);
    auto tt_idle = [&]() { return !has_cpu || port.tt_idle(cpu); };

    for (unsigned attempt = 0; attempt < config.max_boundaries; ++attempt) {
        uint64_t now = port.now_ns();
        uint64_t boundary = 0;
        if (active) {
            if (now < epoch) return reject_staged("clock is before the epoch");
            const uint64_t start = epoch + (now - epoch) / hp * hp;
            boundary = start + hp;
            const uint64_t tt_free = start + active->tt_free_offset_ns;  // G
            if (now < tt_free) {
                port.sleep_until(tt_free);
                now = port.now_ns();
            }
            // The RT thread reports when the last old slot has been fired.
            while (state.fired_hyperperiod_ns.load(std::memory_order_acquire) <
                       start &&
                   now + config.margin_ns <= boundary) {
                port.sleep_until(now + config.poll_ns);
                now = port.now_ns();
            }
            if (state.fired_hyperperiod_ns.load(std::memory_order_acquire) <
                start) {
                port.sleep_until(boundary);
                continue;
            }
        } else if (!boundary_at_or_after(epoch, hp, now + config.margin_ns,
                                         boundary)) {
            return reject_staged("boundary exceeds the time range");
        }

        // No TT execution and no BPF reference before closing the gate.
        bool idle = tt_idle();
        while (!idle && now + config.margin_ns + config.poll_ns <= boundary) {
            port.sleep_until(now + config.poll_ns);
            now = port.now_ns();
            idle = tt_idle();
        }
        if (!idle || port.now_ns() + config.margin_ns > boundary) {
            port.sleep_until(boundary);
            continue;
        }

        // Commit: from here the RT thread treats a missing kReady at the
        // boundary as a failure.
        state.boundary_ns.store(boundary, std::memory_order_relaxed);
        state.pending_token.store(next.token, std::memory_order_relaxed);
        state.context.store(&next, std::memory_order_relaxed);
        state.phase.store(Phase::kCommitting, std::memory_order_release);
        // Re-checked after the store so that the RT thread, which reaches the
        // boundary later, is guaranteed to see the commit.
        bool roll_back = port.now_ns() + config.margin_ns > boundary;
        if (!roll_back) {
            port.set_tt_gate(true);
            // A TT activation that raced with the gate is still visible here.
            roll_back = !tt_idle();
            if (roll_back) port.set_tt_gate(false);
        }
        if (roll_back) {
            Phase expected = Phase::kCommitting;
            if (!state.phase.compare_exchange_strong(
                    expected, Phase::kIdle, std::memory_order_acq_rel)) {
                return failed("boundary reached while committing", boundary);
            }
            state.context.store(nullptr, std::memory_order_relaxed);
            port.sleep_until(boundary);
            continue;
        }

        // [G, T): invalidate the SHM, write the image, select the new bank.
        const uint32_t publishing = ttsched_shm_begin_publish(shm);
        write_shm_image(shm, prepared.shm, owners);
        state.shm_publishing.store(publishing, std::memory_order_relaxed);
        std::string error;
        if (!port.select_staged_bank()) {
            error = "selecting the staged BPF bank failed";
        } else if (has_cpu &&
                   !port.reset_slot_state(
                       cpu, next.slots.empty()
                                ? 0
                                : boundary + next.slots.front().offset_ns)) {
            error = "resetting the BPF slot state failed";
        }
        Phase expected = Phase::kCommitting;
        if (!error.empty()) {
            state.phase.compare_exchange_strong(expected, Phase::kFailed,
                                                std::memory_order_acq_rel);
            return failed(error, boundary);
        }
        if (!state.phase.compare_exchange_strong(expected, Phase::kReady,
                                                 std::memory_order_acq_rel)) {
            return failed("the boundary passed before the handover was ready",
                          boundary);
        }
        ArmResult ready;
        ready.outcome = ArmOutcome::kReady;
        ready.boundary_ns = boundary;
        ready.shm_generation = publishing + 1u;
        return ready;
    }
    return reject_staged("no quiescent TT-free window within " +
                         std::to_string(config.max_boundaries) + " boundaries");
}

Completion await_completion(HandoverState& state, CoordinatorPort& port,
                            uint64_t boundary_ns, const HandoverConfig& config)
{
    Completion done;
    port.sleep_until(boundary_ns);
    const uint64_t deadline = boundary_ns + config.completion_timeout_ns;
    for (;;) {
        const Phase phase = state.phase.load(std::memory_order_acquire);
        if (phase == Phase::kApplied || phase == Phase::kFailed) {
            done.applied = phase == Phase::kApplied;
            done.token = state.done_token.load(std::memory_order_relaxed);
            done.planned_ns =
                state.done_planned_ns.load(std::memory_order_relaxed);
            done.actual_ns =
                state.done_actual_ns.load(std::memory_order_relaxed);
            if (!done.applied) {
                done.error =
                    "the boundary passed before the handover was ready";
            }
            return done;
        }
        const uint64_t now = port.now_ns();
        if (now >= deadline) {
            done.error = "no completion from the RT thread";
            return done;
        }
        port.sleep_until(std::min(now + config.poll_ns, deadline));
    }
}

}  // namespace phase0
}  // namespace node
}  // namespace timpani
