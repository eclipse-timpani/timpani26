// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// DDR-011 Phase 0 P0-T04 and P0-T05 (writer): boundary handover between the
// non-RT coordinator and the resident RT engine, and the ttsched SHM image
// written during the TT-free window.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../sample-apps/src/ttsched_shm.h"
#include "phase0_handover.h"
#include "phase0_prepare.h"

namespace p0 = timpani::node::phase0;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kEpoch = 1000 * kMs;
constexpr uint32_t kHyperperiodUs = 20000;
constexpr uint64_t kH = kHyperperiodUs * 1000ULL;
constexpr uint32_t kCpu = 2;

uint64_t hash_of(const std::string& text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct SlotSpec {
    std::string task;
    uint32_t offset_us;
    uint32_t duration_us;
    uint32_t shm_idx;
};

// An NPrepared as P0-T02 builds it; slots must be given in offset order.
p0::NPrepared make_prepared(const std::vector<SlotSpec>& specs,
                            uint32_t base_generation,
                            uint64_t epoch_ns = kEpoch,
                            uint32_t hyperperiod_us = kHyperperiodUs)
{
    p0::NPrepared prepared;
    prepared.table_id = "t";
    prepared.epoch_ns = epoch_ns;
    prepared.hyperperiod_us = hyperperiod_us;
    prepared.has_cpu = true;
    prepared.cpu = kCpu;
    uint32_t slot_idx = 0;
    for (const auto& spec : specs) {
        p0::PreparedSlot slot;
        slot.offset_us = spec.offset_us;
        slot.duration_us = spec.duration_us;
        slot.deadline_us = spec.duration_us;
        slot.offset_ns = spec.offset_us * 1000ULL;
        slot.duration_ns = spec.duration_us * 1000ULL;
        slot.cpu = kCpu;
        slot.slot_idx = slot_idx++;
        slot.task_id_hash = hash_of(spec.task);
        slot.workload_id_hash = hash_of("w-" + spec.task);
        prepared.slots.push_back(slot);

        p0::TaskRecord& task = prepared.tasks[slot.task_id_hash];
        task.task_id = spec.task;
        task.workload_id = "w-" + spec.task;
        task.workload_id_hash = slot.workload_id_hash;
        task.pid = static_cast<int32_t>(100 + spec.shm_idx);
        task.shm_index = spec.shm_idx;

        p0::ShmEntry& entry = prepared.shm.entries[spec.shm_idx];
        entry.occupied = true;
        entry.task_id_hash = slot.task_id_hash;
        std::strncpy(entry.name.data(), spec.task.c_str(),
                     entry.name.size() - 1);
        prepared.shm.n_tasks = std::max(prepared.shm.n_tasks, spec.shm_idx + 1);
    }
    prepared.shm.base_generation = base_generation;
    prepared.shm.next_generation = base_generation + 2;
    return prepared;
}

// The DDR-011 Phase 0 plan fixture (H = 20 ms). A = {W1, W2},
// B = {W1, W3}: W2 removed, W3 added on a fresh SHM index.
const std::vector<SlotSpec> kTableA = {{"w1_fast", 0, 1000, 0},
                                       {"w1_anchor", 1000, 8000, 1},
                                       {"w2_task", 9000, 1000, 2},
                                       {"w1_fast", 10000, 1000, 0}};
const std::vector<SlotSpec> kTableB = {{"w1_fast", 0, 1000, 0},
                                       {"w1_anchor", 1000, 8000, 1},
                                       {"w1_fast", 10000, 1000, 0},
                                       {"w3_task", 11000, 2000, 3}};

struct Fire {
    uint64_t time;
    uint64_t token;
    uint64_t task;
    uint32_t shm_idx;
    uint64_t target;
};

// Discrete-event world: the coordinator's sleeps run the RT events due.
class World final : public p0::RtPlatform, public p0::CoordinatorPort
{
  public:
    World() : engine(state, *this, &shm)
    {
        std::memset(&shm, 0, sizeof(shm));
        ttsched_shm_reset(&shm);
    }

    void run_until(uint64_t t)
    {
        while (engine.next_wake_ns() <= t) {
            uint64_t wake = std::max(now, engine.next_wake_ns());
            if (wake < rt_stalled_until) {  // the RT thread is delayed
                if (rt_stalled_until > t) break;
                wake = rt_stalled_until;
            }
            now = wake;
            in_rt = true;
            engine.on_wake(now);
            in_rt = false;
        }
        now = std::max(now, t);
    }

    // RtPlatform and CoordinatorPort
    uint64_t now_ns() override
    {
        // Preempts the coordinator right after it published the commit.
        if (!in_rt && preempt_until != 0 &&
            state.phase.load() == p0::Phase::kCommitting) {
            const uint64_t until = preempt_until;
            preempt_until = 0;
            run_until(until);
        }
        return now;
    }

    // RtPlatform
    void fire(const p0::RtSlot& slot, uint64_t target_ns, uint64_t) override
    {
        fires.push_back({now, engine.active()->token, slot.task_id_hash,
                         slot.shm_idx, target_ns});
        __atomic_fetch_add(&shm.tasks[slot.shm_idx].counter, 1u,
                           __ATOMIC_SEQ_CST);
    }
    void open_tt_gate() override { set_gate(false); }

    // CoordinatorPort
    void sleep_until(uint64_t t) override { run_until(t); }
    bool stage_bank(const p0::NPrepared&) override
    {
        ++stage_calls;
        if (fail_stage) return false;
        staged = true;
        return true;
    }
    void discard_staged_bank() override
    {
        staged = false;
        ++discards;
    }
    bool tt_idle(uint32_t) override
    {
        ++idle_calls;
        return idle_fn ? idle_fn(now) : true;
    }
    void set_tt_gate(bool closed) override { set_gate(closed); }
    bool select_staged_bank() override
    {
        if (select_cost) run_until(now + select_cost);
        if (fail_select || !staged) return false;
        bank ^= 1;
        staged = false;
        ++selects;
        return true;
    }
    int active_bank() override { return bank; }
    bool reset_slot_state(uint32_t, uint64_t first_start_ns) override
    {
        slot_resets.push_back({now, first_start_ns});
        return true;
    }

    void set_gate(bool closed)
    {
        gate = closed;
        gate_log.push_back({now, closed});
    }

    uint64_t now = 0;
    uint64_t rt_stalled_until = 0;
    uint64_t preempt_until = 0;
    bool in_rt = false;
    p0::HandoverState state;
    timpani_ttsched_shm shm;
    p0::RtEngine engine;

    bool gate = false;
    int bank = 0;
    bool staged = false;
    bool fail_stage = false;
    bool fail_select = false;
    uint64_t select_cost = 0;
    int stage_calls = 0;
    int discards = 0;
    int selects = 0;
    int idle_calls = 0;
    std::function<bool(uint64_t)> idle_fn;
    std::vector<Fire> fires;
    std::vector<std::pair<uint64_t, bool>> gate_log;
    std::vector<std::pair<uint64_t, uint64_t>> slot_resets;
};

struct Outcome {
    p0::ArmResult arm;
    p0::Completion done;
};

class HandoverTest : public ::testing::Test
{
  protected:
    uint32_t generation()
    {
        return __atomic_load_n(&world_.shm.generation, __ATOMIC_ACQUIRE);
    }

    p0::NPrepared prepared(const std::vector<SlotSpec>& specs)
    {
        return make_prepared(specs, generation());
    }

    const p0::Context* build(const p0::NPrepared& prepared)
    {
        auto ctx = std::make_unique<p0::Context>();
        std::string error;
        EXPECT_TRUE(p0::build_context(prepared, ++token_, *ctx, error))
            << error;
        contexts_.push_back(std::move(ctx));
        return contexts_.back().get();
    }

    // Mirrors TimerMaster::apply_phase0_table().
    Outcome apply(const p0::NPrepared& prepared,
                  const p0::HandoverConfig& config = p0::HandoverConfig())
    {
        const p0::Context* next = build(prepared);
        Outcome out;
        out.arm = p0::arm_handover(world_.state, world_, &world_.shm, active_,
                                   *next, prepared, owners_, config);
        if (out.arm.outcome != p0::ArmOutcome::kReady) return out;
        out.done = p0::await_completion(world_.state, world_,
                                        out.arm.boundary_ns, config);
        if (out.done.applied) {
            active_ = next;
            owners_ = p0::owners_from(prepared.shm);
            world_.state.phase.store(p0::Phase::kIdle);
        }
        return out;
    }

    // Installs table A as the first table: T0 = epoch + H.
    uint64_t install_a()
    {
        world_.run_until(kEpoch + 5 * kMs + kMs / 2);
        const Outcome out = apply(prepared(kTableA));
        EXPECT_TRUE(out.done.applied) << out.arm.error << out.done.error;
        return out.arm.boundary_ns;
    }

    std::vector<Fire> fires_between(uint64_t from, uint64_t to) const
    {
        std::vector<Fire> result;
        for (const auto& fire : world_.fires) {
            if (fire.time >= from && fire.time < to) result.push_back(fire);
        }
        return result;
    }

    World world_;
    std::vector<std::unique_ptr<p0::Context>> contexts_;
    const p0::Context* active_ = nullptr;
    p0::ShmOwners owners_;
    uint64_t token_ = 0;
};

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------

TEST(Phase0Context, BuildsOrderedRtSlotsAndTheTtFreeOffset)
{
    const p0::NPrepared prepared = make_prepared(kTableA, 2);
    p0::Context ctx;
    std::string error;
    ASSERT_TRUE(p0::build_context(prepared, 7, ctx, error)) << error;
    EXPECT_EQ(ctx.token, 7u);
    EXPECT_EQ(ctx.epoch_ns, kEpoch);
    EXPECT_EQ(ctx.hyperperiod_ns, kH);
    EXPECT_TRUE(ctx.has_cpu);
    EXPECT_EQ(ctx.cpu, kCpu);
    ASSERT_EQ(ctx.slots.size(), 4u);
    const uint64_t offsets[] = {0, 1 * kMs, 9 * kMs, 10 * kMs};
    const uint32_t shm_idx[] = {0, 1, 2, 0};
    for (size_t i = 0; i < ctx.slots.size(); ++i) {
        EXPECT_EQ(ctx.slots[i].offset_ns, offsets[i]);
        EXPECT_EQ(ctx.slots[i].deadline_ns,
                  static_cast<uint64_t>(prepared.slots[i].deadline_us) *
                      1000ULL);
        EXPECT_EQ(ctx.slots[i].shm_idx, shm_idx[i]);
        EXPECT_EQ(ctx.slots[i].slot_idx, i);
        EXPECT_EQ(ctx.slots[i].cpu, kCpu);
    }
    EXPECT_EQ(ctx.slots[2].end_ns, 10 * kMs);
    EXPECT_EQ(ctx.tt_free_offset_ns, 11 * kMs);
}

TEST(Phase0Context, RejectsMissingTasksOverrunsAndDisorder)
{
    p0::Context ctx;
    ctx.token = 99;
    std::string error;

    p0::NPrepared missing = make_prepared(kTableA, 2);
    missing.tasks.erase(hash_of("w2_task"));
    EXPECT_FALSE(p0::build_context(missing, 1, ctx, error));
    EXPECT_NE(error.find("without a prepared task"), std::string::npos);

    p0::NPrepared overrun = make_prepared({{"long", 15000, 6000, 0}}, 2);
    EXPECT_FALSE(p0::build_context(overrun, 1, ctx, error));
    EXPECT_NE(error.find("ends after the hyperperiod"), std::string::npos);

    p0::NPrepared disorder = make_prepared(kTableA, 2);
    std::swap(disorder.slots[0], disorder.slots[1]);
    EXPECT_FALSE(p0::build_context(disorder, 1, ctx, error));
    EXPECT_NE(error.find("not ordered"), std::string::npos);
    EXPECT_EQ(ctx.token, 99u);  // untouched on failure
}

TEST(Phase0Boundary, FindsTheFirstGridPointAtOrAfterT)
{
    uint64_t out = 0;
    ASSERT_TRUE(p0::boundary_at_or_after(kEpoch, kH, kEpoch - 1, out));
    EXPECT_EQ(out, kEpoch);
    ASSERT_TRUE(p0::boundary_at_or_after(kEpoch, kH, kEpoch, out));
    EXPECT_EQ(out, kEpoch);
    ASSERT_TRUE(p0::boundary_at_or_after(kEpoch, kH, kEpoch + 1, out));
    EXPECT_EQ(out, kEpoch + kH);
    ASSERT_TRUE(p0::boundary_at_or_after(kEpoch, kH, kEpoch + 3 * kH, out));
    EXPECT_EQ(out, kEpoch + 3 * kH);
    EXPECT_FALSE(p0::boundary_at_or_after(kEpoch, 0, kEpoch, out));
    // The last representable boundary is accepted, the next one is not.
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    ASSERT_TRUE(p0::boundary_at_or_after(0, kH, max - kH, out));
    EXPECT_EQ(out, max / kH * kH);
    EXPECT_FALSE(p0::boundary_at_or_after(0, kH, max - 1, out));
}

TEST(Phase0ShmImage, KeepsRetainedCountersAndResetsOnlyNewOwners)
{
    timpani_ttsched_shm shm;
    std::memset(&shm, 0, sizeof(shm));
    const uint32_t counters[] = {41, 7, 5, 77};
    for (uint32_t i = 0; i < 4; ++i) shm.tasks[i].counter = counters[i];
    const p0::NPrepared a = make_prepared(kTableA, 2);
    const p0::NPrepared b = make_prepared(kTableB, 2);
    const uint32_t generation_before = shm.generation;

    p0::write_shm_image(&shm, b.shm, p0::owners_from(a.shm));

    EXPECT_EQ(shm.tasks[0].counter, 41u);  // w1_fast retained
    EXPECT_EQ(shm.tasks[1].counter, 7u);   // w1_anchor retained
    EXPECT_EQ(shm.tasks[2].counter, 5u);   // w2_task removed: untouched
    EXPECT_EQ(shm.tasks[3].counter, 0u);   // w3_task new owner
    EXPECT_STREQ(const_cast<const char*>(shm.tasks[0].name), "w1_fast");
    EXPECT_STREQ(const_cast<const char*>(shm.tasks[3].name), "w3_task");
    for (char ch : shm.tasks[2].name) EXPECT_EQ(ch, '\0');
    EXPECT_EQ(shm.n_tasks, 4u);
    EXPECT_EQ(shm.generation, generation_before);  // publication is separate
}

// ---------------------------------------------------------------------------
// Handover through the coordinator and the RT engine
// ---------------------------------------------------------------------------

TEST_F(HandoverTest, FirstTableAppliesAtTheFirstBoundaryAfterTheMargin)
{
    const uint32_t base = generation();
    const uint64_t t0 = install_a();
    EXPECT_EQ(t0, kEpoch + kH);
    EXPECT_EQ(world_.state.done_planned_ns.load(), t0);
    EXPECT_EQ(world_.state.done_actual_ns.load(), t0);
    EXPECT_EQ(world_.state.done_token.load(), token_);

    world_.run_until(t0 + kH - 1);
    const std::vector<Fire> fires = fires_between(0, t0 + kH);
    ASSERT_EQ(fires.size(), 4u);
    const uint64_t offsets[] = {0, 1 * kMs, 9 * kMs, 10 * kMs};
    for (size_t i = 0; i < fires.size(); ++i) {
        EXPECT_EQ(fires[i].time, t0 + offsets[i]);
        EXPECT_EQ(fires[i].target, t0 + offsets[i]);
    }
    EXPECT_EQ(world_.state.fired_hyperperiod_ns.load(), t0);

    ASSERT_EQ(world_.gate_log.size(), 2u);
    EXPECT_TRUE(world_.gate_log[0].second);
    EXPECT_LT(world_.gate_log[0].first, t0);
    EXPECT_EQ(world_.gate_log[1], std::make_pair(t0, false));
    EXPECT_EQ(world_.bank, 1);
    ASSERT_EQ(world_.slot_resets.size(), 1u);
    EXPECT_EQ(world_.slot_resets[0].second, t0);

    EXPECT_EQ(generation(), base + 2);
    EXPECT_EQ(world_.shm.magic, TIMPANI_TTSCHED_MAGIC);
    EXPECT_EQ(world_.shm.n_tasks, 3u);
    EXPECT_EQ(world_.shm.tasks[0].counter, 2u);  // two real w1_fast fires
    EXPECT_EQ(world_.shm.tasks[2].counter, 1u);
}

TEST_F(HandoverTest, ReplaceSwitchesExactlyAtTheBoundaryWithoutMixing)
{
    const uint64_t t0 = install_a();
    world_.run_until(t0 + 3 * kMs);
    const uint32_t base = generation();
    const uint64_t token_a = token_;

    const Outcome out = apply(prepared(kTableB));
    ASSERT_EQ(out.arm.outcome, p0::ArmOutcome::kReady) << out.arm.error;
    ASSERT_TRUE(out.done.applied) << out.done.error;
    const uint64_t t1 = t0 + kH;
    EXPECT_EQ(out.arm.boundary_ns, t1);
    EXPECT_EQ(out.done.planned_ns, t1);
    EXPECT_EQ(out.done.actual_ns, t1);
    EXPECT_EQ(out.done.token, token_);
    EXPECT_EQ(out.arm.shm_generation, base + 2);

    world_.run_until(t1 + kH - 1);
    const std::vector<Fire> before = fires_between(t0, t1);
    const std::vector<Fire> after = fires_between(t1, t1 + kH);
    ASSERT_EQ(before.size(), 4u);  // the old hyperperiod was not cut
    ASSERT_EQ(after.size(), 4u);
    for (const auto& fire : before) EXPECT_EQ(fire.token, token_a);
    for (const auto& fire : after) {
        EXPECT_EQ(fire.token, token_);
        EXPECT_NE(fire.task, hash_of("w2_task"));
    }
    EXPECT_EQ(after.back().time, t1 + 11 * kMs);
    EXPECT_EQ(after.back().task, hash_of("w3_task"));

    // The gate was closed only within [G, T) of the old hyperperiod.
    ASSERT_EQ(world_.gate_log.size(), 4u);
    EXPECT_EQ(world_.gate_log[2], std::make_pair(t0 + 11 * kMs, true));
    EXPECT_EQ(world_.gate_log[3], std::make_pair(t1, false));
    EXPECT_EQ(world_.selects, 2);
    EXPECT_EQ(world_.slot_resets.back().second, t1);

    // Retained counters kept counting; the new owner started from zero.
    EXPECT_EQ(generation(), base + 2);
    EXPECT_EQ(world_.shm.tasks[0].counter, 4u);  // 2 old + 2 new fires
    EXPECT_EQ(world_.shm.tasks[1].counter, 2u);
    EXPECT_EQ(world_.shm.tasks[3].counter, 1u);
    EXPECT_EQ(world_.shm.tasks[2].name[0], '\0');
    EXPECT_EQ(world_.shm.n_tasks, 4u);
}

TEST_F(HandoverTest, LatePreparationRunsTheOldTableOneMoreHyperperiod)
{
    const uint64_t t0 = install_a();
    world_.run_until(t0 + 19 * kMs + kMs / 2);  // less than the margin left
    const uint64_t token_a = token_;

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    const uint64_t t2 = t0 + 2 * kH;
    EXPECT_EQ(out.arm.boundary_ns, t2);
    world_.run_until(t2 + kH - 1);
    const std::vector<Fire> old_fires = fires_between(t0, t2);
    ASSERT_EQ(old_fires.size(), 8u);
    for (const auto& fire : old_fires) EXPECT_EQ(fire.token, token_a);
    for (const auto& fire : fires_between(t2, t2 + kH)) {
        EXPECT_EQ(fire.token, token_);
    }
    EXPECT_GE(world_.gate_log[2].first, t0 + kH + 11 * kMs);
}

TEST_F(HandoverTest, LateOldFiresPostponeTheGateToTheNextBoundary)
{
    const uint64_t t0 = install_a();
    const uint64_t t1 = t0 + kH;
    world_.run_until(t0 + 5 * kMs);
    // The RT thread fires the last two old slots only 0.5 ms before T1.
    world_.rt_stalled_until = t1 - kMs / 2;

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    EXPECT_EQ(out.arm.boundary_ns, t1 + kH);
    ASSERT_EQ(world_.gate_log.size(), 4u);
    EXPECT_EQ(world_.gate_log[2], std::make_pair(t1 + 11 * kMs, true));
    const std::vector<Fire> late = fires_between(t1 - kMs, t1);
    ASSERT_EQ(late.size(), 2u);
    EXPECT_EQ(late[0].target, t0 + 9 * kMs);
    EXPECT_EQ(late[1].target, t0 + 10 * kMs);
}

TEST_F(HandoverTest, BusyTtExecutionPostponesTheGate)
{
    const uint64_t t0 = install_a();
    const uint64_t t1 = t0 + kH;
    world_.idle_fn = [t1](uint64_t now) { return now >= t1 + 15 * kMs; };

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    EXPECT_EQ(out.arm.boundary_ns, t1 + kH);
    ASSERT_EQ(world_.gate_log.size(), 4u);
    EXPECT_EQ(world_.gate_log[2], std::make_pair(t1 + 15 * kMs, true));
}

TEST_F(HandoverTest, GateRaceRollsBackWithoutTouchingTheShmOrBank)
{
    const uint64_t t0 = install_a();
    const uint32_t base = generation();
    int calls = 0;
    world_.idle_fn = [&calls](uint64_t) { return ++calls != 2; };

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    const uint64_t t2 = t0 + 2 * kH;
    EXPECT_EQ(out.arm.boundary_ns, t2);
    ASSERT_EQ(world_.gate_log.size(), 6u);
    EXPECT_EQ(world_.gate_log[2], std::make_pair(t0 + 11 * kMs, true));
    EXPECT_EQ(world_.gate_log[3], std::make_pair(t0 + 11 * kMs, false));
    EXPECT_EQ(world_.gate_log[4], std::make_pair(t0 + kH + 11 * kMs, true));
    EXPECT_EQ(world_.gate_log[5], std::make_pair(t2, false));
    EXPECT_EQ(world_.selects, 2);       // one bank switch for B
    EXPECT_EQ(generation(), base + 2);  // one publication for B
}

TEST_F(HandoverTest, NeverQuiescentIsRejectedAfterEightBoundaries)
{
    const uint64_t t0 = install_a();
    const uint32_t base = generation();
    const uint64_t token_a = token_;
    world_.idle_fn = [](uint64_t) { return false; };

    const Outcome out = apply(prepared(kTableB));
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kRejected);
    EXPECT_NE(out.arm.error.find("8 boundaries"), std::string::npos);
    EXPECT_EQ(world_.discards, 1);
    EXPECT_FALSE(world_.staged);
    EXPECT_EQ(world_.gate_log.size(), 2u);  // only the first table's
    EXPECT_FALSE(world_.gate);
    EXPECT_EQ(world_.bank, 1);
    EXPECT_EQ(generation(), base);
    EXPECT_EQ(world_.shm.magic, TIMPANI_TTSCHED_MAGIC);
    EXPECT_EQ(world_.state.phase.load(), p0::Phase::kIdle);
    EXPECT_GE(world_.now, t0 + 8 * kH);
    const std::vector<Fire> fires = fires_between(t0, t0 + 8 * kH);
    EXPECT_EQ(fires.size(), 32u);
    for (const auto& fire : fires) EXPECT_EQ(fire.token, token_a);
}

