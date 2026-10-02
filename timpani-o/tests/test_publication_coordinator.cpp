/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "../src/global_scheduler.h"
#include "../src/node_config.h"
#include "../src/publication_coordinator.h"

namespace {

constexpr uint64_t kStartNs = 1800000000000000000ULL;

class PublicationCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        now_ns_ = kStartNs;
        clock_calls_ = 0;
        coordinator_ = std::make_unique<PublicationCoordinator>([this]() {
            ++clock_calls_;
            return now_ns_;
        });
        margin_ = PublicationCoordinator::propagation_margin_ns();
    }

    // Generate a real table through GlobalScheduler (no epoch assigned).
    timpani::node::v1::HierarchicalScheduleTable Generate(
        const std::string& node_id, uint32_t period_us) {
        GlobalScheduler scheduler(std::make_shared<NodeConfigManager>());
        ClassifiedTask t;
        t.workload_id = "wl_" + node_id;
        t.task_id = "task_" + node_id;
        t.mechanism = Mechanism::TT;
        t.period_us = period_us;
        t.wcet_us = period_us / 10;
        t.deadline_us = period_us;
        auto result = scheduler.generate_schedule(node_id, {t});
        EXPECT_TRUE(std::holds_alternative<
                    timpani::node::v1::HierarchicalScheduleTable>(result));
        return std::get<timpani::node::v1::HierarchicalScheduleTable>(result);
    }

    static void ExpectCommonEpoch(
        const PublicationCoordinator::PreparedBatch& batch) {
        for (const auto& [node_id, table] : batch.tables) {
            EXPECT_EQ(table.epoch_ns(), batch.epoch_ns) << "node " << node_id;
        }
    }

    uint64_t now_ns_;
    int clock_calls_;
    uint64_t margin_;
    std::unique_ptr<PublicationCoordinator> coordinator_;
};

}  // namespace

TEST_F(PublicationCoordinatorTest, GlobalSchedulerDoesNotAssignEpoch) {
    EXPECT_EQ(Generate("node1", 10000).epoch_ns(), 0u);
}

TEST_F(PublicationCoordinatorTest, MultiNodeBatchSharesOneEpoch) {
    ScheduleTableMap tables = {
        {"node1", Generate("node1", 10000)},
        {"node2", Generate("node2", 20000)},
        {"node3", Generate("node3", 5000)},
    };
    EXPECT_EQ(clock_calls_, 0);  // table generation never reads the clock

    auto batch =
        coordinator_->PrepareBatch({"node1", "node2", "node3"}, tables);

    EXPECT_EQ(clock_calls_, 1);
    EXPECT_TRUE(batch.epoch_renewed);
    EXPECT_EQ(batch.epoch_ns, kStartNs + margin_);
    ASSERT_EQ(batch.tables.size(), 3u);
    ExpectCommonEpoch(batch);
    // Table content other than epoch is preserved.
    EXPECT_EQ(batch.tables.at("node2").hyperperiod_us(),
              tables.at("node2").hyperperiod_us());
}

TEST_F(PublicationCoordinatorTest, EpochIndependentOfNodeCountAndOrder) {
    ScheduleTableMap tables;
    std::vector<std::string> ids = {"z", "a", "m", "c", "x", "b"};
    std::set<std::string> nodes;
    for (const auto& id : ids) {
        tables[id] = Generate(id, 10000);
        nodes.insert(id);
    }

    auto batch = coordinator_->PrepareBatch(nodes, tables);
    EXPECT_EQ(clock_calls_, 1);
    EXPECT_EQ(batch.tables.size(), ids.size());
    ExpectCommonEpoch(batch);

    PublicationCoordinator single([this]() { return now_ns_; });
    auto one = single.PrepareBatch({"a"}, tables);
    EXPECT_EQ(one.epoch_ns, batch.epoch_ns);
}

