// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// DDR-011 Phase 0 P0-T06: one attempt on the main non-RT loop. The ACK is
// decided only after the boundary switch is confirmed and the old bank is
// cleaned up; failures after the handover started never claim a result.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "phase0_attempt.h"

namespace p0 = timpani::node::phase0;
using p0::AttemptOutcome;
using p0::Table;
using timpani::node::v1::TtSlot;

namespace {

constexpr uint64_t kEpoch = 1000000000ULL;
constexpr uint32_t kHyperperiodUs = 10000;
constexpr uint32_t kCpu = 2;
constexpr uint64_t kStartNs = kEpoch + 25 * 1000000ULL;  // 2.5 hyperperiods
constexpr uint64_t kApplyDelayNs = 20000;
const char kNode[] = "node-a";

uint64_t fnv1a(const std::string& text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

Table make_table(const std::string& table_id)
{
    Table table;
    table.set_table_id(table_id);
    table.set_node_id(kNode);
    table.set_hyperperiod_us(kHyperperiodUs);
    table.set_epoch_ns(kEpoch);
    auto* partition = table.add_partitions();
    partition->set_partition_id("p0");
    partition->mutable_cpuset()->add_cpus(kCpu);
    partition->mutable_cpuset()->set_isolated(true);
    auto* layer = partition->add_layers();
    layer->set_layer_index(1);
    layer->set_model(timpani::node::v1::TIME_TRIGGERED);
    return table;
}

void add_slot(Table& table, const std::string& workload,
              const std::string& task, uint32_t offset_us,
              uint32_t current_limit = 3)
{
    TtSlot* slot =
        table.mutable_partitions(0)->mutable_layers(0)->add_tt_slots();
    slot->set_workload_id(workload);
    slot->set_task_id(task);
    slot->set_offset_us(offset_us);
    slot->set_duration_us(100);
    slot->set_deadline_us(100);
    slot->set_cpu(kCpu);
    slot->set_workload_id_hash(fnv1a(workload));
    slot->set_task_id_hash(fnv1a(task));
    slot->set_current_limit(current_limit);
}

// Records every side effect in order. By default apply() switches to the
// other bank at T + kApplyDelayNs and publishes the prepared generation.
class FakePort : public p0::AttemptPort
{
  public:
    uint64_t now_ns() override { return now; }

    void sleep_until(uint64_t ns) override
    {
        calls.push_back("sleep");
        sleep_targets.push_back(ns);
        now = std::max(now, ns);
    }

    int active_bank() override
    {
        calls.push_back("active_bank");
        return bank;
    }

    bool apply(const p0::NPrepared& prepared, p0::ApplyResult& result) override
    {
        calls.push_back("apply");
        applied_table = prepared.table_id;
        if (reject) {
            result.rejected = true;
            result.error = error;
            return false;
        }
        result.token = 1;
        result.planned_boundary_ns = prepared.boundary.target_ns;
        result.actual_apply_ns = prepared.boundary.target_ns + kApplyDelayNs;
        if (fail) {
            result.error = error;
            return false;
        }
        result.applied = true;
        result.shm_generation = prepared.shm.next_generation;
        result.active_bank = 1 - bank;
        if (edit_result) edit_result(result);
        bank = result.active_bank;
        shm_generation = result.shm_generation;
        now = std::max(now, result.actual_apply_ns);
        return true;
    }

    // An unreadable SHM still fills plausible values: only the return value
    // may reject it.
    bool shm_state(uint32_t& generation, bool& magic_valid) override
    {
        calls.push_back("shm_state");
        generation = shm_generation;
        magic_valid = shm_magic;
        if (edit_shm) edit_shm(generation, magic_valid);
        return shm_readable;
    }

    bool clear_inactive_bank() override
    {
        calls.push_back("clear");
        if (never_clear) return false;
        if (busy_clears > 0) {
            --busy_clears;
            return false;
        }
        return true;
    }

    void update_tracking(const p0::NPrepared& prepared) override
    {
        calls.push_back("tracking");
        tracked_table = prepared.table_id;
        tracked_workloads.clear();
        for (const auto& item : prepared.workloads) {
            tracked_workloads.insert(item.second);
        }
    }

    void reset_faults(const p0::FaultLimits& value) override
    {
        calls.push_back("faults");
        limits = value;
    }

    size_t count(const std::string& call) const
    {
        return static_cast<size_t>(
            std::count(calls.begin(), calls.end(), call));
    }

    uint64_t now = kStartNs;
    int bank = 0;
    bool reject = false;
    bool fail = false;
    std::string error;
    std::function<void(p0::ApplyResult&)> edit_result;
    bool shm_readable = true;
    bool shm_magic = true;
    uint32_t shm_generation = 4;
    std::function<void(uint32_t&, bool&)> edit_shm;
    int busy_clears = 0;
    bool never_clear = false;

    std::vector<std::string> calls;
    std::vector<uint64_t> sleep_targets;
    std::string applied_table;
    std::string tracked_table;
    std::set<std::string> tracked_workloads;
    p0::FaultLimits limits;
};

class Phase0AttemptTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        env_.node_id = kNode;
        env_.isolated_cpu_mask = 1ULL << kCpu;
        env_.now_ns = kStartNs;
        env_.shm_generation = 4;
        env_.resolve_pids = [this](const std::set<std::string>& names) {
            std::map<std::string, std::vector<int32_t>> found;
            for (const auto& name : names) {
                auto it = pids_.find(name);
                if (it != pids_.end()) found[name] = it->second;
            }
            return found;
        };
        pids_ = {{"taskA", {101}}, {"taskB", {102}}, {"taskC", {103}}};
    }

