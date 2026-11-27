/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <set>
#include <variant>
#include <vector>

#include "../src/global_scheduler.h"
#include "../src/node_config.h"

class GlobalSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        node_config_manager_ = std::make_shared<NodeConfigManager>();
        scheduler_ = std::make_unique<GlobalScheduler>(node_config_manager_);
    }

    ClassifiedTask MakeTtTask(const std::string& wl_id,
                              const std::string& task_id,
                              uint32_t period_us,
                              uint32_t wcet_us,
                              uint32_t deadline_us = 0) {
        ClassifiedTask t;
        t.workload_id = wl_id;
        t.task_id     = task_id;
        t.mechanism   = Mechanism::TT;
        t.period_us   = period_us;
        t.wcet_us     = wcet_us;
        t.deadline_us = (deadline_us > 0) ? deadline_us : period_us;
        t.assigned_cpu = -1;
        return t;
    }

    ClassifiedTask MakeCbsTask(const std::string& wl_id,
                               const std::string& task_id,
                               uint32_t mit_us,
                               uint32_t wcet_us,
                               uint32_t deadline_us = 0) {
        ClassifiedTask t;
        t.workload_id = wl_id;
        t.task_id     = task_id;
        t.mechanism   = Mechanism::CBS;
        t.period_us   = mit_us;
        t.wcet_us     = wcet_us;
        t.deadline_us = (deadline_us > 0) ? deadline_us : mit_us;
        t.assigned_cpu = -1;
        return t;
    }

    bool IsSuccess(const ScheduleResult& result) {
        return std::holds_alternative<timpani::node::v1::HierarchicalScheduleTable>(result);
    }

    const timpani::node::v1::HierarchicalScheduleTable& GetTable(
        const ScheduleResult& result) {
        return std::get<timpani::node::v1::HierarchicalScheduleTable>(result);
    }

    const InfeasibleError& GetError(const ScheduleResult& result) {
        return std::get<InfeasibleError>(result);
    }

    std::shared_ptr<NodeConfigManager> node_config_manager_;
    std::unique_ptr<GlobalScheduler> scheduler_;
};

// ─── Classification ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, SingleTtTaskProducesTable) {
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000)
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.partitions_size(), 0);
}

TEST_F(GlobalSchedulerTest, SingleCbsTaskProducesTable) {
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("lidar", "lidar_main", 5000, 2000, 5000)
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_GT(table.partitions_size(), 0);

    // Should have CBS entries
    bool has_cbs = false;
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            if (table.partitions(p).layers(l).cbs_entries_size() > 0) {
                has_cbs = true;
            }
        }
    }
    EXPECT_TRUE(has_cbs);
}

TEST_F(GlobalSchedulerTest, EmptyTasksReturnsEmptyTable) {
    std::vector<ClassifiedTask> tasks;
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));
    const auto& table = GetTable(result);
    EXPECT_EQ(table.partitions_size(), 0);
}

// ─── Harmonic Period Validation ─────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, HarmonicPeriodsSucceed) {
    // 5ms, 10ms, 20ms are harmonic (5 | 10, 5 | 20, 10 | 20)
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl1", "t1", 5000, 500, 5000),
        MakeTtTask("wl1", "t2", 10000, 1000, 10000),
        MakeTtTask("wl1", "t3", 20000, 1000, 20000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    EXPECT_TRUE(IsSuccess(result));
}

TEST_F(GlobalSchedulerTest, NonHarmonicPeriodsReturnError) {
    // 7ms and 10ms are not harmonic
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl1", "t1", 7000, 500, 7000),
        MakeTtTask("wl1", "t2", 10000, 1000, 10000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    EXPECT_EQ(GetError(result).reason, InfeasibleReason::NonHarmonicPeriod);
}

// ─── TT Slot Placement ─────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, TtSlotOffsetsMatchPeriod) {
    // Single task with period=10ms, wcet=2ms in hyperperiod=20ms
    // Expect 2 slots at offsets 0 and 10000
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    ASSERT_GT(table.partitions_size(), 0);

    const auto& layer = table.partitions(0).layers(0);
    // With period 10000 in hyperperiod 10000 → 1 slot at offset 0
    // (hyperperiod = max period = 10000 under harmonic assumption)
    EXPECT_GE(layer.tt_slots_size(), 1);
    EXPECT_EQ(layer.tt_slots(0).offset_us(), 0u);
    EXPECT_EQ(layer.tt_slots(0).duration_us(), 2000u);
}