TEST_F(PublicationCoordinatorTest, EmptyTablesUseCommonEpoch) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)}};

    auto batch = coordinator_->PrepareBatch({"node1", "idle"}, tables);

    ASSERT_EQ(batch.tables.count("idle"), 1u);
    const auto& empty = batch.tables.at("idle");
    EXPECT_EQ(empty.node_id(), "idle");
    EXPECT_EQ(empty.partitions_size(), 0);
    EXPECT_EQ(empty.hyperperiod_us(), 10000u);
    EXPECT_EQ(clock_calls_, 1);
    ExpectCommonEpoch(batch);
}

TEST_F(PublicationCoordinatorTest, RetryReusesEpochWhileMarginRemains) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)},
                               {"node2", Generate("node2", 10000)}};
    // Retry at the same instant: exactly the full margin still remains.
    auto first = coordinator_->PrepareBatch({"node1", "node2"}, tables);

    auto retry = coordinator_->PrepareBatch({"node2"}, tables);
    EXPECT_FALSE(retry.epoch_renewed);
    EXPECT_EQ(retry.epoch_ns, first.epoch_ns);
    EXPECT_EQ(retry.tables.at("node2").epoch_ns(), first.epoch_ns);
}

TEST_F(PublicationCoordinatorTest, InsufficientMarginRenewsWholeBatch) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)},
                               {"node2", Generate("node2", 10000)}};
    auto first = coordinator_->PrepareBatch({"node1", "node2"}, tables);

    now_ns_ += 1;  // remaining time is now just below the margin
    auto retry = coordinator_->PrepareBatch({"node1", "node2"}, tables);

    EXPECT_TRUE(retry.epoch_renewed);
    EXPECT_EQ(retry.epoch_ns, now_ns_ + margin_);
    EXPECT_NE(retry.epoch_ns, first.epoch_ns);
    ExpectCommonEpoch(retry);
}

TEST_F(PublicationCoordinatorTest, ExpiredEpochRenews) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)}};
    coordinator_->PrepareBatch({"node1"}, tables);

    now_ns_ += 2 * margin_;  // epoch already in the past
    auto retry = coordinator_->PrepareBatch({"node1"}, tables);

    EXPECT_TRUE(retry.epoch_renewed);
    EXPECT_EQ(retry.epoch_ns, now_ns_ + margin_);
}

TEST_F(PublicationCoordinatorTest, ReconnectReusesValidEpoch) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)},
                               {"node2", Generate("node2", 10000)}};
    auto first = coordinator_->PrepareBatch({"node1"}, tables);

    now_ns_ -= margin_;  // plenty of time left before the cached epoch
    auto reconnect = coordinator_->PrepareBatch({"node1", "node2"}, tables);

    EXPECT_FALSE(reconnect.epoch_renewed);
    EXPECT_EQ(reconnect.epoch_ns, first.epoch_ns);
    ExpectCommonEpoch(reconnect);
}

TEST_F(PublicationCoordinatorTest, NewGenerationGetsNewEpoch) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)}};
    auto first = coordinator_->PrepareBatch({"node1"}, tables);

    now_ns_ -= margin_;  // cached epoch would still be valid
    coordinator_->BeginGeneration();
    auto next = coordinator_->PrepareBatch({"node1"}, tables);

    EXPECT_TRUE(next.epoch_renewed);
    EXPECT_EQ(next.epoch_ns, now_ns_ + margin_);
    EXPECT_NE(next.epoch_ns, first.epoch_ns);
    EXPECT_EQ(clock_calls_, 2);
}

TEST_F(PublicationCoordinatorTest, EpochNotEarlierThanNowPlusMargin) {
    ScheduleTableMap tables = {{"node1", Generate("node1", 10000)}};
    for (int i = 0; i < 5; ++i) {
        auto batch = coordinator_->PrepareBatch({"node1"}, tables);
        EXPECT_GE(batch.epoch_ns, now_ns_ + margin_);
        now_ns_ += margin_ / 3;
    }
}

TEST_F(PublicationCoordinatorTest, DefaultClockProducesFutureEpoch) {
    PublicationCoordinator real;
    auto batch = real.PrepareBatch({"node1"}, {});
    EXPECT_GT(batch.epoch_ns, PublicationCoordinator::propagation_margin_ns());
    ExpectCommonEpoch(batch);
}