    static Table table_w1_w2(const std::string& table_id = "t1")
    {
        Table table = make_table(table_id);
        add_slot(table, "W1", "taskA", 0);
        add_slot(table, "W2", "taskB", 2000);
        return table;
    }

    p0::AttemptResult attempt(const Table& table)
    {
        return p0::run_attempt(table, env_, baseline_, port_);
    }

    // Applies @table so that later attempts start from a known baseline.
    void activate(const Table& table)
    {
        ASSERT_EQ(attempt(table).outcome, AttemptOutcome::kApplied);
        port_.calls.clear();
        env_.shm_generation = port_.shm_generation;
    }

    void expect_baseline_workloads(const std::set<std::string>& expected)
    {
        std::set<std::string> actual;
        for (const auto& item : baseline_.workloads) {
            actual.insert(item.second);
        }
        EXPECT_EQ(actual, expected);
    }

    p0::NodeEnvironment env_;
    p0::ActiveBaseline baseline_;
    FakePort port_;
    std::map<std::string, std::vector<int32_t>> pids_;
};

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Applied
// ---------------------------------------------------------------------------

TEST_F(Phase0AttemptTest, AcksOnlyAfterSwitchChecksAndCleanup)
{
    const auto result = attempt(table_w1_w2());

    EXPECT_EQ(result.outcome, AttemptOutcome::kApplied);
    EXPECT_TRUE(result.reason.empty());
    const std::vector<std::string> expected = {
        "active_bank", "apply", "shm_state", "clear", "tracking", "faults"};
    EXPECT_EQ(port_.calls, expected);
    EXPECT_EQ(port_.applied_table, "t1");
    EXPECT_EQ(port_.tracked_table, "t1");
    EXPECT_EQ(port_.tracked_workloads, (std::set<std::string>{"W1", "W2"}));
    EXPECT_TRUE(baseline_.has_table);
    expect_baseline_workloads({"W1", "W2"});
    EXPECT_EQ(baseline_.tasks.size(), 2u);
}

TEST_F(Phase0AttemptTest, LogCarriesTheEvidenceFields)
{
    const auto result = attempt(table_w1_w2());

    ASSERT_EQ(result.outcome, AttemptOutcome::kApplied);
    const uint64_t target = kEpoch + 30 * 1000000ULL;
    EXPECT_EQ(result.log.rfind("[RTU-P0] node_id=node-a table_id=t1 ", 0), 0u)
        << result.log;
    EXPECT_TRUE(contains(result.log, " added_workloads=W1,W2 "));
    EXPECT_TRUE(contains(result.log, " removed_workloads=- "));
    EXPECT_TRUE(
        contains(result.log, " planned_boundary_ns=" + std::to_string(target)));
    EXPECT_TRUE(
        contains(result.log,
                 " actual_apply_ns=" + std::to_string(target + kApplyDelayNs)));
    EXPECT_TRUE(contains(result.log, " active_bank=1 shm_gen=6 "));
    EXPECT_TRUE(contains(result.log, " result=applied"));
    EXPECT_FALSE(contains(result.log, " reason="));
}

