/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef PUBLICATION_COORDINATOR_H
#define PUBLICATION_COORDINATOR_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <stdint.h>

#include "global_scheduler.h"
#include "proto/node_control.grpc.pb.h"

/**
 * @brief Assigns one common epoch_ns to every table of a multi-node
 *        schedule publication (DDR-004 §9, DDR-003).
 *
 * All target tables of a batch are prepared first (including empty tables
 * for nodes without a schedule); then CLOCK_REALTIME is sampled once and
 *
 *     common_epoch_ns = now_ns + propagation_margin_ns
 *
 * is applied to every table in the batch.
 *
 * The epoch is cached per schedule generation. Retries and reconnecting
 * nodes of the same generation reuse it while at least
 * propagation_margin_ns remains; otherwise a new epoch is calculated and
 * the whole batch must be resent (PreparedBatch::epoch_renewed).
 */
class PublicationCoordinator {
public:
    /// Returns the current CLOCK_REALTIME time in nanoseconds.
    using RealtimeClock = std::function<uint64_t()>;

    struct PreparedBatch {
        ScheduleTableMap tables;   // node_id → table stamped with epoch_ns
        uint64_t epoch_ns = 0;
        bool epoch_renewed = false; // true → resend every node in the batch
    };

    PublicationCoordinator();
    explicit PublicationCoordinator(RealtimeClock clock);

    /**
     * @brief Start a new schedule generation; drops the cached epoch.
     */
    void BeginGeneration();

    /**
     * @brief Prepare the publication batch for @p target_nodes.
     *
     * Nodes missing from @p sched_tables receive an empty table. The
     * common epoch is determined only after all tables are prepared.
     */
    PreparedBatch PrepareBatch(const std::set<std::string>& target_nodes,
                               const ScheduleTableMap& sched_tables);

    /**
     * @brief Table sent to a node that has no scheduled tasks.
     */
    static timpani::node::v1::HierarchicalScheduleTable MakeEmptyTable(
        const std::string& node_id);

    static uint64_t propagation_margin_ns() { return kPropagationMarginNs; }

private:
    // DDR-004 §9 candidate value; covers transmission to the last node,
    // gRPC processing, table application, TimerMaster preparation,
    // CLOCK_REALTIME sync error and wakeup jitter. Subject to validation
    // against measured system behavior.
    static constexpr uint64_t kPropagationMarginNs = 500ULL * 1000 * 1000;

    static uint64_t SystemRealtimeNs();

    RealtimeClock clock_;
    bool has_epoch_ = false;
    uint64_t epoch_ns_ = 0;
};

#endif // PUBLICATION_COORDINATOR_H