TEST_F(HandoverTest, ShortTtFreeWindowIsRejectedBeforeStaging)
{
    world_.run_until(kEpoch + 5 * kMs);
    const Outcome first = apply(prepared({{"long", 0, 19500, 0}}));
    ASSERT_TRUE(first.done.applied) << first.arm.error << first.done.error;
    const int stage_calls = world_.stage_calls;

    const Outcome out = apply(prepared(kTableB));
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kRejected);
    EXPECT_NE(out.arm.error.find("TT-free window"), std::string::npos);
    EXPECT_EQ(world_.stage_calls, stage_calls);
}

TEST_F(HandoverTest, StagingFailureAndShmChangesAreRejectedWithoutSideEffects)
{
    const uint64_t t0 = install_a();
    const uint32_t base = generation();

    world_.fail_stage = true;
    Outcome out = apply(prepared(kTableB));
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kRejected);
    EXPECT_NE(out.arm.error.find("staging"), std::string::npos);
    world_.fail_stage = false;

    const int stage_calls = world_.stage_calls;
    out = apply(make_prepared(kTableB, base + 2));
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kRejected);
    EXPECT_NE(out.arm.error.find("SHM generation"), std::string::npos);
    EXPECT_EQ(world_.stage_calls, stage_calls);

    EXPECT_EQ(world_.gate_log.size(), 2u);
    EXPECT_EQ(generation(), base);
    EXPECT_EQ(world_.state.phase.load(), p0::Phase::kIdle);
    EXPECT_LT(world_.now, t0 + kH);  // rejected at once
}

