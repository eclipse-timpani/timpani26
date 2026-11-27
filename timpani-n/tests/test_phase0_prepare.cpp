// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// DDR-011 Phase 0 P0-T02: node pre-checks, rejection without side effects
// and NPrepared assembly.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "phase0_prepare.h"

namespace p0 = timpani::node::phase0;
using p0::Table;
using timpani::node::v1::TtSlot;

namespace {

constexpr uint64_t kEpoch = 1000000000ULL;
constexpr uint32_t kHyperperiodUs = 10000;
constexpr uint64_t kHyperperiodNs = kHyperperiodUs * 1000ULL;
constexpr uint32_t kCpu = 2;
constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();
constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();
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

Table make_table(const std::string& table_id = "t1")
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

TtSlot* add_slot(Table& table, const std::string& workload,
                 const std::string& task, uint32_t offset_us,
                 uint32_t duration_us = 100, uint32_t cpu = kCpu)
{
    auto* slot = table.mutable_partitions(0)->mutable_layers(0)->add_tt_slots();
    slot->set_workload_id(workload);
    slot->set_task_id(task);
    slot->set_offset_us(offset_us);
    slot->set_duration_us(duration_us);
    slot->set_deadline_us(duration_us);
    slot->set_cpu(cpu);
    slot->set_workload_id_hash(fnv1a(workload));
    slot->set_task_id_hash(fnv1a(task));
    slot->set_current_limit(3);
    return slot;
}

std::string task_name(size_t index)
{
    return "t" + std::to_string(100 + index);
}

bool same_records(const std::map<uint64_t, p0::TaskRecord>& a,
                  const std::map<uint64_t, p0::TaskRecord>& b)
{
    if (a.size() != b.size()) return false;
    for (auto ia = a.begin(), ib = b.begin(); ia != a.end(); ++ia, ++ib) {
        const p0::TaskRecord& x = ia->second;
        const p0::TaskRecord& y = ib->second;
        if (ia->first != ib->first || x.task_id != y.task_id ||
            x.workload_id != y.workload_id ||
            x.workload_id_hash != y.workload_id_hash || x.pid != y.pid ||
            x.assigned_cpu != y.assigned_cpu || x.shm_index != y.shm_index) {
            return false;
        }
    }
    return true;
}

bool same_slots(const std::vector<p0::PreparedSlot>& a,
                const std::vector<p0::PreparedSlot>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (std::tie(x.offset_us, x.duration_us, x.deadline_us, x.offset_ns,
                     x.duration_ns, x.cpu, x.slot_idx, x.workload_id_hash,
                     x.task_id_hash, x.current_limit, x.layer_index) !=
            std::tie(y.offset_us, y.duration_us, y.deadline_us, y.offset_ns,
                     y.duration_ns, y.cpu, y.slot_idx, y.workload_id_hash,
                     y.task_id_hash, y.current_limit, y.layer_index)) {
            return false;
        }
    }
    return true;
}

bool same_baseline(const p0::ActiveBaseline& a, const p0::ActiveBaseline& b)
{
    return a.has_table == b.has_table && a.epoch_ns == b.epoch_ns &&
           a.hyperperiod_us == b.hyperperiod_us && a.has_cpu == b.has_cpu &&
           a.cpu == b.cpu && a.cpus == b.cpus && same_records(a.tasks, b.tasks) &&
           a.workloads == b.workloads && same_slots(a.slots, b.slots);
}

std::string shm_name(const p0::NPrepared& prepared, size_t index)
{
    return std::string(prepared.shm.entries[index].name.data());
}

class Phase0PrepareTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        env_.node_id = kNode;
        env_.isolated_cpu_mask = (1ULL << 2) | (1ULL << 3);
        env_.now_ns = kEpoch + 25 * 1000000ULL;  // 2.5 hyperperiods
        env_.shm_generation = 4;
        env_.resolve_pids = [this](const std::set<std::string>& names) {
            ++resolver_calls_;
            std::map<std::string, std::vector<int32_t>> found;
            for (const auto& name : names) {
                auto it = pids_.find(name);
                if (it != pids_.end()) found[name] = it->second;
            }
            return found;
        };
        pids_ = {{"taskA", {101}},
                 {"taskB", {102}},
                 {"taskC", {103}},
                 {"taskD", {104}},
                 {"taskE", {105}}};
    }

    bool prepare(const Table& table, p0::NPrepared& out, std::string& error)
    {
        return p0::prepare_table(table, active_, env_, out, error);
    }

    p0::NPrepared accept(const Table& table)
    {
        p0::NPrepared out;
        std::string error;
        EXPECT_TRUE(prepare(table, out, error)) << error;
        return out;
    }

    void activate(const Table& table)
    {
        active_ = p0::baseline_from(accept(table));
    }

    // A rejected table must leave both the candidate and the baseline intact.
    void expect_reject(const Table& table, const std::string& reason)
    {
        p0::NPrepared out;
        out.table_id = "sentinel";
        const p0::ActiveBaseline before = active_;
        std::string error;
        EXPECT_FALSE(prepare(table, out, error));
        EXPECT_NE(error.find(reason), std::string::npos) << error;
        EXPECT_EQ(out.table_id, "sentinel");
        EXPECT_TRUE(out.slots.empty());
        EXPECT_TRUE(out.tasks.empty());
        EXPECT_TRUE(same_baseline(before, active_));
    }

    p0::ActiveBaseline active_;
    p0::NodeEnvironment env_;
    std::map<std::string, std::vector<int32_t>> pids_;
    int resolver_calls_ = 0;
};

