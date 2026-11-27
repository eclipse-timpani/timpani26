// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "orchestrator_service.h"
#include "fault_client.h"
#include "tlog.h"
#include <iostream>

namespace timpani {
namespace orchestrator {

OrchestratorServiceImpl::OrchestratorServiceImpl()
    : fault_worker_(&OrchestratorServiceImpl::forward_faults, this)
{
}

OrchestratorServiceImpl::~OrchestratorServiceImpl()
{
    {
        std::lock_guard<std::mutex> lock(fault_queue_mutex_);
        fault_worker_running_.store(false, std::memory_order_release);
        fault_queue_.clear();
    }
    fault_queue_cv_.notify_all();
    if (fault_worker_.joinable()) fault_worker_.join();
}

void OrchestratorServiceImpl::forward_faults()
{
    for (;;) {
        PendingFault fault;
        {
            std::unique_lock<std::mutex> lock(fault_queue_mutex_);
            fault_queue_cv_.wait(lock, [this] {
                return !fault_worker_running_.load(std::memory_order_acquire) ||
                       !fault_queue_.empty();
            });
            if (fault_queue_.empty() &&
                !fault_worker_running_.load(std::memory_order_acquire)) {
                return;
            }
            fault = std::move(fault_queue_.front());
            fault_queue_.pop_front();
        }

        if (!FaultServiceClient::GetInstance().NotifyFault(
                fault.workload_id, fault.node_id, fault.task_id,
                static_cast<schedinfo::v1::FaultType>(fault.fault_type),
                fault.dmiss_count)) {
            TLOG_WARN("[Orchestrator] Fault forwarding failed (queued for "
                      "retry if enabled): workload=", fault.workload_id,
                      " task=", fault.task_id);
        }
    }
}

grpc::Status OrchestratorServiceImpl::NodeStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream) {
    
    auto current_node = std::make_shared<ConnectedNode>();
    current_node->stream = stream;

    grpc::Status result;
    NodeEvent event;
    while (stream->Read(&event)) {
        if (phase0_ack_) {
            if (current_node->node_id.empty() &&
                (!event.has_ready() || event.node_id() != phase0_node_id_)) {
                TLOG_ERROR("[RTU-P0] rejected NodeStream identity: ", event.node_id());
                return {grpc::StatusCode::INVALID_ARGUMENT, "expected configured node readiness"};
            }
            if (!current_node->node_id.empty() && event.node_id() != current_node->node_id) {
                phase0_abort_("NodeStream event changed node identity");
                result = {grpc::StatusCode::INVALID_ARGUMENT, "node identity changed"};
                break;
            }
        }
        if (current_node->node_id.empty() && !event.node_id().empty()) {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            if (phase0_ack_ && connected_nodes_.count(event.node_id())) {
                TLOG_ERROR("[RTU-P0] duplicate NodeStream: ", event.node_id());
                return {grpc::StatusCode::ALREADY_EXISTS, "node already connected"};
            }
            current_node->node_id = event.node_id();
            connected_nodes_[current_node->node_id] = current_node;
            std::cout << "[Orchestrator] Node connected: " << current_node->node_id << std::endl;
        }

        if (event.has_ready()) {
            handle_node_ready(event.ready(), *current_node);
        } else if (event.has_status()) {
            handle_node_status(event.status(), *current_node);
        } else if (event.has_fault()) {
            handle_fault_info(event.fault(), *current_node);
        } else if (event.has_applied()) {
            handle_table_applied(event.applied(), *current_node);
        }
    }

    if (!current_node->node_id.empty()) {
        if (phase0_abort_) phase0_abort_("NodeStream disconnected; no automatic replay");
        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            auto it = connected_nodes_.find(current_node->node_id);
            if (it != connected_nodes_.end() && it->second == current_node) {
                connected_nodes_.erase(it);
            }
        }
        {
            std::lock_guard<std::mutex> lock(current_node->write_mutex);
            current_node->stream = nullptr;
        }
        std::cout << "[Orchestrator] Node disconnected: " << current_node->node_id << std::endl;
    }

    return result;
}

void OrchestratorServiceImpl::handle_node_ready(const NodeReady& ready, ConnectedNode& node) {
    std::cout << "[Orchestrator] NodeReady from " << node.node_id 
              << ": CPUs=" << ready.cpu_count() 
              << " RAM=" << ready.memory_mb() << "MB" << std::endl;
}

void OrchestratorServiceImpl::handle_node_status(const NodeStatus& status, ConnectedNode& node) {
    // Log or forward stats
}