TEST_F(HandoverTest, LateCoordinatorNeverPublishesACommitInsideTheMargin)
{
    const uint64_t t0 = install_a();
    const uint64_t t1 = t0 + kH;
    world_.run_until(t1 - kMs / 2);
    // A commit published now would be preempted past T1.
    world_.preempt_until = t1 + kMs / 2;

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    EXPECT_EQ(out.arm.boundary_ns, t1 + kH);
    EXPECT_FALSE(world_.engine.failed());
}

TEST_F(HandoverTest, CommitPreemptedNearTheBoundaryRollsBackBeforeTheGate)
{
    const uint64_t t0 = install_a();
    const uint64_t t1 = t0 + kH;
    const uint32_t base = generation();
    world_.preempt_until = t1 - kMs / 2;

    const Outcome out = apply(prepared(kTableB));
    ASSERT_TRUE(out.done.applied) << out.arm.error << out.done.error;
    EXPECT_EQ(out.arm.boundary_ns, t1 + kH);
    ASSERT_EQ(world_.gate_log.size(), 4u);  // no gate change for T1
    EXPECT_EQ(world_.gate_log[2], std::make_pair(t1 + 11 * kMs, true));
    EXPECT_EQ(generation(), base + 2);
    EXPECT_EQ(world_.selects, 2);
}