// ---------------------------------------------------------------------------
// Valid candidate
// ---------------------------------------------------------------------------

TEST_F(Phase0PrepareTest, ValidTableBuildsSortedCandidate)
{
    Table table = make_table("t-valid");
    add_slot(table, "wlA", "taskB", 5000, 1000);
    add_slot(table, "wlA", "taskA", 0, 1000);
    add_slot(table, "wlB", "taskC", 2000, 500);
    add_slot(table, "wlA", "taskA", 7000, 1000);

    const p0::NPrepared prepared = accept(table);
    EXPECT_EQ(prepared.table_id, "t-valid");
    EXPECT_EQ(prepared.epoch_ns, kEpoch);
    EXPECT_EQ(prepared.hyperperiod_us, kHyperperiodUs);
    EXPECT_TRUE(prepared.has_cpu);
    EXPECT_EQ(prepared.cpu, kCpu);

    ASSERT_EQ(prepared.slots.size(), 4u);
    const std::vector<std::pair<uint32_t, uint32_t>> expected = {
        {0, 1}, {2000, 2}, {5000, 0}, {7000, 3}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(prepared.slots[i].offset_us, expected[i].first);
        EXPECT_EQ(prepared.slots[i].offset_ns, expected[i].first * 1000ULL);
        EXPECT_EQ(prepared.slots[i].slot_idx, expected[i].second);
        EXPECT_EQ(prepared.slots[i].cpu, kCpu);
        EXPECT_EQ(prepared.slots[i].current_limit, 3u);
        EXPECT_EQ(prepared.slots[i].layer_index, 1);
    }
    EXPECT_EQ(prepared.slots[1].duration_ns, 500000u);
    EXPECT_EQ(prepared.slots[1].task_id_hash, fnv1a("taskC"));
    EXPECT_EQ(prepared.slots[1].workload_id_hash, fnv1a("wlB"));

    ASSERT_EQ(prepared.tasks.size(), 3u);
    const auto& task_a = prepared.tasks.at(fnv1a("taskA"));
    EXPECT_EQ(task_a.task_id, "taskA");
    EXPECT_EQ(task_a.workload_id, "wlA");
    EXPECT_EQ(task_a.workload_id_hash, fnv1a("wlA"));
    EXPECT_EQ(task_a.pid, 101);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskC")).pid, 103);
    EXPECT_EQ(prepared.workloads.size(), 2u);
    EXPECT_EQ(prepared.workloads.at(fnv1a("wlB")), "wlB");

    // SHM indexes follow first appearance in the sorted slots.
    EXPECT_EQ(task_a.shm_index, 0u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskC")).shm_index, 1u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskB")).shm_index, 2u);
    EXPECT_EQ(prepared.shm.n_tasks, 3u);
    EXPECT_EQ(shm_name(prepared, 0), "taskA");
    EXPECT_EQ(shm_name(prepared, 1), "taskC");
    EXPECT_EQ(shm_name(prepared, 2), "taskB");
    EXPECT_EQ(prepared.shm.entries[1].task_id_hash, fnv1a("taskC"));
    EXPECT_FALSE(prepared.shm.entries[3].occupied);
    EXPECT_EQ(prepared.shm.base_generation, 4u);
    EXPECT_EQ(prepared.shm.next_generation, 6u);

    EXPECT_EQ(prepared.boundary.index, 3u);
    EXPECT_EQ(prepared.boundary.target_ns, kEpoch + 3 * kHyperperiodNs);
    EXPECT_EQ(prepared.boundary.hyperperiod_ns, kHyperperiodNs);
    EXPECT_TRUE(prepared.removed_tasks.empty());
    EXPECT_TRUE(prepared.removed_workloads.empty());
    EXPECT_EQ(resolver_calls_, 1);
}

TEST_F(Phase0PrepareTest, SuccessReplacesStaleCandidate)
{
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);
    p0::NPrepared out;
    out.removed_tasks[1] = p0::TaskRecord{};
    out.slots.resize(5);
    std::string error;
    ASSERT_TRUE(prepare(table, out, error)) << error;
    EXPECT_EQ(out.slots.size(), 1u);
    EXPECT_TRUE(out.removed_tasks.empty());
}

TEST_F(Phase0PrepareTest, AcceptsOneIsolatedCpuPerPartition)
{
    Table table = make_table("t-multi-cpu");
    add_slot(table, "wlA", "taskA", 0);
    auto* partition = table.add_partitions();
    partition->set_partition_id("p1");
    partition->mutable_cpuset()->add_cpus(3);
    partition->mutable_cpuset()->set_isolated(true);
    auto* layer = partition->add_layers();
    layer->set_layer_index(2);
    layer->set_model(timpani::node::v1::TIME_TRIGGERED);
    auto* slot = layer->add_tt_slots();
    slot->set_workload_id("wlB");
    slot->set_task_id("taskB");
    slot->set_offset_us(5000);
    slot->set_duration_us(100);
    slot->set_deadline_us(100);
    slot->set_cpu(3);
    slot->set_workload_id_hash(fnv1a("wlB"));
    slot->set_task_id_hash(fnv1a("taskB"));
    slot->set_current_limit(3);

    const p0::NPrepared prepared = accept(table);
    EXPECT_EQ(prepared.cpus, (std::set<uint32_t>{2, 3}));
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskA")).assigned_cpu, 2u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskB")).assigned_cpu, 3u);
}