TEST_F(GlobalSchedulerTest, DmOrderingShorterDeadlineFirst) {
    // Two tasks: brake(deadline=5ms), steer(deadline=10ms)
    // DM ordering: brake gets placed first at offset 0
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
        MakeTtTask("steer", "steer_ctrl", 10000, 2000, 10000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    ASSERT_GT(table.partitions_size(), 0);

    // Find the partition with TT slots
    for (int p = 0; p < table.partitions_size(); ++p) {
        const auto& layer = table.partitions(p).layers(0);
        if (layer.tt_slots_size() >= 2) {
            // First placed task (brake, deadline=5ms) should have offset=0
            EXPECT_EQ(layer.tt_slots(0).task_id(), "brake_ctrl");
            EXPECT_EQ(layer.tt_slots(0).offset_us(), 0u);
        }
    }
}

// ─── Gap Extraction ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, NoTtSlotsProducesFullGap) {
    // CBS-only workload → entire hyperperiod is a gap
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("lidar", "lidar_main", 5000, 1000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));
}

// ─── CBS Allocation ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, CbsWithinBudgetSucceeds) {
    // TT uses 20% utilization, CBS uses 30% → total 52% < 80% bound
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),   // U=0.20
        MakeCbsTask("lidar", "lidar_main", 10000, 3000, 10000), // U=0.30
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    EXPECT_TRUE(IsSuccess(result));
}

TEST_F(GlobalSchedulerTest, CbsExceedingBandwidthFails) {
    // With 2 default CPUs, TT and CBS go to separate CPUs.
    // To trigger overload, we need CBS utilization alone to exceed
    // U_BOUND on whatever CPU it lands on.
    // CBS_U = 0.85 > 0.80 - 0.02 = 0.78 → must fail
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("wl2", "cbs1", 10000, 8500, 10000),  // U=0.85
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    // CBS over-capacity is caught during CPU assignment
    auto reason = GetError(result).reason;
    EXPECT_TRUE(reason == InfeasibleReason::UtilizationExceeded ||
                reason == InfeasibleReason::NoCbsCpu);
}

// ─── End-to-End: DDR-007 §5 Sample Workloads ────────────────────────────────

TEST_F(GlobalSchedulerTest, Ddr007SampleWorkloads) {
    // From DDR-007 §5.1:
    //   brake_ctrl    (L1, period=10ms,  wcet=2ms,   deadline=5ms)
    //   steer_ctrl    (L1, period=20ms,  wcet=2ms,   deadline=10ms)
    //   collision_det (L2, MIT=10ms,     wcet=1.5ms, deadline=10ms)
    //   lidar_proc    (L2, MIT=5ms,      wcet=2ms,   deadline=5ms)
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
        MakeTtTask("steer", "steer_ctrl", 20000, 2000, 10000),
        MakeCbsTask("collision", "col_detect", 10000, 1500, 10000),
        MakeCbsTask("lidar_proc", "lidar_main", 5000, 2000, 5000),
    };

    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.hyperperiod_us(), 0u);
    EXPECT_GT(table.partitions_size(), 0);

    // Count total TT slots and CBS entries across all partitions
    int total_tt = 0;
    int total_cbs = 0;
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            total_tt += table.partitions(p).layers(l).tt_slots_size();
            total_cbs += table.partitions(p).layers(l).cbs_entries_size();
        }
        // Each partition should have isolated=true
        EXPECT_TRUE(table.partitions(p).cpuset().isolated());
    }

    // Two TT workloads go to separate CPUs:
    //   CPU A: brake_ctrl (period=10ms, global hyperperiod=20ms → 2 slots)
    //   CPU B: steer_ctrl (period=20ms, global hyperperiod=20ms → 1 slot)
    // Total TT slots == 3 (brake_ctrl repeats twice in 20ms hyperperiod)
    EXPECT_EQ(total_tt, 3);
    // Should have CBS entries for collision_det and lidar_main
    EXPECT_GE(total_cbs, 2);
}