TEST_F(HandoverTest, CommitPreemptedPastTheBoundaryFailsWithTheGateOpen)
{
    const uint64_t t0 = install_a();
    const uint64_t t1 = t0 + kH;
    const uint32_t base = generation();
    world_.preempt_until = t1 + kMs / 2;

    const Outcome out = apply(prepared(kTableB));
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kFailed);
    EXPECT_NE(out.arm.error.find("while committing"), std::string::npos);
    EXPECT_TRUE(world_.engine.failed());
    EXPECT_EQ(world_.gate_log.size(), 2u);
    EXPECT_FALSE(world_.gate);
    EXPECT_EQ(generation(), base);
    EXPECT_EQ(world_.shm.magic, TIMPANI_TTSCHED_MAGIC);
    EXPECT_EQ(world_.selects, 1);
    world_.run_until(t1 + 2 * kH);
    EXPECT_TRUE(fires_between(t1, t1 + 2 * kH).empty());
}

TEST_F(HandoverTest, NotReadyByTheBoundaryFailsAndIsNotRestored)
{
    const uint64_t t0 = install_a();
    const uint32_t base = generation();
    world_.select_cost = 10 * kMs;  // the TT-free steps overrun T

    const Outcome out = apply(prepared(kTableB));
    const uint64_t t1 = t0 + kH;
    EXPECT_EQ(out.arm.outcome, p0::ArmOutcome::kFailed);
    EXPECT_EQ(out.arm.boundary_ns, t1);
    EXPECT_TRUE(world_.engine.failed());
    EXPECT_EQ(world_.state.phase.load(), p0::Phase::kFailed);
    EXPECT_EQ(world_.state.done_planned_ns.load(), t1);
    EXPECT_EQ(world_.state.done_token.load(), token_);

    world_.run_until(t1 + 3 * kH);
    EXPECT_TRUE(fires_between(t1, t1 + 3 * kH).empty());
    EXPECT_TRUE(world_.gate);  // neither reopened nor restored
    EXPECT_EQ(generation(), base + 1);
    EXPECT_EQ(world_.shm.magic, 0u);
}