TEST_F(Phase0PrepareTest, BaselineFromCopiesAcceptedState)
{
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);
    add_slot(table, "wlA", "taskA", 5000);
    p0::NPrepared prepared = accept(table);
    const p0::ActiveBaseline active = p0::baseline_from(prepared);
    EXPECT_TRUE(active.has_table);
    EXPECT_EQ(active.epoch_ns, kEpoch);
    EXPECT_EQ(active.hyperperiod_us, kHyperperiodUs);
    EXPECT_TRUE(active.has_cpu);
    EXPECT_EQ(active.cpu, kCpu);
    EXPECT_TRUE(same_records(active.tasks, prepared.tasks));
    EXPECT_EQ(active.workloads, prepared.workloads);
    ASSERT_TRUE(same_slots(active.slots, prepared.slots));
    prepared.slots.front().duration_us += 1;
    EXPECT_NE(active.slots.front().duration_us,
              prepared.slots.front().duration_us);
}

TEST_F(Phase0PrepareTest, AcceptsMaximumIdentifierLength)
{
    const std::string id(p0::kMaxIdBytes, 'x');
    pids_[id] = {200};
    Table table = make_table();
    add_slot(table, id, id, 0);
    const p0::NPrepared prepared = accept(table);
    EXPECT_EQ(shm_name(prepared, 0), id);
}

// ---------------------------------------------------------------------------
// Table and node level rejections
// ---------------------------------------------------------------------------

TEST_F(Phase0PrepareTest, RejectsEmptyTableId)
{
    Table table = make_table("");
    add_slot(table, "wlA", "taskA", 0);
    expect_reject(table, "empty table_id");
}

TEST_F(Phase0PrepareTest, RejectsNodeIdMismatch)
{
    Table table = make_table();
    table.set_node_id("node-b");
    add_slot(table, "wlA", "taskA", 0);
    expect_reject(table, "node_id mismatch");
}

TEST_F(Phase0PrepareTest, RejectsZeroHyperperiod)
{
    Table table = make_table();
    table.set_hyperperiod_us(0);
    expect_reject(table, "hyperperiod_us must be positive");
}

TEST_F(Phase0PrepareTest, RejectsEpochOrHyperperiodChange)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    activate(first);

    Table epoch = make_table("t2");
    epoch.set_epoch_ns(kEpoch + 1);
    add_slot(epoch, "wlA", "taskA", 0);
    expect_reject(epoch, "epoch_ns differs");

    Table period = make_table("t3");
    period.set_hyperperiod_us(kHyperperiodUs * 2);
    add_slot(period, "wlA", "taskA", 0);
    expect_reject(period, "hyperperiod_us differs");
}

TEST_F(Phase0PrepareTest, RejectsNonTtLayer)
{
    Table table = make_table();
    table.mutable_partitions(0)->mutable_layers(0)->set_model(
        timpani::node::v1::EVENT_DRIVEN);
    expect_reject(table, "non-TT layer");
}

TEST_F(Phase0PrepareTest, RejectsCbsEntries)
{
    Table table = make_table();
    auto* cbs =
        table.mutable_partitions(0)->mutable_layers(0)->add_cbs_entries();
    cbs->set_workload_id("wlA");
    cbs->set_task_id("taskA");
    expect_reject(table, "CBS entries");
}

TEST_F(Phase0PrepareTest, RejectsMultiCpuPartitionsAndSlotCpuMismatch)
{
    Table slots = make_table();
    add_slot(slots, "wlA", "taskA", 0, 100, 3);
    expect_reject(slots, "slot CPU differs from partition");

    Table cpuset = make_table();
    cpuset.mutable_partitions(0)->mutable_cpuset()->add_cpus(3);
    add_slot(cpuset, "wlA", "taskA", 0);
    expect_reject(cpuset, "one isolated CPU");
}

TEST_F(Phase0PrepareTest, RejectsNonIsolatedCpu)
{
    Table slot_cpu = make_table();
    slot_cpu.mutable_partitions(0)->mutable_cpuset()->set_cpus(0, 5);
    add_slot(slot_cpu, "wlA", "taskA", 0, 100, 5);
    expect_reject(slot_cpu, "CPU 5 is not isolated");

    Table partition_cpu = make_table();
    partition_cpu.mutable_partitions(0)->mutable_cpuset()->set_cpus(0, 5);
    expect_reject(partition_cpu, "CPU 5 is not isolated");
}

TEST_F(Phase0PrepareTest, RejectsCpuOutsideMaskWidth)
{
    env_.isolated_cpu_mask = kU64Max;
    Table table = make_table();
    table.mutable_partitions(0)->mutable_cpuset()->set_cpus(0, p0::kMaxCpus);
    add_slot(table, "wlA", "taskA", 0, 100, p0::kMaxCpus);
    expect_reject(table, "is not isolated");
}

TEST_F(Phase0PrepareTest, RejectsCpuChangeFromActive)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    activate(first);

    Table moved = make_table("t2");
    moved.mutable_partitions(0)->mutable_cpuset()->set_cpus(0, 3);
    add_slot(moved, "wlA", "taskA", 0, 100, 3);
    expect_reject(moved, "retained workload slot parameters changed");
}

TEST_F(Phase0PrepareTest, RejectsBoundaryOutsideUint64Range)
{
    Table table = make_table();
    table.set_epoch_ns(kU64Max - 1000);  // T = E > UINT64_MAX - H
    add_slot(table, "wlA", "taskA", 0);
    expect_reject(table, "uint64 time range");
}

