// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <grpcpp/grpcpp.h>
#include "proto/node_control.grpc.pb.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class OrchestratorServiceTestPeer;
class SchedInfoServer;

namespace timpani {
namespace orchestrator {

using namespace timpani::node::v1;

class OrchestratorServiceImpl final : public OrchestratorService::Service {
public:
    OrchestratorServiceImpl();
    ~OrchestratorServiceImpl() override;

    grpc::Status NodeStream(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream) override;

    bool push_full_table(const std::string& node_id, const HierarchicalScheduleTable& table);
    bool push_update(const std::string& node_id, const ScheduleTableUpdate& update);
    bool broadcast_recovery_signal(const std::string& workload_id, RecoverySignal::Action action);
    std::vector<std::string> get_connected_node_ids();

private:
    friend class ::OrchestratorServiceTestPeer;
    friend class ::SchedInfoServer;

    static constexpr size_t kMaxFaultQueue = 1024;
    struct PendingFault {
        std::string workload_id;
        std::string node_id;
        std::string task_id;
        int fault_type = 0;
        uint32_t dmiss_count = 0;
    };

    std::string phase0_node_id_;
    std::function<void(const std::string&, const TableApplied&)> phase0_ack_;
    std::function<void(const std::string&)> phase0_abort_;
    struct ConnectedNode {
        std::string node_id;
        grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream;
        std::mutex write_mutex; // Protect stream writes
    };

    std::mutex nodes_mutex_;
    std::map<std::string, std::shared_ptr<ConnectedNode>> connected_nodes_;

    std::mutex fault_queue_mutex_;
    std::condition_variable fault_queue_cv_;
    std::deque<PendingFault> fault_queue_;
    std::atomic<bool> fault_worker_running_{true};
    std::thread fault_worker_;

    void forward_faults();
    void handle_node_ready(const NodeReady& ready, ConnectedNode& node);
    void handle_node_status(const NodeStatus& status, ConnectedNode& node);
    void handle_fault_info(const FaultInfo& fault, ConnectedNode& node);
    void handle_table_applied(const TableApplied& applied, ConnectedNode& node);
};

} // namespace orchestrator
} // namespace timpani