TEST_F(HandoverTest, ClearKeepsTheGridAndALaterAddNeedsNoRestart)
{
    const uint64_t t0 = install_a();
    world_.run_until(t0 + 3 * kMs);

    p0::NPrepared clear = prepared({});
    const Outcome cleared = apply(clear);
    ASSERT_TRUE(cleared.done.applied) << cleared.arm.error;
    const uint64_t t1 = t0 + kH;
    EXPECT_EQ(cleared.arm.boundary_ns, t1);
    EXPECT_EQ(world_.slot_resets.back().second, 0u);
    EXPECT_EQ(world_.shm.n_tasks, 0u);
    for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; ++i) {
        EXPECT_EQ(world_.shm.tasks[i].name[0], '\0') << i;
    }

    world_.run_until(t1 + 3 * kH);
    EXPECT_TRUE(fires_between(t1, t1 + 3 * kH).empty());
    EXPECT_EQ(world_.state.fired_hyperperiod_ns.load(), t1 + 3 * kH);

    const Outcome added = apply(prepared(kTableA));
    ASSERT_TRUE(added.done.applied) << added.arm.error << added.done.error;
    const uint64_t t4 = t1 + 4 * kH;
    EXPECT_EQ(added.arm.boundary_ns, t4);
    world_.run_until(t4 + kH - 1);
    const std::vector<Fire> fires = fires_between(t4, t4 + kH);
    ASSERT_EQ(fires.size(), 4u);
    for (const auto& fire : fires) EXPECT_EQ(fire.token, token_);
    EXPECT_EQ(world_.engine.active()->token, token_);
}