TEST_F(Phase0PrepareTest, ShmGenerationNeedsTwoStepsOfHeadroom)
{
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);

    env_.shm_generation = kU32Max - 1;
    expect_reject(table, "SHM generation would wrap");
    env_.shm_generation = kU32Max;
    expect_reject(table, "SHM generation would wrap");

    env_.shm_generation = kU32Max - 2;
    const p0::NPrepared prepared = accept(table);
    EXPECT_EQ(prepared.shm.next_generation, kU32Max);
}

// ---------------------------------------------------------------------------
// Slot level rejections
// ---------------------------------------------------------------------------

TEST_F(Phase0PrepareTest, SlotMustEndWithinHyperperiod)
{
    Table over = make_table();
    add_slot(over, "wlA", "taskA", kHyperperiodUs - 50, 51);
    expect_reject(over, "slot end exceeds hyperperiod");

    Table exact = make_table();
    add_slot(exact, "wlA", "taskA", kHyperperiodUs - 50, 50);
    accept(exact);
}

TEST_F(Phase0PrepareTest, SlotEndDoesNotWrapUint32)
{
    Table wrap = make_table();
    wrap.set_hyperperiod_us(kU32Max);
    add_slot(wrap, "wlA", "taskA", kU32Max - 10, 20);
    expect_reject(wrap, "slot end exceeds hyperperiod");

    Table edge = make_table();
    edge.set_hyperperiod_us(kU32Max);
    add_slot(edge, "wlA", "taskA", kU32Max - 20, 20);
    accept(edge);
}

TEST_F(Phase0PrepareTest, RejectsZeroDurationOrDeadline)
{
    Table duration = make_table();
    add_slot(duration, "wlA", "taskA", 0, 0);
    expect_reject(duration, "duration/deadline must be > 0");

    Table deadline = make_table();
    add_slot(deadline, "wlA", "taskA", 0)->set_deadline_us(0);
    expect_reject(deadline, "duration/deadline must be > 0");
}

TEST_F(Phase0PrepareTest, RejectsInvalidIdentifiers)
{
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"", "taskA"},
        {"wlA", ""},
        {std::string(p0::kMaxIdBytes + 1, 'w'), "taskA"},
        {"wlA", std::string(p0::kMaxIdBytes + 1, 't')},
        {"wlA", "t\xc3\xa1sk"},
        {"w\x80", "taskA"},
    };
    for (const auto& ids : bad) {
        Table table = make_table();
        add_slot(table, ids.first, ids.second, 0);
        expect_reject(table, "IDs must be 1..15-byte ASCII");
    }
}

TEST_F(Phase0PrepareTest, RejectsHashConflictsWithinTable)
{
    Table workload = make_table();
    add_slot(workload, "wlA", "taskA", 0);
    add_slot(workload, "wlA", "taskB", 100)->set_workload_id_hash(7);
    expect_reject(workload, "workload ID/hash conflict");

    Table shared_workload_hash = make_table();
    add_slot(shared_workload_hash, "wlA", "taskA", 0);
    add_slot(shared_workload_hash, "wlB", "taskB", 100)
        ->set_workload_id_hash(fnv1a("wlA"));
    expect_reject(shared_workload_hash, "workload ID/hash conflict");

    Table task = make_table();
    add_slot(task, "wlA", "taskA", 0);
    add_slot(task, "wlA", "taskA", 100)->set_task_id_hash(7);
    expect_reject(task, "task ID/hash conflict");

    Table shared_task_hash = make_table();
    add_slot(shared_task_hash, "wlA", "taskA", 0);
    add_slot(shared_task_hash, "wlA", "taskB", 100)
        ->set_task_id_hash(fnv1a("taskA"));
    expect_reject(shared_task_hash, "task ID/hash conflict");
}

TEST_F(Phase0PrepareTest, RejectsTaskInMultipleWorkloads)
{
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);
    add_slot(table, "wlB", "taskA", 100);
    expect_reject(table, "task belongs to multiple workloads");
}

TEST_F(Phase0PrepareTest, RejectsIdentityConflictsWithActive)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    activate(first);

    Table task_hash = make_table("t2");
    add_slot(task_hash, "wlA", "taskA", 0)->set_task_id_hash(7);
    expect_reject(task_hash, "task ID/hash conflicts with the active table");

    Table task_id = make_table("t3");
    add_slot(task_id, "wlA", "taskB", 0)->set_task_id_hash(fnv1a("taskA"));
    expect_reject(task_id, "task ID/hash conflicts with the active table");

    Table workload_hash = make_table("t4");
    add_slot(workload_hash, "wlA", "taskB", 0)->set_workload_id_hash(7);
    expect_reject(workload_hash,
                  "workload ID/hash conflicts with the active table");

    Table moved = make_table("t5");
    add_slot(moved, "wlB", "taskA", 0);
    expect_reject(moved, "task workload differs from the active table");
}

TEST_F(Phase0PrepareTest, RejectsRetainedSlotTimingAndLimitChanges)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlA", "taskA", 5000);
    activate(first);
    const int calls = resolver_calls_;

    using Setter = void (TtSlot::*)(uint32_t);
    const std::pair<const char*, Setter> changes[] = {
        {"offset", &TtSlot::set_offset_us},
        {"duration", &TtSlot::set_duration_us},
        {"deadline", &TtSlot::set_deadline_us},
        {"current_limit", &TtSlot::set_current_limit},
    };
    for (const auto& change : changes) {
        SCOPED_TRACE(change.first);
        Table second = first;
        second.set_table_id("t2");
        auto* slot =
            second.mutable_partitions(0)->mutable_layers(0)->mutable_tt_slots(
                0);
        (slot->*change.second)(101);
        expect_reject(second, "retained workload slot parameters changed: wlA");
        EXPECT_EQ(resolver_calls_, calls);
    }
}

