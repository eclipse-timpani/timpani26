/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <utility>

#include "publication_coordinator.h"

using timpani::node::v1::HierarchicalScheduleTable;

PublicationCoordinator::PublicationCoordinator()
    : PublicationCoordinator(&PublicationCoordinator::SystemRealtimeNs)
{
}

PublicationCoordinator::PublicationCoordinator(RealtimeClock clock)
    : clock_(std::move(clock))
{
}

void PublicationCoordinator::BeginGeneration()
{
    has_epoch_ = false;
    epoch_ns_ = 0;
}

PublicationCoordinator::PreparedBatch PublicationCoordinator::PrepareBatch(
    const std::set<std::string>& target_nodes,
    const ScheduleTableMap& sched_tables)
{
    PreparedBatch batch;

    // 1. Prepare the complete batch, including empty tables.
    for (const auto& node_id : target_nodes) {
        auto it = sched_tables.find(node_id);
        if (it != sched_tables.end()) {
            batch.tables[node_id] = it->second;
        } else {
            batch.tables[node_id] = MakeEmptyTable(node_id);
        }
    }

    // 2. First batch of a generation: sample CLOCK_REALTIME once,
    //    immediately before publication. Later batches (retry, reconnect)
    //    reuse the cached epoch even if it has passed: TimerMaster catches
    //    up on the same epoch + k * hyperperiod grid, so running nodes keep
    //    their phase and late nodes join it.
    if (!has_epoch_) {
        epoch_ns_ = clock_() + kPropagationMarginNs;
        has_epoch_ = true;
        batch.epoch_renewed = true;
    }
    batch.epoch_ns = epoch_ns_;

    // 3. Apply the same epoch to every table in the batch.
    for (auto& [node_id, table] : batch.tables) {
        table.set_epoch_ns(epoch_ns_);
    }

    return batch;
}

HierarchicalScheduleTable PublicationCoordinator::MakeEmptyTable(
    const std::string& node_id)
{
    HierarchicalScheduleTable table;
    table.set_table_id("table_v1");
    table.set_node_id(node_id);
    table.set_hyperperiod_us(10000);
    return table;
}

uint64_t PublicationCoordinator::SystemRealtimeNs()
{
    // std::chrono::system_clock is CLOCK_REALTIME on Linux.
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