// ---------------------------------------------------------------------------
// RT engine boundary rules
// ---------------------------------------------------------------------------

TEST_F(HandoverTest, EngineIgnoresALaterBoundaryUntilItArrives)
{
    const uint64_t t0 = install_a();
    const uint64_t token_a = token_;
    const p0::NPrepared b = prepared(kTableB);
    const p0::Context* next = build(b);
    const uint64_t t2 = t0 + 2 * kH;
    world_.state.boundary_ns = t2;
    world_.state.pending_token = next->token;
    world_.state.context = next;
    world_.state.shm_publishing = ttsched_shm_begin_publish(&world_.shm);
    world_.state.phase = p0::Phase::kReady;

    world_.run_until(t2 - 1);
    for (const auto& fire : fires_between(t0, t2)) {
        EXPECT_EQ(fire.token, token_a);
    }
    world_.run_until(t2 + kH - 1);
    EXPECT_EQ(world_.state.phase.load(), p0::Phase::kApplied);
    EXPECT_EQ(world_.state.done_planned_ns.load(), t2);
    const std::vector<Fire> after = fires_between(t2, t2 + kH);
    ASSERT_EQ(after.size(), 4u);
    for (const auto& fire : after) EXPECT_EQ(fire.token, next->token);
}

TEST_F(HandoverTest, EngineStopsAfterACoordinatorFailure)
{
    const uint64_t t0 = install_a();
    world_.state.phase = p0::Phase::kFailed;
    world_.run_until(t0 + 3 * kH);
    EXPECT_TRUE(world_.engine.failed());
    EXPECT_TRUE(fires_between(t0 + kH, t0 + 3 * kH).empty());
}

