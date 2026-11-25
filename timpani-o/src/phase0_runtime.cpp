/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include "schedinfo_service.h"
#include "orchestrator_service.h"
#include "tlog.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <tuple>

namespace {

// Effective Phase 0 limits: N's TT map and libttsched's task array.
constexpr size_t kMaxTtSlots = 2048;
constexpr size_t kMaxTasks = 32;
// SRS-011 REQ-RTU-F-032 default ACK wait; bounds the recovery STOP reply.
constexpr auto kRecoveryStopAckTimeout = std::chrono::seconds(5);

bool valid_identifier(const std::string& value)
{
    return !value.empty() && value.size() <= 15 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch > 0 && ch <= 0x7f;
        });
}

using Slots = std::vector<std::tuple<std::string, uint32_t, uint32_t,
                                     uint32_t, uint32_t, uint32_t>>;

Slots workload_slots(const timpani::node::v1::HierarchicalScheduleTable& table,
                     const std::string& workload)
{
    Slots result;
    for (const auto& partition : table.partitions()) {
        for (const auto& layer : partition.layers()) {
            for (const auto& slot : layer.tt_slots()) {
                if (slot.workload_id() == workload) {
                    result.emplace_back(slot.task_id(), slot.offset_us(),
                                        slot.duration_us(), slot.deadline_us(),
                                        slot.cpu(), slot.current_limit());
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool same_parameters(const SchedInfo& left, const SchedInfo& right)
{
    std::map<std::string, std::string> a;
    std::map<std::string, std::string> b;
    for (const auto& task : left.tasks()) a.emplace(task.name(), task.SerializeAsString());
    for (const auto& task : right.tasks()) b.emplace(task.name(), task.SerializeAsString());
    return left.temporal_class() == right.temporal_class() && a == b;
}

bool expand_phase0_table(
    timpani::node::v1::HierarchicalScheduleTable& table,
    uint32_t execution_frame_us, std::string& error)
{
    const uint32_t minimum_cycle_us = table.hyperperiod_us();
    if (minimum_cycle_us == 0 || execution_frame_us == 0 ||
        execution_frame_us % minimum_cycle_us != 0) {
        error = "generated hyperperiod does not divide the confirmed grid";
        return false;
    }

    const uint32_t repeats = execution_frame_us / minimum_cycle_us;
    size_t expanded_slots = 0;
    for (const auto& partition : table.partitions()) {
        for (const auto& layer : partition.layers()) {
            const size_t slots = static_cast<size_t>(layer.tt_slots_size());
            if (slots > kMaxTtSlots / repeats ||
                expanded_slots > kMaxTtSlots - slots * repeats) {
                error = "expanded TT slot capacity exceeds 2048";
                return false;
            }
            expanded_slots += slots * repeats;
        }
    }
    if (repeats == 1) return true;

    for (auto& partition : *table.mutable_partitions()) {
        for (auto& layer : *partition.mutable_layers()) {
            std::vector<timpani::node::v1::TtSlot> expanded;
            expanded.reserve(static_cast<size_t>(layer.tt_slots_size()) * repeats);
            for (const auto& original : layer.tt_slots()) {
                for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
                    auto slot = original;
                    const uint64_t offset = static_cast<uint64_t>(repeat) *
                                                minimum_cycle_us +
                                            original.offset_us();
                    if (offset > std::numeric_limits<uint32_t>::max()) {
                        error = "expanded slot offset exceeds uint32";
                        return false;
                    }
                    slot.set_offset_us(static_cast<uint32_t>(offset));
                    expanded.push_back(std::move(slot));
                }
            }
            std::sort(expanded.begin(), expanded.end(),
                      [](const auto& left, const auto& right) {
                          return std::tuple(left.offset_us(), left.cpu(),
                                            left.workload_id(), left.task_id()) <
                                 std::tuple(right.offset_us(), right.cpu(),
                                            right.workload_id(), right.task_id());
                      });
            layer.clear_tt_slots();
            for (auto& slot : expanded) {
                *layer.add_tt_slots() = std::move(slot);
            }
        }
    }
    table.set_hyperperiod_us(execution_frame_us);
    return true;
}

} // namespace

bool SchedInfoServer::EnablePhase0(timpani::orchestrator::OrchestratorServiceImpl& orchestrator)
{
    std::unique_lock<std::shared_mutex> lock(service_.schedule_mutex_);
    const auto& config = service_.node_config_manager_;
    if (server_ || service_.phase0_ || orchestrator.phase0_ack_ ||
        !service_.workload_tasks_.empty() || !service_.schedule_tables_.empty() ||
        !orchestrator.get_connected_node_ids().empty()) {
        TLOG_ERROR("[RTU-P0] enable rejected: configure once before starting services");
        return false;
    }
    if (!config || !config->IsLoaded() || config->GetAllNodes().size() != 1) {
        TLOG_ERROR("[RTU-P0] enable rejected: one explicitly configured node is required");
        return false;
    }
    const auto& [node_id, node] = *config->GetAllNodes().begin();
    if (node_id.empty() || node.available_cpus.size() != 1 || node.available_cpus.front() < 0) {
        TLOG_ERROR("[RTU-P0] enable rejected: one configured isolated CPU is required");
        return false;
    }
    auto state = std::make_shared<SchedInfoServiceImpl::Phase0State>();
    state->node_id = node_id;
    state->cpu = node.available_cpus.front();
    // Callbacks share only the state, not the service lifetime.
    orchestrator.phase0_node_id_ = node_id;
    orchestrator.phase0_ack_ = [state](const auto& node, const auto& ack) {
        SchedInfoServiceImpl::HandlePhase0Ack(*state, node, ack);
    };
    orchestrator.phase0_abort_ = [state](const auto& reason) {
        SchedInfoServiceImpl::AbortPhase0(*state, reason);
    };
    service_.phase0_ = std::move(state);
    phase0_orchestrator_ = &orchestrator;
    TLOG_INFO("[RTU-P0] enabled node_id=", node_id, " cpu=", node.available_cpus.front());
    return true;
}

void SchedInfoServer::PushPendingTables()
{
    if (!phase0_orchestrator_ || !service_.phase0_) {
        TLOG_ERROR("[RTU-P0] PushPendingTables requires EnablePhase0");
        return;
    }
    auto& state = *service_.phase0_;
    const auto connected = phase0_orchestrator_->get_connected_node_ids();
    if (std::find(connected.begin(), connected.end(), state.node_id) == connected.end()) {
        return;
    }
    SchedInfoServiceImpl::Phase0Table table;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.stopped || !state.attempt || state.attempt->sent) return;
        state.attempt->sent = true;
        table = state.attempt->candidate.table;
    }
    // An ACK can arrive during Write; publish sent first and release the state lock.
    if (!phase0_orchestrator_->push_full_table(state.node_id, table)) {
        SchedInfoServiceImpl::AbortPhase0(state, "full_table write failed; outcome unverified");
    }
}

const char* SchedInfoServiceImpl::Phase0OperationName(Phase0Operation operation)
{
    switch (operation) {
        case Phase0Operation::Replace: return "REPLACE";
        case Phase0Operation::Add: return "ADD";
        case Phase0Operation::Remove: return "REMOVE";
        case Phase0Operation::Clear: return "CLEAR";
    }
    return "UNKNOWN";
}

bool SchedInfoServiceImpl::ValidatePhase0Workloads(
    Phase0Workloads& workloads, std::vector<ClassifiedTask>& tasks, std::string& error) const
{
    std::set<std::string> names;
    std::map<uint64_t, std::string> workload_hashes;
    std::map<uint64_t, std::string> task_hashes;
    uint32_t hyperperiod = 0;
    for (auto& [id, workload] : workloads) {
        if (!valid_identifier(id) || workload.tasks().empty() ||
            workload.temporal_class() != TEMPORAL_PERIODIC) {
            error = "expected TT workload, tasks and 1..15-byte ASCII workload ID";
            return false;
        }
        if (!workload_hashes.emplace(GlobalScheduler::fnv1a_hash(id.c_str()), id).second) {
            error = "workload hash collision";
            return false;
        }
        if (tasks.size() + static_cast<size_t>(workload.tasks_size()) > kMaxTasks) {
            error = "unique task capacity exceeds 32";
            return false;
        }
        for (auto& task : *workload.mutable_tasks()) {
            if (!valid_identifier(task.name()) || !names.insert(task.name()).second ||
                !task_hashes.emplace(GlobalScheduler::fnv1a_hash(task.name().c_str()),
                                     task.name()).second) {
                error = "invalid, duplicate or colliding task ID";
                return false;
            }
            const auto owner = phase0_->task_workloads.find(task.name());
            if (owner != phase0_->task_workloads.end() && owner->second != id) {
                error = "task identity belongs to another workload in this trial";
                return false;
            }
            if (!task.node_id().empty() && task.node_id() != phase0_->node_id) {
                error = "task targets a different node";
                return false;
            }
            if (task.period() <= 0 || task.runtime() <= 0 || task.deadline() < 0 ||
                task.max_dmiss() < 0 || task.release_time() != 0) {
                error = "invalid timing/limit or unsupported release time";
                return false;
            }
            task.set_node_id(phase0_->node_id);
            if (task.deadline() == 0) task.set_deadline(task.period());
            if (task.runtime() > task.deadline() || task.deadline() > task.period()) {
                error = "expected runtime <= deadline <= period";
                return false;
            }
            ClassifiedTask classified;
            classified.workload_id = id;
            classified.task_id = task.name();
            classified.mechanism = Mechanism::TT;
            classified.period_us = static_cast<uint32_t>(task.period());
            classified.wcet_us = static_cast<uint32_t>(task.runtime());
            classified.deadline_us = static_cast<uint32_t>(task.deadline());
            classified.max_dmiss = static_cast<uint32_t>(task.max_dmiss());
            tasks.push_back(std::move(classified));
            hyperperiod = std::max(hyperperiod, static_cast<uint32_t>(task.period()));
        }
    }
    // For harmonic inputs H=max(period); the generator rejects non-harmonic input.
    uint64_t slots = 0;
    for (const auto& task : tasks) slots += hyperperiod / task.period_us;
    if (slots > kMaxTtSlots) {
        error = "TT slot capacity exceeds 2048";
        return false;
    }
    return true;
}

bool SchedInfoServiceImpl::ValidatePhase0Table(const Phase0Table& table, std::string& error) const
{
    const uint64_t h = table.hyperperiod_us();
    if (table.table_id().empty() || table.node_id() != phase0_->node_id || h == 0 ||
        h > std::numeric_limits<uint64_t>::max() / 1000 ||
        table.epoch_ns() > std::numeric_limits<uint64_t>::max() - h * 1000) {
        error = "invalid table identity or unrepresentable time grid";
        return false;
    }
    size_t slots = 0;
    for (const auto& partition : table.partitions()) {
        if (!partition.cpuset().isolated() || partition.cpuset().cpus_size() != 1 ||
            partition.cpuset().cpus(0) != static_cast<uint32_t>(phase0_->cpu)) {
            error = "generated table changes the isolated CPU";
            return false;
        }
        for (const auto& layer : partition.layers()) {
            if (layer.model() != timpani::node::v1::TIME_TRIGGERED ||
                !layer.cbs_entries().empty()) {
                error = "generated table is not TT-only";
                return false;
            }
            for (const auto& slot : layer.tt_slots()) {
                if (slot.duration_us() == 0 || slot.deadline_us() == 0 ||
                    uint64_t{slot.offset_us()} + slot.duration_us() > h ||
                    slot.cpu() != static_cast<uint32_t>(phase0_->cpu)) {
                    error = "generated slot is outside the fixed grid";
                    return false;
                }
                ++slots;
            }
        }
    }
    if (slots > kMaxTtSlots) {
        error = "generated TT slot capacity exceeds 2048";
        return false;
    }
    return true;
}

bool SchedInfoServiceImpl::PreparePhase0Candidate(
    Phase0Operation operation, const std::vector<SchedInfo>& inputs,
    const std::vector<std::string>& removals, OAttempt& attempt, std::string& error)
{
    auto& state = *phase0_;
    if (node_config_manager_->GetAllNodes().size() != 1 ||
        node_config_manager_->GetAvailableCpus(state.node_id) != std::vector<int>{state.cpu}) {
        error = "node/CPU configuration changed during the trial";
        return false;
    }
    auto workloads = state.confirmed ? state.confirmed->workloads : Phase0Workloads{};
    if (operation == Phase0Operation::Replace || operation == Phase0Operation::Clear) {
        workloads.clear();
    }
    if ((operation == Phase0Operation::Add && inputs.empty()) ||
        (operation == Phase0Operation::Remove && removals.empty()) ||
        ((operation == Phase0Operation::Remove || operation == Phase0Operation::Clear) && !inputs.empty()) ||
        (operation != Phase0Operation::Remove && !removals.empty())) {
        error = "invalid operation arguments";
        return false;
    }
    std::string targets;
    auto add_target = [&targets](const std::string& id) {
        if (!targets.empty()) targets += ",";
        targets += id;
    };
    for (const auto& input : inputs) {
        if (!workloads.emplace(input.workload_id(), input).second) {
            error = "duplicate/existing workload; MODIFY is outside Phase 0";
            return false;
        }
        add_target(input.workload_id());
    }
    for (const auto& id : removals) {
        if (workloads.erase(id) == 0) {
            error = "cannot remove unknown/duplicate workload";
            return false;
        }
        add_target(id);
    }
    if (operation == Phase0Operation::Clear && state.confirmed) {
        for (const auto& [id, workload] : state.confirmed->workloads) add_target(id);
    }
    std::vector<ClassifiedTask> tasks;
    if (!ValidatePhase0Workloads(workloads, tasks, error)) return false;
    if (state.confirmed) {
        for (const auto& [id, previous] : state.confirmed->workloads) {
            auto retained = workloads.find(id);
            if (retained != workloads.end() && !same_parameters(previous, retained->second)) {
                error = "retained workload parameters changed; MODIFY is outside Phase 0";
                return false;
            }
        }
    }
    Phase0Table table;
    if (workloads.empty()) {
        if (!state.confirmed) {
            error = "empty snapshot requires an ACK-confirmed time grid";
            return false;
        }
        table = state.confirmed->table;
        table.clear_partitions();
    } else {
        auto result = global_scheduler_->generate_schedule(state.node_id, tasks);
        if (std::holds_alternative<InfeasibleError>(result)) {
            error = std::get<InfeasibleError>(result).details;
            return false;
        }
        table = std::move(std::get<Phase0Table>(result));
        if (state.confirmed &&
            !expand_phase0_table(table,
                                 state.confirmed->table.hyperperiod_us(),
                                 error)) {
            return false;
        }
    }
    if (state.confirmed) {
        table.set_epoch_ns(state.confirmed->table.epoch_ns());
        if (operation == Phase0Operation::Add || operation == Phase0Operation::Remove) {
            for (const auto& [id, previous] : state.confirmed->workloads) {
                if (workloads.count(id) &&
                    workload_slots(table, id) != workload_slots(state.confirmed->table, id)) {
                    error = "generated schedule moves retained workload slots";
                    return false;
                }
            }
        }
    }
    if (state.next_table_id == std::numeric_limits<uint64_t>::max()) {
        error = "table sequence exhausted";
        return false;
    }
    table.set_table_id("rtu-p0-" + std::to_string(state.next_table_id));
    if (!ValidatePhase0Table(table, error)) return false;
    ++state.next_table_id;
    attempt = {{std::move(workloads), std::move(table)}, operation, std::move(targets), false};
    return true;
}

bool SchedInfoServiceImpl::SubmitPhase0Update(
    Phase0Operation operation, const std::vector<SchedInfo>& inputs,
    const std::vector<std::string>& removals,
    std::shared_ptr<OAttemptCompletion> completion)
{
    auto& state = *phase0_;
    std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        TLOG_WARN("[RTU-P0] node_id=", state.node_id, " op=", Phase0OperationName(operation),
                  " result=rejected reason=busy");
        return false;
    }
    std::string error;
    OAttempt candidate;
    bool prepared = false;
    if (state.stopped) error = "trial stopped; result/state is unverified";
    else if (state.attempt) error = "busy: update in progress";
    else prepared = PreparePhase0Candidate(operation, inputs, removals, candidate, error);
    if (!prepared) {
        TLOG_WARN("[RTU-P0] node_id=", state.node_id, " op=", Phase0OperationName(operation),
                  " result=rejected reason=", error);
        return false;
    }
    candidate.completion = std::move(completion);
    state.attempt = std::move(candidate);
    TLOG_INFO("[RTU-P0] node_id=", state.node_id,
              " table_id=", state.attempt->candidate.table.table_id(),
              " op=", Phase0OperationName(operation), " target_workloads=", state.attempt->targets,
              " result=staged");
    return true;
}