TEST_F(Phase0AttemptTest, LogsWorkloadChangesAgainstTheBaseline)
{
    activate(table_w1_w2());
    Table next = make_table("t2");
    add_slot(next, "W1", "taskA", 0);
    add_slot(next, "W3", "taskC", 4000);

    const auto result = attempt(next);

    ASSERT_EQ(result.outcome, AttemptOutcome::kApplied) << result.log;
    EXPECT_TRUE(
        contains(result.log, " added_workloads=W3 removed_workloads=W2 "));
    EXPECT_TRUE(contains(result.log, " active_bank=0 shm_gen=8 "));
    EXPECT_EQ(port_.tracked_workloads, (std::set<std::string>{"W1", "W3"}));
    expect_baseline_workloads({"W1", "W3"});
}

TEST_F(Phase0AttemptTest, AcceptsAnApplyExactlyAtTheBoundary)
{
    port_.edit_result = [](p0::ApplyResult& r) {
        r.actual_apply_ns = r.planned_boundary_ns;
    };

    EXPECT_EQ(attempt(table_w1_w2()).outcome, AttemptOutcome::kApplied);
}

TEST_F(Phase0AttemptTest, RetriesCleanupUntilTheOldBankIsReleased)
{
    port_.busy_clears = 3;

    const auto result = attempt(table_w1_w2());

    EXPECT_EQ(result.outcome, AttemptOutcome::kApplied);
    const std::vector<std::string> expected = {
        "active_bank", "apply", "shm_state", "clear", "sleep",    "clear",
        "sleep",       "clear", "sleep",     "clear", "tracking", "faults"};
    EXPECT_EQ(port_.calls, expected);
    ASSERT_EQ(port_.sleep_targets.size(), 3u);
    const uint64_t first = kEpoch + 30 * 1000000ULL + kApplyDelayNs;
    for (size_t i = 0; i < port_.sleep_targets.size(); ++i) {
        EXPECT_EQ(port_.sleep_targets[i],
                  first + (i + 1) * p0::kOldBankCleanupPollNs);
    }
}

// ---------------------------------------------------------------------------
// Rejected: nothing was changed, so a negative ACK is sent
// ---------------------------------------------------------------------------

TEST_F(Phase0AttemptTest, PrecheckRejectionHasNoSideEffects)
{
    activate(table_w1_w2());
    Table other = table_w1_w2("t2");
    other.set_node_id("node-b");

    const auto result = attempt(other);

    EXPECT_EQ(result.outcome, AttemptOutcome::kRejected);
    EXPECT_FALSE(result.reason.empty());
    EXPECT_TRUE(port_.calls.empty());
    EXPECT_TRUE(
        contains(result.log, " result=rejected reason=" + result.reason));
    expect_baseline_workloads({"W1", "W2"});
}

TEST_F(Phase0AttemptTest, UnknownActiveBankRejectsBeforeApply)
{
    port_.bank = -1;

    const auto result = attempt(table_w1_w2());

    EXPECT_EQ(result.outcome, AttemptOutcome::kRejected);
    EXPECT_EQ(result.reason, "the active BPF bank is unknown");
    EXPECT_EQ(port_.calls, std::vector<std::string>{"active_bank"});
    EXPECT_FALSE(baseline_.has_table);
}

TEST_F(Phase0AttemptTest, ApplyRejectionKeepsTheBaseline)
{
    activate(table_w1_w2());
    port_.reject = true;
    port_.error = "no TT-free window";
    Table next = make_table("t2");
    add_slot(next, "W1", "taskA", 0);

    const auto result = attempt(next);

    EXPECT_EQ(result.outcome, AttemptOutcome::kRejected);
    EXPECT_EQ(result.reason, "no TT-free window");
    const std::vector<std::string> expected = {"active_bank", "apply"};
    EXPECT_EQ(port_.calls, expected);
    expect_baseline_workloads({"W1", "W2"});
}

TEST_F(Phase0AttemptTest, RejectedAttemptReportsTheReason)
{
    const auto result =
        p0::rejected_attempt(kNode, "t9", "SHM generation unavailable");

    EXPECT_EQ(result.outcome, AttemptOutcome::kRejected);
    EXPECT_EQ(result.reason, "SHM generation unavailable");
    EXPECT_EQ(result.log,
              "[RTU-P0] node_id=node-a table_id=t9 result=rejected "
              "reason=SHM generation unavailable");
}

// ---------------------------------------------------------------------------
// Trial failed: after the handover started, no ACK and no restore
// ---------------------------------------------------------------------------