// ---------------------------------------------------------------------------
// Completion wait
// ---------------------------------------------------------------------------

class ClockOnlyPort final : public p0::CoordinatorPort
{
  public:
    uint64_t now_ns() override { return now; }
    void sleep_until(uint64_t t) override { now = std::max(now, t); }
    bool stage_bank(const p0::NPrepared&) override { return true; }
    void discard_staged_bank() override {}
    bool tt_idle(uint32_t) override { return true; }
    void set_tt_gate(bool) override {}
    bool select_staged_bank() override { return true; }
    int active_bank() override { return 0; }
    bool reset_slot_state(uint32_t, uint64_t) override { return true; }
    uint64_t now = 0;
};

TEST(Phase0Completion, TimesOutWithoutAnRtCompletion)
{
    p0::HandoverState state;
    state.phase = p0::Phase::kReady;
    ClockOnlyPort port;
    p0::HandoverConfig config;
    config.completion_timeout_ns = 5 * kMs;
    const p0::Completion done =
        p0::await_completion(state, port, kEpoch, config);
    EXPECT_FALSE(done.applied);
    EXPECT_NE(done.error.find("no completion"), std::string::npos);
    EXPECT_EQ(port.now, kEpoch + 5 * kMs);
}

// ---------------------------------------------------------------------------
// Real threads: the coordinator and the RT engine run concurrently.
// ---------------------------------------------------------------------------

class RealPlatform final : public p0::RtPlatform, public p0::CoordinatorPort
{
  public:
    struct RealFire {
        uint64_t time;
        uint64_t task;
    };