void SchedInfoServiceImpl::HandlePhase0Ack(
    Phase0State& state, const std::string& node_id, const timpani::node::v1::TableApplied& applied)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.stopped || !state.attempt || !state.attempt->sent || node_id != state.node_id ||
        applied.table_id() != state.attempt->candidate.table.table_id()) {
        state.stopped = true;
        TLOG_ERROR("[RTU-P0] node_id=", node_id, " table_id=", applied.table_id(),
                   " result=trial_failed reason=unexpected ACK or stopped trial");
        state.settled.notify_all();
        return;
    }
    TLOG_INFO("[RTU-P0] node_id=", node_id, " table_id=", applied.table_id(),
              " op=", Phase0OperationName(state.attempt->operation),
              " target_workloads=", state.attempt->targets,
              " result=", applied.success() ? "confirmed" : "rejected",
              " reason=", applied.error());
    if (applied.success()) {
        for (const auto& [id, workload] : state.attempt->candidate.workloads) {
            for (const auto& task : workload.tasks()) state.task_workloads.emplace(task.name(), id);
        }
        state.confirmed = std::move(state.attempt->candidate);
        state.changed = true;
    }
    if (state.attempt->completion) {
        state.attempt->completion->completed = true;
        state.attempt->completion->success = applied.success();
    }
    state.attempt.reset();
    state.settled.notify_all();
}