TEST_F(Phase0AttemptTest, ApplyFailureAfterHandoverFailsTheTrial)
{
    port_.fail = true;
    port_.error = "the handover was not ready at T";

    const auto result = attempt(table_w1_w2());

    EXPECT_EQ(result.outcome, AttemptOutcome::kTrialFailed);
    EXPECT_EQ(result.reason, "the handover was not ready at T");
    const std::vector<std::string> expected = {"active_bank", "apply"};
    EXPECT_EQ(port_.calls, expected);
    EXPECT_TRUE(contains(result.log, " result=trial_failed reason="));
    EXPECT_FALSE(baseline_.has_table);
}

TEST_F(Phase0AttemptTest, UnconfirmedSwitchFailsTheTrial)
{
    struct Case {
        const char* name;
        std::function<void(FakePort&)> setup;
        const char* reason;
    };
    const char* kBank = "the active BPF bank did not change";
    const char* kShm = "SHM is not the published stable generation";
    const char* kTime = "actual apply time precedes the boundary";
    const std::vector<Case> cases = {
        {"bank unchanged",
         [](FakePort& port) {
             port.edit_result = [](p0::ApplyResult& r) { r.active_bank = 0; };
         },
         kBank},
        {"bank unknown",
         [](FakePort& port) {
             port.edit_result = [](p0::ApplyResult& r) { r.active_bank = -1; };
         },
         kBank},
        {"SHM unreadable", [](FakePort& port) { port.shm_readable = false; },
         kShm},
        {"SHM magic invalid", [](FakePort& port) { port.shm_magic = false; },
         kShm},
        {"SHM odd generation",
         [](FakePort& port) {
             port.edit_result = [](p0::ApplyResult& r) {
                 r.shm_generation = 7;
             };
         },
         kShm},
        {"SHM other generation",
         [](FakePort& port) {
             port.edit_shm = [](uint32_t& generation, bool&) {
                 generation += 2;
             };
         },
         kShm},
        {"applied before T",
         [](FakePort& port) {
             port.edit_result = [](p0::ApplyResult& r) {
                 r.actual_apply_ns = r.planned_boundary_ns - 1;
             };
         },
         kTime},
        {"no planned boundary",
         [](FakePort& port) {
             port.edit_result = [](p0::ApplyResult& r) {
                 r.planned_boundary_ns = 0;
             };
         },
         kTime},
    };

    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        p0::ActiveBaseline baseline;
        FakePort port;
        item.setup(port);

        const auto result =
            p0::run_attempt(table_w1_w2(), env_, baseline, port);

        EXPECT_EQ(result.outcome, AttemptOutcome::kTrialFailed);
        EXPECT_EQ(result.reason, item.reason);
        EXPECT_EQ(port.count("clear"), 0u);
        EXPECT_EQ(port.count("tracking"), 0u);
        EXPECT_EQ(port.count("faults"), 0u);
        EXPECT_FALSE(baseline.has_table);
    }
}

TEST_F(Phase0AttemptTest, CleanupTimeoutFailsTheTrial)
{
    activate(table_w1_w2());
    port_.never_clear = true;
    Table next = make_table("t2");
    add_slot(next, "W1", "taskA", 0);

    const uint64_t applied_at = kEpoch + 30 * 1000000ULL + kApplyDelayNs;
    const auto result = attempt(next);

    EXPECT_EQ(result.outcome, AttemptOutcome::kTrialFailed);
    EXPECT_EQ(result.reason, "the old BPF bank is still referenced");
    EXPECT_EQ(port_.count("clear"), port_.count("sleep") + 1);
    EXPECT_GE(port_.now - applied_at, p0::kOldBankCleanupTimeoutNs);
    EXPECT_LT(port_.now - applied_at,
              p0::kOldBankCleanupTimeoutNs + p0::kOldBankCleanupPollNs);
    EXPECT_EQ(port_.count("tracking"), 0u);
    EXPECT_EQ(port_.count("faults"), 0u);
    expect_baseline_workloads({"W1", "W2"});
}

// ---------------------------------------------------------------------------
// Fault limits
// ---------------------------------------------------------------------------

TEST_F(Phase0AttemptTest, FaultLimitsFollowTheLegacyRule)
{
    Table table = make_table("t1");
    // Table order differs from the sorted slot order on purpose.
    add_slot(table, "W1", "taskA", 5000, 7);
    add_slot(table, "W1", "taskA", 0, 3);
    add_slot(table, "W2", "taskB", 2000, 0);

    const auto result = attempt(table);

    ASSERT_EQ(result.outcome, AttemptOutcome::kApplied) << result.log;
    const p0::FaultLimits expected = {{{fnv1a("W1"), fnv1a("taskA")}, 3u}};
    EXPECT_EQ(port_.limits, expected);
}

}  // namespace