TEST_F(Phase0PrepareTest, RejectsRetainedSlotLayerChanges)
{
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);
    activate(table);
    const int calls = resolver_calls_;

    table.set_table_id("t2");
    table.mutable_partitions(0)->mutable_layers(0)->set_layer_index(2);
    expect_reject(table, "retained workload slot parameters changed: wlA");
    EXPECT_EQ(resolver_calls_, calls);
}

TEST_F(Phase0PrepareTest, RejectsChangedRetainedSlotOccurrences)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlA", "taskA", 5000);
    activate(first);
    const int calls = resolver_calls_;

    Table moved = first;
    moved.set_table_id("t-moved");
    moved.mutable_partitions(0)
        ->mutable_layers(0)
        ->mutable_tt_slots(1)
        ->set_offset_us(6000);
    expect_reject(moved, "retained workload slot parameters changed: wlA");

    Table removed = first;
    removed.set_table_id("t-removed");
    removed.mutable_partitions(0)
        ->mutable_layers(0)
        ->mutable_tt_slots()
        ->RemoveLast();
    expect_reject(removed, "retained workload slot parameters changed: wlA");

    Table duplicated = first;
    duplicated.set_table_id("t-duplicated");
    add_slot(duplicated, "wlA", "taskA", 0);
    expect_reject(duplicated, "retained workload slot parameters changed: wlA");

    Table substituted = first;
    substituted.set_table_id("t-substituted");
    substituted.mutable_partitions(0)
        ->mutable_layers(0)
        ->mutable_tt_slots(1)
        ->set_offset_us(0);
    expect_reject(substituted,
                  "retained workload slot parameters changed: wlA");
    EXPECT_EQ(resolver_calls_, calls);
}

TEST_F(Phase0PrepareTest, RejectsTaskSetChangesWithinRetainedWorkload)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlA", "taskB", 2000);
    activate(first);
    const int calls = resolver_calls_;

    Table added = first;
    added.set_table_id("t-added");
    add_slot(added, "wlA", "taskC", 4000);
    expect_reject(added, "retained workload slot parameters changed: wlA");

    Table removed = first;
    removed.set_table_id("t-removed");
    removed.mutable_partitions(0)
        ->mutable_layers(0)
        ->mutable_tt_slots()
        ->RemoveLast();
    expect_reject(removed, "retained workload slot parameters changed: wlA");

    Table replaced = first;
    replaced.set_table_id("t-replaced");
    auto* slot =
        replaced.mutable_partitions(0)->mutable_layers(0)->mutable_tt_slots(1);
    slot->set_task_id("taskC");
    slot->set_task_id_hash(fnv1a("taskC"));
    expect_reject(replaced, "retained workload slot parameters changed: wlA");
    EXPECT_EQ(resolver_calls_, calls);
}

TEST_F(Phase0PrepareTest, AcceptsReorderedRetainedSlotsIncludingTies)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 5000);
    add_slot(first, "wlA", "taskA", 0, 100);
    add_slot(first, "wlA", "taskA", 0, 50);
    activate(first);
    const p0::ActiveBaseline before = active_;

    Table second = first;
    second.set_table_id("t2");
    auto* slots =
        second.mutable_partitions(0)->mutable_layers(0)->mutable_tt_slots();
    slots->SwapElements(0, 2);
    slots->SwapElements(1, 2);
    const std::string snapshot = second.SerializeAsString();
    const auto prepared = accept(second);
    EXPECT_EQ(second.SerializeAsString(), snapshot);
    ASSERT_EQ(prepared.slots.size(), active_.slots.size());
    EXPECT_NE(prepared.slots[0].slot_idx, active_.slots[0].slot_idx);
    EXPECT_TRUE(same_baseline(before, active_));
}

TEST_F(Phase0PrepareTest, WholeWorkloadUpdatesPreserveRetainedSlotParameters)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlA", "taskA", 5000);
    add_slot(first, "wlB", "taskB", 1000);
    activate(first);

    Table added = first;
    added.set_table_id("t-added");
    add_slot(added, "wlC", "taskC", 2000);
    added.mutable_partitions(0)
        ->mutable_layers(0)
        ->mutable_tt_slots()
        ->SwapElements(0, 3);
    activate(added);
    ASSERT_EQ(active_.slots.size(), 4u);

    Table removed = make_table("t-removed");
    add_slot(removed, "wlC", "taskC", 2000);
    add_slot(removed, "wlA", "taskA", 5000);
    add_slot(removed, "wlA", "taskA", 0);
    auto prepared = accept(removed);
    EXPECT_EQ(prepared.removed_workloads.count(fnv1a("wlB")), 1u);
    EXPECT_EQ(prepared.removed_tasks.count(fnv1a("taskB")), 1u);
    active_ = p0::baseline_from(prepared);

    Table replaced = make_table("t-replaced");
    add_slot(replaced, "wlD", "taskD", 3000);
    add_slot(replaced, "wlA", "taskA", 0);
    add_slot(replaced, "wlA", "taskA", 5000);
    prepared = accept(replaced);
    EXPECT_EQ(prepared.removed_workloads.count(fnv1a("wlC")), 1u);
    active_ = p0::baseline_from(prepared);

    Table clear = make_table("t-clear");
    clear.clear_partitions();
    prepared = accept(clear);
    EXPECT_EQ(prepared.removed_workloads.size(), 2u);
    active_ = p0::baseline_from(prepared);
    EXPECT_TRUE(active_.slots.empty());
    EXPECT_TRUE(active_.workloads.empty());
    EXPECT_EQ(active_.epoch_ns, kEpoch);
    EXPECT_EQ(active_.hyperperiod_us, kHyperperiodUs);

    first.set_table_id("t-after-clear");
    activate(first);
    EXPECT_EQ(active_.slots.size(), 3u);
}