void OrchestratorServiceImpl::handle_fault_info(const FaultInfo& fault, ConnectedNode& node) {
    std::cerr << "[Orchestrator] Fault from " << node.node_id 
              << " task=" << fault.task_id() << " type=" << fault.fault_type()
              << " dmiss_count=" << fault.dmiss_count()
              << " current_limit=" << fault.current_limit() << std::endl;

    std::string workload_id = fault.workload_id();
    if (workload_id.empty()) {
        workload_id = std::to_string(fault.workload_id_hash());
    }
    
    std::string task_id = fault.task_id();
    if (task_id.empty()) {
        task_id = std::to_string(fault.task_id_hash());
    }
    
    schedinfo::v1::FaultType proto_fault_type;
    switch(fault.fault_type()) {
        case timpani::node::v1::FaultType::DMISS:
            proto_fault_type = schedinfo::v1::FaultType::DMISS;
            break;
        default:
            proto_fault_type = schedinfo::v1::FaultType::UNKNOWN;
            break;
    }

    if (fault.current_limit() > 0 && fault.dmiss_count() >= fault.current_limit()) {
        std::cerr << "[Orchestrator] DMISS limit reached: workload=" << workload_id
                  << " task=" << task_id
                  << " dmiss_count=" << fault.dmiss_count()
                  << " current_limit=" << fault.current_limit() << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(fault_queue_mutex_);
        if (fault_queue_.size() >= kMaxFaultQueue) {
            fault_queue_.pop_front();
            TLOG_WARN("[Orchestrator] Fault queue full; dropping oldest event");
        }
        fault_queue_.push_back(PendingFault{std::move(workload_id),
                                             node.node_id,
                                             std::move(task_id),
                                             static_cast<int>(proto_fault_type),
                                             fault.dmiss_count()});
    }
    fault_queue_cv_.notify_one();
}

void OrchestratorServiceImpl::handle_table_applied(const TableApplied& applied, ConnectedNode& node) {
    if (phase0_ack_) {
        phase0_ack_(node.node_id, applied);
        return;
    }
    std::cout << "[Orchestrator] TableApplied from " << node.node_id 
              << " table=" << applied.table_id() << " success=" << applied.success() << std::endl;
}

bool OrchestratorServiceImpl::push_full_table(const std::string& node_id, const HierarchicalScheduleTable& table) {
    std::shared_ptr<ConnectedNode> target_node;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        auto it = connected_nodes_.find(node_id);
        if (it != connected_nodes_.end()) {
            target_node = it->second;
        }
    }
    
    if (target_node) {
        ControlCommand cmd;
        *cmd.mutable_full_table() = table;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        return target_node->stream && target_node->stream->Write(cmd);
    }
    return false;
}

bool OrchestratorServiceImpl::push_update(const std::string& node_id, const ScheduleTableUpdate& update) {
    if (phase0_ack_) {
        TLOG_ERROR("[RTU-P0] node_id=", node_id, " table_id=", update.table_id(),
                   " result=rejected reason=incremental updates are outside Phase 0");
        return false;
    }
    std::shared_ptr<ConnectedNode> target_node;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        auto it = connected_nodes_.find(node_id);
        if (it != connected_nodes_.end()) {
            target_node = it->second;
        }
    }
    
    if (target_node) {
        ControlCommand cmd;
        *cmd.mutable_update() = update;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        return target_node->stream && target_node->stream->Write(cmd);
    }
    return false;
}

std::vector<std::string> OrchestratorServiceImpl::get_connected_node_ids() {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    std::vector<std::string> ids;
    ids.reserve(connected_nodes_.size());
    for (const auto& pair : connected_nodes_) {
        ids.push_back(pair.first);
    }
    return ids;
}

bool OrchestratorServiceImpl::broadcast_recovery_signal(const std::string& workload_id, RecoverySignal::Action action) {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    if (phase0_ack_ && connected_nodes_.empty()) {
        TLOG_ERROR("[RTU-P0] recovery signal not sent: no connected node");
        return false;
    }
    bool all_success = true;
    for (auto& pair : connected_nodes_) {
        ControlCommand cmd;
        RecoverySignal* rec_sig = cmd.mutable_recovery();
        rec_sig->set_workload_id(workload_id);
        rec_sig->set_action(action);
        
        auto& target_node = pair.second;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        if (!target_node->stream || !target_node->stream->Write(cmd)) {
            all_success = false;
        }
    }
    return all_success;
}

} // namespace orchestrator
} // namespace timpani