void SchedInfoServiceImpl::AbortPhase0(Phase0State& state, const std::string& reason)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.stopped = true;
    TLOG_ERROR("[RTU-P0] node_id=", state.node_id, " table_id=",
               state.attempt ? state.attempt->candidate.table.table_id() : "",
               " result=trial_failed reason=", reason);
    state.settled.notify_all();
}

bool SchedInfoServiceImpl::RemovePhase0Workload(
    const std::string& workload_id, std::string* resolved_id, bool trigger_push)
{
    auto& state = *phase0_;
    std::string actual_id;
    {
        std::unique_lock<std::mutex> lock(state.mutex, std::defer_lock);
        if (trigger_push) {
            if (!lock.try_lock()) {
                TLOG_WARN("[RTU-P0] op=REMOVE result=rejected reason=busy");
                return false;
            }
        } else {
            lock.lock();
        }
        uint64_t hash = 0;
        const bool hex = workload_id.rfind("0x", 0) == 0;
        const char* end = workload_id.data() + workload_id.size();
        const auto parsed = std::from_chars(workload_id.data() + (hex ? 2 : 0), end,
                                            hash, hex ? 16 : 10);
        auto resolve = [&](const Phase0Workloads& workloads) {
            for (const auto& [name, workload] : workloads) {
                if (name == workload_id ||
                    (parsed.ec == std::errc{} && parsed.ptr == end &&
                     GlobalScheduler::fnv1a_hash(name.c_str()) == hash)) {
                    actual_id = name;
                    break;
                }
            }
        };
        if (state.confirmed) resolve(state.confirmed->workloads);
        if (actual_id.empty() && state.attempt) resolve(state.attempt->candidate.workloads);
    }
    if (actual_id.empty()) {
        TLOG_WARN("[RTU-P0] unknown workload for removal: ", workload_id);
        return false;
    }
    if (resolved_id) *resolved_id = actual_id;
    auto completion =
        trigger_push ? nullptr : std::make_shared<OAttemptCompletion>();
    if (!SubmitPhase0Update(Phase0Operation::Remove, {}, {actual_id},
                           completion)) {
        return false;
    }
    if (trigger_push) return true;
    std::unique_lock<std::mutex> lock(state.mutex);
    const bool settled = state.settled.wait_for(
        lock, kRecoveryStopAckTimeout, [&state, &completion] {
            return state.stopped || completion->completed;
        });
    const bool removed = settled && !state.stopped && completion->completed &&
                         completion->success;
    const char* reply = removed ? "confirmed" : settled ? "failed" : "timeout";
    if (removed) {
        TLOG_INFO("[RTU-P0] node_id=", state.node_id, " recovery=STOP target_workloads=",
                  actual_id, " reply=", reply);
    } else {
        TLOG_WARN("[RTU-P0] node_id=", state.node_id, " recovery=STOP target_workloads=",
                  actual_id, " reply=", reply);
    }
    return removed;
}