TEST_F(Phase0PrepareTest, EnforcesSlotCapacity)
{
    Table over = make_table();
    for (uint32_t i = 0; i <= p0::kMaxTtSlots; ++i) {
        add_slot(over, "wlA", "taskA", i * 4, 2);
    }
    expect_reject(over, "TT slot capacity exceeds 2048");
    EXPECT_EQ(resolver_calls_, 0);

    Table full = make_table();
    for (uint32_t i = 0; i < p0::kMaxTtSlots; ++i) {
        add_slot(full, "wlA", "taskA", i * 4, 2);
    }
    EXPECT_EQ(accept(full).slots.size(), p0::kMaxTtSlots);
}

TEST_F(Phase0PrepareTest, EnforcesTaskCapacity)
{
    Table over = make_table();
    for (size_t i = 0; i <= p0::kMaxTasks; ++i) {
        pids_[task_name(i)] = {static_cast<int32_t>(1000 + i)};
        add_slot(over, "wlA", task_name(i), static_cast<uint32_t>(i * 100));
    }
    expect_reject(over, "unique TT task capacity exceeds 32");
    EXPECT_EQ(resolver_calls_, 0);

    Table full = make_table();
    for (size_t i = 0; i < p0::kMaxTasks; ++i) {
        add_slot(full, "wlA", task_name(i), static_cast<uint32_t>(i * 100));
    }
    const p0::NPrepared prepared = accept(full);
    EXPECT_EQ(prepared.tasks.size(), p0::kMaxTasks);
    EXPECT_EQ(prepared.shm.n_tasks, p0::kMaxTasks);
}

// ---------------------------------------------------------------------------
// PID resolution
// ---------------------------------------------------------------------------

TEST_F(Phase0PrepareTest, RejectsUnresolvedTask)
{
    Table table = make_table();
    add_slot(table, "wlA", "ghost", 0);
    expect_reject(table, "task not found: ghost");

    pids_["empty"] = {};
    Table empty = make_table();
    add_slot(empty, "wlA", "empty", 0);
    expect_reject(empty, "task not found: empty");
}

TEST_F(Phase0PrepareTest, RejectsAmbiguousOrSharedPid)
{
    pids_["taskA"] = {101, 111};
    Table multiple = make_table();
    add_slot(multiple, "wlA", "taskA", 0);
    expect_reject(multiple, "task resolves to multiple PIDs");

    pids_["taskA"] = {101};
    pids_["taskB"] = {101};
    Table shared = make_table();
    add_slot(shared, "wlA", "taskA", 0);
    add_slot(shared, "wlA", "taskB", 100);
    expect_reject(shared, "task PID is invalid or shared");

    pids_["taskA"] = {0};
    Table invalid = make_table();
    add_slot(invalid, "wlA", "taskA", 0);
    expect_reject(invalid, "task PID is invalid or shared");
}

TEST_F(Phase0PrepareTest, RejectsRetainedTaskWithNewPid)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    activate(first);

    pids_["taskA"] = {201};
    Table second = make_table("t2");
    add_slot(second, "wlA", "taskA", 0);
    expect_reject(second, "retained task PID changed");
}

TEST_F(Phase0PrepareTest, RejectsMissingResolver)
{
    env_.resolve_pids = nullptr;
    Table table = make_table();
    add_slot(table, "wlA", "taskA", 0);
    expect_reject(table, "PID resolver unavailable");
}

// ---------------------------------------------------------------------------
// SHM index rules, removal sets and CLEAR
// ---------------------------------------------------------------------------

TEST_F(Phase0PrepareTest, RetainedTasksKeepShmIndexAndNewTasksAvoidRemoved)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlB", "taskB", 1000);
    add_slot(first, "wlA", "taskC", 2000);
    activate(first);
    EXPECT_EQ(active_.tasks.at(fnv1a("taskB")).shm_index, 1u);

    // Reordered, taskB removed, taskD/taskE added.
    Table second = make_table("t2");
    add_slot(second, "wlC", "taskE", 1000);
    add_slot(second, "wlA", "taskC", 2000);
    add_slot(second, "wlC", "taskD", 3000);
    add_slot(second, "wlA", "taskA", 0);
    const p0::NPrepared prepared = accept(second);

    EXPECT_EQ(prepared.tasks.at(fnv1a("taskA")).shm_index, 0u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskC")).shm_index, 2u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskE")).shm_index, 3u);
    EXPECT_EQ(prepared.tasks.at(fnv1a("taskD")).shm_index, 4u);
    EXPECT_FALSE(prepared.shm.entries[1].occupied);
    EXPECT_EQ(prepared.shm.entries[1].name[0], '\0');
    EXPECT_EQ(prepared.shm.n_tasks, 5u);

    ASSERT_EQ(prepared.removed_tasks.size(), 1u);
    const auto& removed = prepared.removed_tasks.at(fnv1a("taskB"));
    EXPECT_EQ(removed.task_id, "taskB");
    EXPECT_EQ(removed.pid, 102);
    EXPECT_EQ(removed.shm_index, 1u);
    ASSERT_EQ(prepared.removed_workloads.size(), 1u);
    EXPECT_EQ(prepared.removed_workloads.at(fnv1a("wlB")), "wlB");
}

