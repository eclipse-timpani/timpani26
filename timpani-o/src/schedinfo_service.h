/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef SCHEDINFO_SERVICE_H
#define SCHEDINFO_SERVICE_H

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <thread>
#include <grpcpp/grpcpp.h>

#include "proto/schedinfo.grpc.pb.h"
#include "node_config.h"
#include "global_scheduler.h"

using namespace grpc;
using namespace schedinfo::v1;

namespace timpani::orchestrator {
class OrchestratorServiceImpl;
}
class Phase0TestPeer;

/**
* @brief Implementation of the SchedInfoService gRPC service
*
* This service handles scheduling information deliveries from Pullpiri to Timpani-O.
* Uses the DDR-007 GlobalScheduler to generate TT+CBS schedule tables.
*/
class SchedInfoServiceImpl final : public SchedInfoService::Service
{
  public:
    explicit SchedInfoServiceImpl(std::shared_ptr<NodeConfigManager> node_config_manager = nullptr);
    ~SchedInfoServiceImpl() = default;

    Status AddSchedInfo(ServerContext* context, const SchedInfo* request,
                        Response* reply) override;

    /**
     * @brief Remove a workload from the schedule and regenerate.
     * In Phase 0, true means submitted, not applied; false (recovery STOP) returns
     * true only after the correlated TableApplied confirms the removal.
     * @param workload_id The ID of the workload to remove.
     * @return true if workload was found and removed, false otherwise.
     */
    bool RemoveWorkload(const std::string& workload_id, std::string* resolved_id = nullptr, bool trigger_push = true);

    /**
     * @brief Get all schedule tables (workload_id → node_id → table).
     * @param changed  If non-null, set to true if tables changed since last call.
     * @return Current schedule table map.
     * In Phase 0, only ACK-confirmed tables are returned.
     */
    ScheduleTableMap GetScheduleTables(bool* changed = nullptr);

    std::unique_ptr<RecoveryService::Service> recovery_service_;
  private:
    friend class SchedInfoServer;
    friend class Phase0TestPeer;

    enum class Phase0Operation { Replace, Add, Remove, Clear };
    using Phase0Workloads = std::map<std::string, SchedInfo>;
    using Phase0Table = timpani::node::v1::HierarchicalScheduleTable;
    struct OConfirmed {
        Phase0Workloads workloads;
        Phase0Table table;
    };
    struct OAttemptCompletion {
        bool completed = false;
        bool success = false;
    };
    struct OAttempt {
        OConfirmed candidate;
        Phase0Operation operation;
        std::string targets;
        bool sent = false;
        std::shared_ptr<OAttemptCompletion> completion;
    };
    struct Phase0State {
        std::mutex mutex;
        std::string node_id;
        int cpu = -1;
        std::optional<OConfirmed> confirmed;
        std::optional<OAttempt> attempt;
        std::map<std::string, std::string> task_workloads;
        uint64_t next_table_id = 1;
        bool changed = false;
        bool stopped = false;
        // Notified when an attempt settles (ACK or abort); recovery STOP waits on it.
        std::condition_variable settled;
    };
    std::shared_ptr<Phase0State> phase0_;

    bool SubmitPhase0Update(
        Phase0Operation operation, const std::vector<SchedInfo>& inputs,
        const std::vector<std::string>& removals = {},
        std::shared_ptr<OAttemptCompletion> completion = nullptr);
    bool PreparePhase0Candidate(Phase0Operation operation,
                                const std::vector<SchedInfo>& inputs,
                                const std::vector<std::string>& removals,
                                OAttempt& attempt, std::string& error);
    bool ValidatePhase0Workloads(Phase0Workloads& workloads,
                                 std::vector<ClassifiedTask>& tasks, std::string& error) const;
    bool ValidatePhase0Table(const Phase0Table& table, std::string& error) const;
    bool RemovePhase0Workload(const std::string& workload_id,
                              std::string* resolved_id, bool trigger_push);
    static void HandlePhase0Ack(Phase0State& state, const std::string& node_id,
                                const timpani::node::v1::TableApplied& applied);
    static void AbortPhase0(Phase0State& state, const std::string& reason);
    static const char* Phase0OperationName(Phase0Operation operation);

    static int SchedPolicyToInt(SchedPolicy policy);

    // Convert gRPC TaskInfo → ClassifiedTask using TemporalClass
    std::vector<ClassifiedTask> ConvertToClassifiedTasks(
        const SchedInfo* request, Mechanism mechanism);

    // Regenerate schedule tables for all workloads on every target node
    bool RegenerateAllSchedules(std::string& error_detail);

    // Per-workload classified tasks: workload_id → (node_id, tasks).
    // A workload is bound to exactly one node (DDR-001).
    struct WorkloadEntry {
        std::string node_id;
        std::vector<ClassifiedTask> tasks;
    };
    std::map<std::string, WorkloadEntry> workload_tasks_;

    // Schedule tables: node_id → combined HierarchicalScheduleTable (all workloads)
    ScheduleTableMap schedule_tables_;
    mutable std::shared_mutex schedule_mutex_;
    bool schedule_changed_;

    std::shared_ptr<NodeConfigManager> node_config_manager_;
    std::shared_ptr<GlobalScheduler> global_scheduler_;
};

/**
 * @brief SchedInfoServer class for managing the SchedInfoService gRPC server
 */
class SchedInfoServer
{
  public:
    explicit SchedInfoServer(std::shared_ptr<NodeConfigManager> node_config_manager = nullptr);
    ~SchedInfoServer();
    bool Start(int port, std::vector<grpc::Service*> additional_services = {});
    void Stop();

    // Call before either gRPC server starts; orchestrator must outlive this server.
    bool EnablePhase0(timpani::orchestrator::OrchestratorServiceImpl& orchestrator);
    // Called by the existing non-RT main loop; never retries a transmitted attempt.
    void PushPendingTables();

    bool RemoveWorkload(const std::string& workload_id, std::string* resolved_id = nullptr, bool trigger_push = true) { return service_.RemoveWorkload(workload_id, resolved_id, trigger_push); }
    bool Phase0Enabled() const { return phase0_orchestrator_ != nullptr; }

    ScheduleTableMap GetScheduleTables(bool* changed = nullptr);
    void DumpSchedInfo();

    std::unique_ptr<RecoveryService::Service> recovery_service_;
 private:
    friend class Phase0TestPeer;
    SchedInfoServiceImpl service_;
    timpani::orchestrator::OrchestratorServiceImpl* phase0_orchestrator_ = nullptr;
    std::unique_ptr<Server> server_;
    std::unique_ptr<std::thread> server_thread_;
};

#endif  // SCHEDINFO_SERVICE_H