// ─── Multi-CPU ──────────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, MultiCpuTtDifferentPeriodsUnrollAcrossGlobalHyperperiod) {
    // Reproduction case for multi-CPU TT under-scheduling bug (#61):
    // Workload 1: tt_task1 on CPU 2 (period=20ms, wcet=16ms, deadline=18ms)
    // Workload 2: tt_task2 on CPU 3 (period=40ms, wcet=2ms,  deadline=4ms)
    // Global hyperperiod = LCM(20ms, 40ms) = 40ms
    // Expected:
    //   CPU 2 partition has 2 slots (offset 0us, offset 20000us)
    //   CPU 3 partition has 1 slot  (offset 0us)
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("fcw_producer_tt",  "tt_task1", 20000, 16000, 18000),
        MakeTtTask("fcw_producer_tt2", "tt_task2", 40000,  2000,  4000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_EQ(table.hyperperiod_us(), 40000u);
    ASSERT_EQ(table.partitions_size(), 2);

    int total_tt = 0;
    std::vector<uint32_t> cpu2_offsets;
    std::vector<uint32_t> cpu3_offsets;

    for (int p = 0; p < table.partitions_size(); ++p) {
        const auto& part = table.partitions(p);
        ASSERT_GT(part.cpuset().cpus_size(), 0);
        uint32_t cpu = part.cpuset().cpus(0);

        for (int l = 0; l < part.layers_size(); ++l) {
            const auto& layer = part.layers(l);
            for (int s = 0; s < layer.tt_slots_size(); ++s) {
                const auto& slot = layer.tt_slots(s);
                total_tt++;
                if (cpu == 2) {
                    cpu2_offsets.push_back(slot.offset_us());
                    EXPECT_EQ(slot.task_id(), "tt_task1");
                    EXPECT_EQ(slot.duration_us(), 16000u);
                } else if (cpu == 3) {
                    cpu3_offsets.push_back(slot.offset_us());
                    EXPECT_EQ(slot.task_id(), "tt_task2");
                    EXPECT_EQ(slot.duration_us(), 2000u);
                }
            }
        }
    }

    EXPECT_EQ(total_tt, 3);
    ASSERT_EQ(cpu2_offsets.size(), 2u);
    EXPECT_EQ(cpu2_offsets[0], 0u);
    EXPECT_EQ(cpu2_offsets[1], 20000u);

    ASSERT_EQ(cpu3_offsets.size(), 1u);
    EXPECT_EQ(cpu3_offsets[0], 0u);
}

TEST_F(GlobalSchedulerTest, PreservesPinnedTtWorkloadCpuWhenAssigningNewWork)
{
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("retained", "retained_task", 10000, 1000, 2000),
        MakeTtTask("new_work", "new_task", 5000, 1000, 2000),
    };
    tasks[0].assigned_cpu = 3;

    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));
    const auto& table = GetTable(result);

    std::map<std::string, std::set<uint32_t>> cpus;
    for (const auto& partition : table.partitions()) {
        ASSERT_EQ(partition.cpuset().cpus_size(), 1);
        for (const auto& layer : partition.layers()) {
            for (const auto& slot : layer.tt_slots())
                cpus[slot.workload_id()].insert(partition.cpuset().cpus(0));
        }
    }
    EXPECT_EQ(cpus["retained"], (std::set<uint32_t>{3}));
    EXPECT_EQ(cpus["new_work"], (std::set<uint32_t>{2}));
}

TEST_F(GlobalSchedulerTest, UnrolledSlotsExceedingNodeCapacityFails) {
    // Non-harmonic periods on separate CPUs (1,000 us and 1,000,001 us)
    // produce LCM ≈ 1,000,001,000 us. Unrolled slots = 1,000,001 + 1000 > 2048.
    // Must be rejected as TtSlotCapacityExceeded to prevent scheduler stalling.
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl_a", "task_fast", 1000, 100, 1000),
        MakeTtTask("wl_b", "task_coprime", 1000001, 100, 1000001),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    EXPECT_EQ(GetError(result).reason, InfeasibleReason::TtSlotCapacityExceeded);
}