    explicit RealPlatform(timpani_ttsched_shm* shm) : shm_(shm) {}

    uint64_t now_ns() override
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    void sleep_until(uint64_t ns) override
    {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(ns))));
    }

    void fire(const p0::RtSlot& slot, uint64_t, uint64_t) override
    {
        const size_t i = n_fires.fetch_add(1, std::memory_order_relaxed);
        if (i < fires.size()) fires[i] = {now_ns(), slot.task_id_hash};
        __atomic_fetch_add(&shm_->tasks[slot.shm_idx].counter, 1u,
                           __ATOMIC_SEQ_CST);
    }
    void open_tt_gate() override { gate.store(false); }

    bool stage_bank(const p0::NPrepared&) override { return true; }
    void discard_staged_bank() override {}
    bool tt_idle(uint32_t) override { return true; }
    void set_tt_gate(bool closed) override { gate.store(closed); }
    bool select_staged_bank() override
    {
        bank ^= 1;
        return true;
    }
    int active_bank() override { return bank; }
    bool reset_slot_state(uint32_t, uint64_t) override { return true; }

    std::array<RealFire, 4096> fires{};
    std::atomic<size_t> n_fires{0};
    std::atomic<bool> gate{false};
    int bank = 0;

  private:
    timpani_ttsched_shm* shm_;
};

TEST(Phase0RealThreads, HandsOverAtABoundaryWhileTheRtThreadRuns)
{
    timpani_ttsched_shm shm;
    std::memset(&shm, 0, sizeof(shm));
    ttsched_shm_reset(&shm);
    p0::HandoverState state;
    RealPlatform platform(&shm);
    p0::RtEngine engine(state, platform, &shm);

    std::atomic<bool> stop{false};
    std::thread rt([&] {
        while (!stop.load()) {
            const uint64_t wake = engine.next_wake_ns();
            const uint64_t now = platform.now_ns();
            if (now < wake) {
                platform.sleep_until(std::min(wake, now + 2 * kMs));
                continue;
            }
            engine.on_wake(platform.now_ns());
        }
    });

    p0::HandoverConfig config;
    config.margin_ns = 2 * kMs;
    config.poll_ns = 200000;
    const uint64_t epoch = platform.now_ns() + 10 * kMs;
    const std::vector<SlotSpec> a = {{"a0", 0, 2000, 0}, {"a1", 5000, 2000, 1}};
    const std::vector<SlotSpec> b = {{"b0", 0, 2000, 2}, {"b1", 8000, 2000, 3}};
    p0::ShmOwners owners;
    std::vector<std::unique_ptr<p0::Context>> contexts;
    const p0::Context* active = nullptr;
    uint64_t boundaries[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const p0::NPrepared prepared = make_prepared(
            i == 0 ? a : b, __atomic_load_n(&shm.generation, __ATOMIC_ACQUIRE),
            epoch);
        contexts.push_back(std::make_unique<p0::Context>());
        std::string error;
        ASSERT_TRUE(
            p0::build_context(prepared, i + 1, *contexts.back(), error));
        const p0::ArmResult armed =
            p0::arm_handover(state, platform, &shm, active, *contexts.back(),
                             prepared, owners, config);
        ASSERT_EQ(armed.outcome, p0::ArmOutcome::kReady) << armed.error;
        const p0::Completion done =
            p0::await_completion(state, platform, armed.boundary_ns, config);
        ASSERT_TRUE(done.applied) << done.error;
        EXPECT_GE(done.actual_ns, armed.boundary_ns);
        EXPECT_EQ((armed.boundary_ns - epoch) % kH, 0u);
        boundaries[i] = armed.boundary_ns;
        active = contexts.back().get();
        owners = p0::owners_from(prepared.shm);
        state.phase.store(p0::Phase::kIdle);
    }
    platform.sleep_until(boundaries[1] + 2 * kH);
    stop = true;
    rt.join();

    const size_t n = std::min(platform.n_fires.load(), platform.fires.size());
    const std::set<uint64_t> table_b = {hash_of("b0"), hash_of("b1")};
    bool in_b = false;
    size_t b_fires = 0;
    for (size_t i = 0; i < n; ++i) {
        const bool is_b = table_b.count(platform.fires[i].task) != 0;
        if (is_b && !in_b) EXPECT_GE(platform.fires[i].time, boundaries[1]);
        EXPECT_FALSE(in_b && !is_b) << "table A fired after the switch";
        in_b = in_b || is_b;
        b_fires += is_b ? 1 : 0;
    }
    EXPECT_GE(b_fires, 4u);
    EXPECT_FALSE(platform.gate.load());
    EXPECT_EQ(__atomic_load_n(&shm.magic, __ATOMIC_ACQUIRE),
              TIMPANI_TTSCHED_MAGIC);
}

}  // namespace