TEST_F(Phase0PrepareTest, RemovedIndexIsReusedOnlyWhenNecessary)
{
    const size_t retained = p0::kMaxTasks - 1;
    Table first = make_table("t1");
    for (size_t i = 0; i < retained; ++i) {
        pids_[task_name(i)] = {static_cast<int32_t>(1000 + i)};
        add_slot(first, task_name(i), task_name(i),
                 static_cast<uint32_t>(i * 100));
    }
    activate(first);

    Table second = make_table("t2");
    pids_["newA"] = {3000};
    pids_["newB"] = {3001};
    add_slot(second, "new", "newA", 0);
    add_slot(second, "new", "newB", 50);
    for (size_t i = 0; i < retained; ++i) {
        if (i == 5) continue;
        add_slot(second, task_name(i), task_name(i),
                 static_cast<uint32_t>(i * 100));
    }
    const p0::NPrepared prepared = accept(second);

    EXPECT_EQ(prepared.tasks.at(fnv1a("newA")).shm_index, retained);
    EXPECT_EQ(prepared.tasks.at(fnv1a("newB")).shm_index, 5u);
    EXPECT_EQ(shm_name(prepared, 5), "newB");
    EXPECT_EQ(prepared.shm.n_tasks, p0::kMaxTasks);
    EXPECT_EQ(prepared.removed_tasks.count(fnv1a(task_name(5))), 1u);
}

TEST_F(Phase0PrepareTest, ClearKeepsCpuAndSkipsPidResolution)
{
    Table first = make_table("t1");
    add_slot(first, "wlA", "taskA", 0);
    add_slot(first, "wlB", "taskB", 1000);
    activate(first);
    const int calls = resolver_calls_;

    Table clear = make_table("t-clear");
    clear.mutable_partitions(0)->mutable_cpuset()->clear_cpus();
    const p0::NPrepared prepared = accept(clear);

    EXPECT_EQ(resolver_calls_, calls);
    EXPECT_TRUE(prepared.slots.empty());
    EXPECT_TRUE(prepared.tasks.empty());
    EXPECT_EQ(prepared.shm.n_tasks, 0u);
    for (const auto& entry : prepared.shm.entries) {
        EXPECT_FALSE(entry.occupied);
    }
    EXPECT_TRUE(prepared.has_cpu);
    EXPECT_EQ(prepared.cpu, kCpu);
    EXPECT_EQ(prepared.removed_tasks.size(), 2u);
    EXPECT_EQ(prepared.removed_workloads.size(), 2u);
}

TEST_F(Phase0PrepareTest, FirstEmptyTableHasNoCpu)
{
    Table table = make_table();
    table.mutable_partitions(0)->mutable_cpuset()->clear_cpus();
    const p0::NPrepared prepared = accept(table);
    EXPECT_FALSE(prepared.has_cpu);
    EXPECT_EQ(resolver_calls_, 0);
}