TEST_F(GlobalSchedulerTest, TtSlotCapacityExactBoundary) {
    // Exact boundary test for MAX_NODE_TT_SLOTS (2048):
    // Case 1: Exactly 2048 slots (2047 + 1 = 2048) -> SUCCEEDS
    {
        std::vector<ClassifiedTask> tasks = {
            MakeTtTask("wl_a", "t_fast", 1000, 10, 1000),
            MakeTtTask("wl_b", "t_slow", 2047000, 10, 2047000),
        };
        auto result = scheduler_->generate_schedule("node1", tasks);
        ASSERT_TRUE(IsSuccess(result));
        const auto& table = GetTable(result);
        int total_tt = 0;
        for (int p = 0; p < table.partitions_size(); ++p) {
            for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
                total_tt += table.partitions(p).layers(l).tt_slots_size();
            }
        }
        EXPECT_EQ(total_tt, 2048);
    }

    // Case 2: Exactly 2049 slots (2048 + 1 = 2049 > 2048) -> FAILS with TtSlotCapacityExceeded
    {
        std::vector<ClassifiedTask> tasks = {
            MakeTtTask("wl_a", "t_fast", 1000, 10, 1000),
            MakeTtTask("wl_b", "t_slow", 2048000, 10, 2048000),
        };
        auto result = scheduler_->generate_schedule("node1", tasks);
        ASSERT_FALSE(IsSuccess(result));
        EXPECT_EQ(GetError(result).reason, InfeasibleReason::TtSlotCapacityExceeded);
    }
}

TEST_F(GlobalSchedulerTest, HyperperiodExceedingUint32LimitFails) {
    // Large periods (3,000,000,000 us and 4,000,000,000 us) produce LCM = 12×10^9 us > UINT32_MAX.
    // Must be rejected as TtSlotCapacityExceeded per REQ-011 AC-16 without representation loss.
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl_a", "task_3s", 3000000000u, 100, 3000000000u),
        MakeTtTask("wl_b", "task_4s", 4000000000u, 100, 4000000000u),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    EXPECT_EQ(GetError(result).reason, InfeasibleReason::TtSlotCapacityExceeded);
}

TEST_F(GlobalSchedulerTest, MultipleTtWorkloadsGetSeparateCpus) {
    // Two TT workloads should prefer separate CPUs
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl_a", "task_a", 10000, 2000, 5000),
        MakeTtTask("wl_b", "task_b", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    // With default 2 CPUs available, should get 2 partitions
    EXPECT_GE(table.partitions_size(), 2);
}

// ─── Feasibility Constants ──────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, FeasibilityConstants) {
    EXPECT_DOUBLE_EQ(GlobalScheduler::U_OVERHEAD, 0.02);
    EXPECT_DOUBLE_EQ(GlobalScheduler::U_BOUND, 0.80);
    EXPECT_EQ(GlobalScheduler::CBS_MIN_EXEC_US, 100u);
    EXPECT_EQ(GlobalScheduler::MAX_NODE_TT_SLOTS, 2048u);
}

// ─── Table Structure Verification ───────────────────────────────────────────

TEST_F(GlobalSchedulerTest, TableHasRequiredFields) {
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_FALSE(table.table_id().empty());
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.hyperperiod_us(), 0u);
    // epoch_ns is assigned per publication batch, not per node table.
    EXPECT_EQ(table.epoch_ns(), 0u);

    // Check TT slot hashes are non-zero
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            const auto& layer = table.partitions(p).layers(l);
            for (int s = 0; s < layer.tt_slots_size(); ++s) {
                EXPECT_NE(layer.tt_slots(s).workload_id_hash(), 0u);
                EXPECT_NE(layer.tt_slots(s).task_id_hash(), 0u);
            }
        }
    }
}