TEST_F(Phase0PrepareTest, SortTieBreaksUseHashesThenSnapshotOrder)
{
    pids_["tX"] = {201};
    pids_["tY"] = {202};
    pids_["tZ"] = {203};
    Table table = make_table();
    auto set_hashes = [](TtSlot* slot, uint64_t workload, uint64_t task) {
        slot->set_workload_id_hash(workload);
        slot->set_task_id_hash(task);
    };
    set_hashes(add_slot(table, "wlB", "tX", 100), 2, 30);
    set_hashes(add_slot(table, "wlA", "tY", 100), 1, 10);
    set_hashes(add_slot(table, "wlA", "tZ", 100), 1, 5);
    set_hashes(add_slot(table, "wlA", "tY", 100), 1, 10);
    set_hashes(add_slot(table, "wlB", "tX", 0), 2, 30);

    const p0::NPrepared prepared = accept(table);
    ASSERT_EQ(prepared.slots.size(), 5u);
    const std::vector<uint32_t> order = {4, 2, 1, 3, 0};
    for (size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(prepared.slots[i].slot_idx, order[i]) << "position " << i;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TEST(Phase0Helpers, SlotFitsHyperperiodUses64BitSum)
{
    EXPECT_TRUE(p0::slot_fits_hyperperiod(0, 100, 100));
    EXPECT_FALSE(p0::slot_fits_hyperperiod(1, 100, 100));
    EXPECT_FALSE(p0::slot_fits_hyperperiod(kU32Max - 10, 20, kU32Max));
    EXPECT_TRUE(p0::slot_fits_hyperperiod(kU32Max - 20, 20, kU32Max));
}

TEST(Phase0Helpers, SelectBoundaryPicksNextGridPoint)
{
    p0::Boundary b;
    ASSERT_TRUE(p0::select_boundary(1000, 100, 999, b));
    EXPECT_EQ(b.index, 0u);
    EXPECT_EQ(b.target_ns, 1000u);

    ASSERT_TRUE(p0::select_boundary(1000, 100, 1000, b));
    EXPECT_EQ(b.index, 1u);
    EXPECT_EQ(b.target_ns, 1100u);

    ASSERT_TRUE(p0::select_boundary(1000, 100, 1099, b));
    EXPECT_EQ(b.index, 1u);
    ASSERT_TRUE(p0::select_boundary(1000, 100, 1100, b));
    EXPECT_EQ(b.index, 2u);
    EXPECT_EQ(b.target_ns, 1200u);
    EXPECT_EQ(b.epoch_ns, 1000u);
    EXPECT_EQ(b.hyperperiod_ns, 100u);
}

TEST(Phase0Helpers, SelectBoundaryRejectsOverflow)
{
    p0::Boundary b;
    b.target_ns = 42;
    EXPECT_FALSE(p0::select_boundary(0, 0, 0, b));
    EXPECT_FALSE(p0::select_boundary(0, 1, kU64Max, b));
    EXPECT_FALSE(p0::select_boundary(0, 2, kU64Max, b));
    EXPECT_EQ(b.target_ns, 42u);

    // Slot times in [T, T + H) must stay representable.
    const uint64_t h = 1000;
    ASSERT_TRUE(p0::select_boundary(kU64Max - h, h, 0, b));
    EXPECT_EQ(b.target_ns, kU64Max - h);
    EXPECT_FALSE(p0::select_boundary(kU64Max - h + 1, h, 0, b));
    ASSERT_TRUE(p0::select_boundary(kU64Max - 2 * h, h, kU64Max - 2 * h, b));
    EXPECT_EQ(b.target_ns, kU64Max - h);
    EXPECT_FALSE(p0::select_boundary(kU64Max - 2 * h, h, kU64Max - h, b));
}

TEST(Phase0Helpers, NextStableGenerationKeepsHeadroom)
{
    uint32_t next = 7;
    ASSERT_TRUE(p0::next_stable_generation(0, next));
    EXPECT_EQ(next, 2u);
    ASSERT_TRUE(p0::next_stable_generation(kU32Max - 2, next));
    EXPECT_EQ(next, kU32Max);
    next = 7;
    EXPECT_FALSE(p0::next_stable_generation(kU32Max - 1, next));
    EXPECT_FALSE(p0::next_stable_generation(kU32Max, next));
    EXPECT_EQ(next, 7u);
}

TEST(Phase0Helpers, ParseCpuList)
{
    uint64_t mask = 0;
    ASSERT_TRUE(p0::parse_cpu_list("2-3,5", mask));
    EXPECT_EQ(mask, (1ULL << 2) | (1ULL << 3) | (1ULL << 5));
    ASSERT_TRUE(p0::parse_cpu_list(" 1 , 4 - 5 \n", mask));
    EXPECT_EQ(mask, (1ULL << 1) | (1ULL << 4) | (1ULL << 5));
    ASSERT_TRUE(p0::parse_cpu_list("", mask));
    EXPECT_EQ(mask, 0u);
    ASSERT_TRUE(p0::parse_cpu_list("62-70", mask));
    EXPECT_EQ(mask, (1ULL << 62) | (1ULL << 63));
    ASSERT_TRUE(p0::parse_cpu_list("64", mask));
    EXPECT_EQ(mask, 0u);

    mask = 9;
    EXPECT_FALSE(p0::parse_cpu_list("abc", mask));
    EXPECT_FALSE(p0::parse_cpu_list("3-1", mask));
    EXPECT_FALSE(p0::parse_cpu_list("1-", mask));
    EXPECT_FALSE(p0::parse_cpu_list("-1", mask));
    EXPECT_EQ(mask, 9u);
}

// ---------------------------------------------------------------------------
// AttemptSlot
// ---------------------------------------------------------------------------

TEST(Phase0AttemptSlot, StaysBusyUntilFinish)
{
    p0::AttemptSlot slot;
    Table got;
    EXPECT_FALSE(slot.busy());
    ASSERT_TRUE(slot.offer(make_table("t1")));
    EXPECT_TRUE(slot.busy());
    EXPECT_FALSE(slot.offer(make_table("t2")));

    ASSERT_TRUE(slot.wait_take(got, std::chrono::milliseconds(0)));
    EXPECT_EQ(got.table_id(), "t1");
    EXPECT_TRUE(slot.busy());
    EXPECT_FALSE(slot.offer(make_table("t3")));
    EXPECT_FALSE(slot.wait_take(got, std::chrono::milliseconds(0)));

    slot.finish();
    EXPECT_FALSE(slot.busy());
    ASSERT_TRUE(slot.offer(make_table("t4")));
    ASSERT_TRUE(slot.wait_take(got, std::chrono::milliseconds(0)));
    EXPECT_EQ(got.table_id(), "t4");
}

TEST(Phase0AttemptSlot, WaitTakeTimesOut)
{
    p0::AttemptSlot slot;
    Table got;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(slot.wait_take(got, std::chrono::milliseconds(30)));
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(25));

    ASSERT_TRUE(slot.offer(make_table("t1")));
    slot.finish();  // drops an untaken table
    EXPECT_FALSE(slot.wait_take(got, std::chrono::milliseconds(0)));
}

TEST(Phase0AttemptSlot, OfferWakesWaitingThread)
{
    p0::AttemptSlot slot;
    Table got;
    bool taken = false;
    std::thread waiter(
        [&] { taken = slot.wait_take(got, std::chrono::seconds(5)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(slot.offer(make_table("t-wake")));
    waiter.join();
    EXPECT_TRUE(taken);
    EXPECT_EQ(got.table_id(), "t-wake");
}

}  // namespace
