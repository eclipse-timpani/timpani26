/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <limits>
#include <set>
#include <thread>
#include <tuple>

#include "schedinfo_service.h"
#include "orchestrator_service.h"
#include "tlog.h"

using Table = timpani::node::v1::HierarchicalScheduleTable;
using Applied = timpani::node::v1::TableApplied;

class Phase0TestPeer {
public:
    static SchedInfoServiceImpl& Service(SchedInfoServer& server) { return server.service_; }
    static bool Replace(SchedInfoServiceImpl& service, const std::vector<SchedInfo>& inputs) {
        return service.SubmitPhase0Update(SchedInfoServiceImpl::Phase0Operation::Replace, inputs);
    }
    static bool Clear(SchedInfoServiceImpl& service) {
        return service.SubmitPhase0Update(SchedInfoServiceImpl::Phase0Operation::Clear, {});
    }
    static bool Pending(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->attempt.has_value();
    }
    static bool Sent(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->attempt && service.phase0_->attempt->sent;
    }
    static bool Stopped(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->stopped;
    }
    static void SettleAndInstallUnrelatedAttempt(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        ASSERT_TRUE(service.phase0_->attempt);
        ASSERT_TRUE(service.phase0_->attempt->completion);
        service.phase0_->confirmed = service.phase0_->attempt->candidate;
        service.phase0_->attempt->completion->completed = true;
        service.phase0_->attempt->completion->success = true;
        service.phase0_->attempt.reset();

        SchedInfoServiceImpl::OAttempt unrelated;
        unrelated.candidate = *service.phase0_->confirmed;
        unrelated.operation = SchedInfoServiceImpl::Phase0Operation::Add;
        unrelated.targets = "unrelated";
        unrelated.sent = false;
        service.phase0_->attempt = std::move(unrelated);
        service.phase0_->settled.notify_all();
    }
    static std::map<std::string, std::string> ConfirmedInputs(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        std::map<std::string, std::string> result;
        if (service.phase0_->confirmed) {
            for (const auto& [id, input] : service.phase0_->confirmed->workloads) {
                result.emplace(id, input.SerializeAsString());
            }
        }
        return result;
    }
    static Table Candidate(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->attempt->candidate.table;
    }
    static bool Validate(SchedInfoServiceImpl& service, const Table& table) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        std::string error;
        const bool valid = service.ValidatePhase0Table(table, error);
        EXPECT_EQ(valid, error.empty());
        return valid;
    }
    static void ExhaustIds(SchedInfoServiceImpl& service) {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        service.phase0_->next_table_id = std::numeric_limits<uint64_t>::max();
    }
    static std::unique_lock<std::mutex> Lock(SchedInfoServiceImpl& service) {
        return std::unique_lock<std::mutex>(service.phase0_->mutex);
    }
};

namespace {

SchedInfo workload(const std::string& id, const std::string& name,
                   int period, int runtime, int deadline)
{
    SchedInfo result;
    result.set_workload_id(id);
    auto* task = result.add_tasks();
    task->set_name(name);
    task->set_period(period);
    task->set_runtime(runtime);
    task->set_deadline(deadline);
    task->set_max_dmiss(3);
    task->set_node_id("node1");
    return result;
}

SchedInfo w1()
{
    auto result = workload("W1", "w1_fast", 10000, 1000, 2000);
    *result.add_tasks() = workload("W1", "w1_anchor", 20000, 8000, 9000).tasks(0);
    return result;
}
SchedInfo w2() { return workload("W2", "w2_task", 20000, 1000, 10000); }
SchedInfo w3() { return workload("W3", "w3_task", 20000, 2000, 20000); }

using Slot = std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t>;
std::vector<Slot> slots(const Table& table, const std::string& id)
{
    std::vector<Slot> result;
    for (const auto& partition : table.partitions()) {
        for (const auto& layer : partition.layers()) {
            for (const auto& slot : layer.tt_slots()) {
                if (slot.workload_id() == id) {
                    result.emplace_back(slot.task_id(), slot.offset_us(), slot.duration_us(),
                                        slot.deadline_us(), slot.cpu());
                }
            }
        }
    }
    return result;
}

template<typename Predicate>
bool wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

} // namespace

class Phase0Test : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = std::make_shared<NodeConfigManager>();
        ASSERT_TRUE(config_->LoadFromFile(PHASE0_NODE_CONFIG));
        server_ = std::make_unique<SchedInfoServer>(config_);
        ASSERT_TRUE(server_->EnablePhase0(orchestrator_));
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&Service());
        builder.RegisterService(&orchestrator_);
        grpc_server_ = builder.BuildAndStart();
        ASSERT_NE(grpc_server_, nullptr);
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
        sched_stub_ = SchedInfoService::NewStub(channel_);
        node_stub_ = timpani::node::v1::OrchestratorService::NewStub(channel_);
        node_context_.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        stream_ = node_stub_->NodeStream(&node_context_);
        timpani::node::v1::NodeEvent event;
        event.set_node_id("node1");
        event.mutable_ready()->set_cpu_count(4);
        ASSERT_TRUE(stream_->Write(event));
        ASSERT_TRUE(wait_until([&] { return !orchestrator_.get_connected_node_ids().empty(); }));
    }
    void TearDown() override {
        node_context_.TryCancel();
        if (stream_) {
            stream_->WritesDone();
            stream_->Finish();
            stream_.reset();
        }
        if (grpc_server_) {
            grpc_server_->Shutdown(std::chrono::system_clock::now());
            grpc_server_->Wait();
        }
    }
    SchedInfoServiceImpl& Service() { return Phase0TestPeer::Service(*server_); }
    int Add(const SchedInfo& input) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        Response response;
        EXPECT_TRUE(sched_stub_->AddSchedInfo(&context, input, &response).ok());
        return response.status();
    }
    Table Send() {
        server_->PushPendingTables();
        timpani::node::v1::ControlCommand command;
        EXPECT_TRUE(stream_->Read(&command));
        EXPECT_TRUE(command.has_full_table());
        sent_.push_back(command.full_table());
        return command.full_table();
    }
    void WriteAck(const std::string& id, bool success = true, const std::string& node = "node1") {
        timpani::node::v1::NodeEvent event;
        event.set_node_id(node);
        event.mutable_applied()->set_table_id(id);
        event.mutable_applied()->set_success(success);
        if (!success) event.mutable_applied()->set_error("fixture pre-activation rejection");
        ASSERT_TRUE(stream_->Write(event));
    }
    void Ack(const Table& table, bool success = true) {
        WriteAck(table.table_id(), success);
        ASSERT_TRUE(wait_until([&] { return !Phase0TestPeer::Pending(Service()); }));
    }
    void Seed() {
        ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {w1(), w2()}));
        Ack(Send());
    }
    Table Confirmed() { return server_->GetScheduleTables().at("node1"); }

    std::shared_ptr<NodeConfigManager> config_;
    timpani::orchestrator::OrchestratorServiceImpl orchestrator_;
    std::unique_ptr<SchedInfoServer> server_;
    std::unique_ptr<grpc::Server> grpc_server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<SchedInfoService::Stub> sched_stub_;
    std::unique_ptr<timpani::node::v1::OrchestratorService::Stub> node_stub_;
    grpc::ClientContext node_context_;
    std::unique_ptr<grpc::ClientReaderWriter<timpani::node::v1::NodeEvent,
                                             timpani::node::v1::ControlCommand>> stream_;
    std::vector<Table> sent_;
};

TEST_F(Phase0Test, OnlyMatchingAckConfirmsAndChangesNotification)
{
    ASSERT_EQ(Add(w1()), 0);
    EXPECT_FALSE(Phase0TestPeer::Sent(Service()));
    bool changed = true;
    EXPECT_TRUE(server_->GetScheduleTables(&changed).empty());
    EXPECT_FALSE(changed);
    EXPECT_TRUE(Phase0TestPeer::ConfirmedInputs(Service()).empty());
    const auto table = Send();
    EXPECT_TRUE(server_->GetScheduleTables().empty());
    EXPECT_TRUE(Phase0TestPeer::Sent(Service()));
    Ack(table);
    EXPECT_EQ(server_->GetScheduleTables(&changed).at("node1").SerializeAsString(),
              table.SerializeAsString());
    EXPECT_TRUE(changed);
    server_->GetScheduleTables(&changed);
    EXPECT_FALSE(changed);
}

TEST_F(Phase0Test, FourOperationsPreserveGridSlotsAndRepetitionsAfterClear)
{
    Seed();
    const auto baseline = Confirmed();
    const std::vector<Slot> expected = {
        {"w1_fast", 0, 1000, 2000, 2},
        {"w1_anchor", 1000, 8000, 9000, 2},
        {"w1_fast", 10000, 1000, 2000, 2}};
    EXPECT_EQ(baseline.hyperperiod_us(), 20000u);
    EXPECT_EQ(slots(baseline, "W1"), expected);
    EXPECT_EQ(slots(baseline, "W2"), (std::vector<Slot>{{"w2_task", 9000, 1000, 10000, 2}}));
    ASSERT_EQ(Add(w3()), 0);
    auto added = Send();
    EXPECT_EQ(slots(added, "W1"), expected);
    EXPECT_EQ(slots(added, "W2"), slots(baseline, "W2"));
    EXPECT_EQ(slots(added, "W3"), (std::vector<Slot>{{"w3_task", 11000, 2000, 20000, 2}}));
    Ack(added);
    ASSERT_TRUE(server_->RemoveWorkload("W2"));
    const auto removed = Send();
    EXPECT_EQ(slots(removed, "W1"), expected);
    EXPECT_EQ(slots(removed, "W3"), slots(added, "W3"));
    EXPECT_TRUE(slots(removed, "W2").empty());
    Ack(removed);
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {w1(), w2()}));
    Ack(Send());
    ASSERT_TRUE(Phase0TestPeer::Clear(Service()));
    auto cleared = Send();
    EXPECT_EQ(cleared.partitions_size(), 0);
    Ack(cleared);
    EXPECT_TRUE(Phase0TestPeer::ConfirmedInputs(Service()).empty());
    ASSERT_EQ(Add(w1()), 0);
    auto restored = Send();
    EXPECT_EQ(slots(restored, "W1"), expected);
    Ack(restored);
    ASSERT_TRUE(Phase0TestPeer::Clear(Service()));
    Ack(Send());
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {w1()}));
    EXPECT_EQ(slots(Send(), "W1"), expected);
    std::set<std::string> ids;
    for (const auto& table : sent_) {
        EXPECT_FALSE(table.table_id().empty());
        EXPECT_TRUE(ids.insert(table.table_id()).second);
        EXPECT_EQ(table.epoch_ns(), baseline.epoch_ns());
        EXPECT_EQ(table.hyperperiod_us(), baseline.hyperperiod_us());
    }
}

TEST_F(Phase0Test, GenerationAndNodeRejectionPreserveConfirmedBytesAndInputs)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    const auto inputs = Phase0TestPeer::ConfirmedInputs(Service());
    EXPECT_EQ(Add(workload("bad", "overload", 20000, 19000, 20000)), -1);
    EXPECT_FALSE(Phase0TestPeer::Pending(Service()));
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    EXPECT_EQ(Phase0TestPeer::ConfirmedInputs(Service()), inputs);
    ASSERT_EQ(Add(w3()), 0);
    const auto rejected = Send();
    Ack(rejected, false);
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    EXPECT_EQ(Phase0TestPeer::ConfirmedInputs(Service()), inputs);
    EXPECT_FALSE(server_->RemoveWorkload("W3"));
    ASSERT_EQ(Add(w3()), 0);
    const auto next = Send();
    EXPECT_NE(next.table_id(), rejected.table_id());
    Ack(next);
}

TEST_F(Phase0Test, ExistingModificationDoesNotLoseOriginalInput)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    auto modified = w1();
    modified.mutable_tasks(0)->set_runtime(1500);
    EXPECT_EQ(Add(modified), -1);
    EXPECT_FALSE(Phase0TestPeer::Replace(Service(), {modified, w2()}));
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    ASSERT_EQ(Add(w3()), 0);
    EXPECT_EQ(slots(Send(), "W1"), slots(Confirmed(), "W1"));
}

TEST_F(Phase0Test, ExpandsDivisorHyperperiodWithoutOverwritingConfirmedGrid)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    const auto longer = workload("long", "long_task", 40000, 100, 40000);
    const auto shorter = workload("short", "short_task", 10000, 100, 10000);
    EXPECT_EQ(Add(longer), -1);
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {shorter}));
    const auto replaced = Send();
    EXPECT_EQ(replaced.hyperperiod_us(), 20000u);
    EXPECT_EQ(slots(replaced, "short"), (std::vector<Slot>{
        {"short_task", 0, 100, 10000, 2},
        {"short_task", 10000, 100, 10000, 2}}));
    Ack(replaced);
    ASSERT_TRUE(Phase0TestPeer::Clear(Service()));
    Ack(Send());
    ASSERT_EQ(Add(shorter), 0);
    const auto restored = Send();
    EXPECT_EQ(restored.hyperperiod_us(), 20000u);
    EXPECT_EQ(slots(restored, "short"), slots(replaced, "short"));
}

TEST_F(Phase0Test, RemoveAndClearExpandSmallerMinimumCyclesOverConfirmedFrame)
{
    const auto short_task = workload("short", "short_task", 20000, 100, 20000);
    const auto long_task = workload("long", "long_task", 40000, 100, 40000);
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {short_task, long_task}));
    const auto baseline = Send();
    ASSERT_EQ(baseline.hyperperiod_us(), 40000u);
    ASSERT_EQ(slots(baseline, "short").size(), 2u);
    Ack(baseline);

    ASSERT_TRUE(server_->RemoveWorkload("long"));
    const auto removed = Send();
    EXPECT_EQ(removed.hyperperiod_us(), 40000u);
    EXPECT_EQ(slots(removed, "short"), slots(baseline, "short"));
    Ack(removed);

    ASSERT_TRUE(Phase0TestPeer::Clear(Service()));
    Ack(Send());
    ASSERT_EQ(Add(short_task), 0);
    const auto restored = Send();
    EXPECT_EQ(restored.hyperperiod_us(), 40000u);
    EXPECT_EQ(slots(restored, "short"), slots(baseline, "short"));
}

TEST_F(Phase0Test, RejectsAddAndRemoveThatMoveRetainedSlots)
{
    auto later = workload("later", "later_task", 10000, 1000, 10000);
    auto earlier = workload("earlier", "earlier_task", 10000, 1000, 5000);
    ASSERT_EQ(Add(later), 0);
    Ack(Send());
    EXPECT_EQ(Add(earlier), -1);
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {earlier, later}));
    Ack(Send());
    const auto before = Confirmed().SerializeAsString();
    EXPECT_FALSE(server_->RemoveWorkload("earlier"));
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
}

TEST_F(Phase0Test, BusyRejectsAllOperationsBeforeAndAfterTransmission)
{
    Seed();
    ASSERT_EQ(Add(w3()), 0);
    auto reject_all = [&] {
        EXPECT_EQ(Add(w3()), -1);
        EXPECT_FALSE(server_->RemoveWorkload("W2"));
        EXPECT_FALSE(Phase0TestPeer::Replace(Service(), {w1()}));
        EXPECT_FALSE(Phase0TestPeer::Clear(Service()));
    };
    reject_all();
    const auto table = Send();
    reject_all();
    for (int i = 0; i < 5; ++i) server_->PushPendingTables();
    Ack(table, false);
    ASSERT_EQ(Add(w3()), 0);
    const auto next = Send();
    EXPECT_NE(next.table_id(), table.table_id()); // No replay of the rejected candidate.
    Ack(next);
}

TEST_F(Phase0Test, BusyDoesNotWaitForPreparationLock)
{
    auto lock = Phase0TestPeer::Lock(Service());
    auto request = std::async(std::launch::async, [&] { return Add(w1()); });
    const auto status = request.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(request.get(), -1);
}

TEST_F(Phase0Test, ConcurrentRequestsAcceptOnlyOneCandidate)
{
    Seed();
    auto first = std::async(std::launch::async, [&] { return Add(w3()); });
    auto second = std::async(std::launch::async, [&] { return Add(w3()); });
    const auto a = first.get();
    const auto b = second.get();
    EXPECT_TRUE((a == 0 && b == -1) || (a == -1 && b == 0));
    Ack(Send());
}

TEST_F(Phase0Test, AckBeforeTransmissionCannotConfirm)
{
    ASSERT_EQ(Add(w1()), 0);
    WriteAck(Phase0TestPeer::Candidate(Service()).table_id());
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Stopped(Service()); }));
    EXPECT_TRUE(server_->GetScheduleTables().empty());
    EXPECT_EQ(Add(w2()), -1);
}

TEST_F(Phase0Test, StaleAckStopsTrialWithoutConfirmingCandidate)
{
    Seed();
    const auto before = Confirmed();
    ASSERT_EQ(Add(w3()), 0);
    const auto pending = Send();
    WriteAck(before.table_id());
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Stopped(Service()); }));
    WriteAck(pending.table_id());
    EXPECT_EQ(Confirmed().SerializeAsString(), before.SerializeAsString());
    EXPECT_EQ(Add(w3()), -1);
}

TEST_F(Phase0Test, ChangedStreamIdentityCannotConfirm)
{
    ASSERT_EQ(Add(w1()), 0);
    const auto table = Send();
    WriteAck(table.table_id(), true, "other");
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Stopped(Service()); }));
    EXPECT_TRUE(server_->GetScheduleTables().empty());
    EXPECT_EQ(Add(w2()), -1);
}

TEST_F(Phase0Test, DisconnectStopsSubmissionsWithoutChangingConfirmedState)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    ASSERT_EQ(Add(w3()), 0);
    Send();
    node_context_.TryCancel();
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Stopped(Service()); }));
    server_->PushPendingTables();
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    EXPECT_EQ(Add(w3()), -1);
}

TEST_F(Phase0Test, DuplicateStreamCannotReplaceCurrentConnection)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto duplicate = node_stub_->NodeStream(&context);
    timpani::node::v1::NodeEvent event;
    event.set_node_id("node1");
    event.mutable_ready();
    ASSERT_TRUE(duplicate->Write(event));
    duplicate->WritesDone();
    EXPECT_EQ(duplicate->Finish().error_code(), grpc::StatusCode::ALREADY_EXISTS);
    ASSERT_EQ(Add(w1()), 0);
    Ack(Send());
}

TEST_F(Phase0Test, ClearNeedsBaselineAndLastRemovalPreservesIt)
{
    EXPECT_FALSE(Phase0TestPeer::Clear(Service()));
    EXPECT_FALSE(Phase0TestPeer::Replace(Service(), {}));
    ASSERT_EQ(Add(w1()), 0);
    Ack(Send(), false);
    EXPECT_FALSE(Phase0TestPeer::Clear(Service()));
    ASSERT_EQ(Add(w1()), 0);
    Ack(Send());
    const auto before = Confirmed();
    ASSERT_TRUE(server_->RemoveWorkload("W1"));
    const auto empty = Send();
    EXPECT_EQ(empty.partitions_size(), 0);
    EXPECT_EQ(empty.hyperperiod_us(), before.hyperperiod_us());
    EXPECT_EQ(empty.epoch_ns(), before.epoch_ns());
    Ack(empty);
}

TEST_F(Phase0Test, RejectsInvalidInputsBeforeStaging)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    std::vector<SchedInfo> invalid;
    auto append = [&](const std::function<void(SchedInfo&)>& mutate) {
        auto input = w3();
        mutate(input);
        invalid.push_back(std::move(input));
    };
    append([](auto& w) { w.set_workload_id(""); });
    append([](auto& w) { w.set_workload_id(std::string(16, 'w')); });
    append([](auto& w) { w.set_workload_id("\xc3\xa9"); });
    append([](auto& w) { w.set_temporal_class(TEMPORAL_SPORADIC); });
    append([](auto& w) { w.clear_tasks(); });
    append([](auto& w) { *w.add_tasks() = w.tasks(0); });
    append([](auto& w) { w.mutable_tasks(0)->set_name("w1_fast"); });
    append([](auto& w) { w.mutable_tasks(0)->set_name(std::string(16, 't')); });
    append([](auto& w) { w.mutable_tasks(0)->set_node_id("other"); });
    append([](auto& w) { w.mutable_tasks(0)->set_period(0); });
    append([](auto& w) { w.mutable_tasks(0)->set_period(-1); });
    append([](auto& w) { w.mutable_tasks(0)->set_runtime(0); });
    append([](auto& w) { w.mutable_tasks(0)->set_runtime(-1); });
    append([](auto& w) { w.mutable_tasks(0)->set_deadline(-1); });
    append([](auto& w) { w.mutable_tasks(0)->set_deadline(21000); });
    append([](auto& w) { w.mutable_tasks(0)->set_max_dmiss(-1); });
    append([](auto& w) { w.mutable_tasks(0)->set_release_time(1); });
    append([](auto& w) { w.mutable_tasks(0)->set_period(15000); w.mutable_tasks(0)->set_deadline(15000); });
    for (const auto& input : invalid) {
        EXPECT_EQ(Add(input), -1) << input.ShortDebugString();
        EXPECT_EQ(Confirmed().SerializeAsString(), before);
        EXPECT_FALSE(Phase0TestPeer::Pending(Service()));
    }
    EXPECT_EQ(Add(w3()), 0);
}

TEST_F(Phase0Test, EnforcesExactUniqueTaskCapacity)
{
    auto input = workload("capacity", "task0", 10000, 1, 10000);
    for (int i = 1; i < 32; ++i) {
        auto task = input.tasks(0);
        task.set_name("task" + std::to_string(i));
        *input.add_tasks() = task;
    }
    ASSERT_EQ(Add(input), 0);
    const auto table = Send();
    EXPECT_EQ(slots(table, "capacity").size(), 32u);
    Ack(table);
    EXPECT_EQ(Add(workload("extra", "task32", 10000, 1, 10000)), -1);
}

TEST_F(Phase0Test, EnforcesExactRepeatedSlotCapacity)
{
    auto fast = workload("fast", "fast_task", 2, 1, 1);
    auto anchor = workload("anchor", "anchor_task", 4096, 1, 4096);
    EXPECT_FALSE(Phase0TestPeer::Replace(Service(), {fast, anchor}));
    anchor.mutable_tasks(0)->set_period(4094);
    anchor.mutable_tasks(0)->set_deadline(4094);
    ASSERT_TRUE(Phase0TestPeer::Replace(Service(), {fast, anchor}));
    const auto table = Send();
    EXPECT_EQ(slots(table, "fast").size(), 2047u);
    EXPECT_EQ(slots(table, "anchor").size(), 1u);
}

TEST_F(Phase0Test, NormalizesDefaultDeadlineWithoutNarrowingPositiveTime)
{
    const int maximum = std::numeric_limits<int32_t>::max();
    auto input = workload("large", "large_task", maximum, 1, 0);
    input.mutable_tasks(0)->clear_node_id();
    ASSERT_EQ(Add(input), 0);
    const auto table = Send();
    EXPECT_EQ(table.hyperperiod_us(), static_cast<uint32_t>(maximum));
    EXPECT_EQ(std::get<3>(slots(table, "large").front()), static_cast<uint32_t>(maximum));
}

TEST_F(Phase0Test, ValidatesWidenedSlotSumEpochAndSequenceRange)
{
    Seed();
    auto table = Confirmed();
    table.mutable_partitions(0)->mutable_layers(0)->mutable_tt_slots(0)->set_offset_us(
        std::numeric_limits<uint32_t>::max());
    EXPECT_FALSE(Phase0TestPeer::Validate(Service(), table));
    table = Confirmed();
    table.set_epoch_ns(std::numeric_limits<uint64_t>::max() - uint64_t{table.hyperperiod_us()} * 1000);
    EXPECT_TRUE(Phase0TestPeer::Validate(Service(), table));
    table.set_epoch_ns(table.epoch_ns() + 1);
    EXPECT_FALSE(Phase0TestPeer::Validate(Service(), table));
    Phase0TestPeer::ExhaustIds(Service());
    EXPECT_EQ(Add(w3()), -1);
}

TEST_F(Phase0Test, TaskIdentityCannotMoveAfterClearButRejectedInputDoesNotReserveIt)
{
    Seed();
    ASSERT_EQ(Add(w3()), 0);
    Ack(Send(), false);
    auto renamed = w3();
    renamed.set_workload_id("other");
    ASSERT_EQ(Add(renamed), 0);
    Ack(Send());
    ASSERT_TRUE(Phase0TestPeer::Clear(Service()));
    Ack(Send());
    EXPECT_EQ(Add(w3()), -1);
    EXPECT_EQ(Add(w1()), 0);
}

TEST_F(Phase0Test, ChangedConfigurationIsRejected)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    ASSERT_TRUE(config_->LoadFromFile(MULTI_NODE_CONFIG));
    EXPECT_EQ(Add(w3()), -1);
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
}

TEST_F(Phase0Test, RecoveryStopRepliesAfterCorrelatedRemoveAck)
{
    Seed();
    const auto before = Confirmed();
    EXPECT_FALSE(server_->RemoveWorkload("0xnot-a-hash", nullptr, false));
    std::string resolved;
    auto stop = std::async(std::launch::async, [&] {
        return server_->RemoveWorkload(
            std::to_string(GlobalScheduler::fnv1a_hash("W2")), &resolved, false);
    });
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Pending(Service()); }));
    // The next command is the replacement table, not a RecoverySignal.
    const auto removed = Send();
    EXPECT_TRUE(slots(removed, "W2").empty());
    EXPECT_EQ(slots(removed, "W1"), slots(before, "W1"));
    EXPECT_EQ(stop.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    Ack(removed);
    EXPECT_TRUE(stop.get());
    EXPECT_EQ(resolved, "W2");
    EXPECT_EQ(Confirmed().SerializeAsString(), removed.SerializeAsString());
    EXPECT_FALSE(Phase0TestPeer::Stopped(Service()));
    EXPECT_EQ(Add(w3()), 0);
}

TEST_F(Phase0Test, RecoveryStopIgnoresAnUnrelatedLaterAttempt)
{
    Seed();
    auto stop = std::async(std::launch::async, [&] {
        return server_->RemoveWorkload("W2", nullptr, false);
    });
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Pending(Service()); }));
    Phase0TestPeer::SettleAndInstallUnrelatedAttempt(Service());
    EXPECT_TRUE(stop.get());
    EXPECT_TRUE(Phase0TestPeer::Pending(Service()));
    EXPECT_FALSE(Phase0TestPeer::Stopped(Service()));
}

TEST_F(Phase0Test, RecoveryStopFailsWithoutConfirmedRemoval)
{
    Seed();
    const auto before = Confirmed().SerializeAsString();
    auto stop = std::async(std::launch::async, [&] {
        return server_->RemoveWorkload("W2", nullptr, false);
    });
    ASSERT_TRUE(wait_until([&] { return Phase0TestPeer::Pending(Service()); }));
    Ack(Send(), false);
    EXPECT_FALSE(stop.get());
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
    // Removing W1 would move W2's slot: rejected before any table is sent.
    EXPECT_FALSE(server_->RemoveWorkload("W1", nullptr, false));
    EXPECT_FALSE(Phase0TestPeer::Pending(Service()));
    EXPECT_EQ(Confirmed().SerializeAsString(), before);
}

TEST_F(Phase0Test, DeltaPathIsRejectedOnlyInPhase0)
{
    timpani::node::v1::ScheduleTableUpdate update;
    update.set_table_id("delta");
    EXPECT_FALSE(orchestrator_.push_update("node1", update));
    ASSERT_EQ(Add(w1()), 0);
    Ack(Send());
}

TEST(Phase0ConfigurationTest, MissingMultiNodeAndLateActivationAreRejected)
{
    timpani::orchestrator::OrchestratorServiceImpl orchestrator;
    SchedInfoServer missing;
    EXPECT_FALSE(missing.EnablePhase0(orchestrator));
    auto config = std::make_shared<NodeConfigManager>();
    ASSERT_TRUE(config->LoadFromFile(MULTI_NODE_CONFIG));
    SchedInfoServer multiple(config);
    EXPECT_FALSE(multiple.EnablePhase0(orchestrator));
    ASSERT_TRUE(config->LoadFromFile(PHASE0_NODE_CONFIG));
    SchedInfoServer active(config);
    ASSERT_TRUE(active.Start(0));
    EXPECT_FALSE(active.EnablePhase0(orchestrator));
    active.Stop();
}

TEST(Phase0ConfigurationTest, OneUnsentAttemptWaitsForInitialConnectionWithoutQueueing)
{
    auto config = std::make_shared<NodeConfigManager>();
    ASSERT_TRUE(config->LoadFromFile(PHASE0_NODE_CONFIG));
    timpani::orchestrator::OrchestratorServiceImpl orchestrator;
    SchedInfoServer server(config);
    ASSERT_TRUE(server.EnablePhase0(orchestrator));
    EXPECT_FALSE(server.EnablePhase0(orchestrator));
    auto& service = Phase0TestPeer::Service(server);
    ASSERT_TRUE(Phase0TestPeer::Replace(service, {w1(), w2()}));
    server.PushPendingTables();
    EXPECT_TRUE(Phase0TestPeer::Pending(service));
    EXPECT_FALSE(Phase0TestPeer::Sent(service));
    EXPECT_TRUE(server.GetScheduleTables().empty());
    EXPECT_FALSE(Phase0TestPeer::Replace(service, {w1(), w3()}));
    EXPECT_FALSE(orchestrator.broadcast_recovery_signal(
        "W1", timpani::node::v1::RecoverySignal::ACTION_STOP));
}

TEST(LegacyCompatibilityTest, DefaultPathStillConfirmsImmediatelyAndAcceptsCbs)
{
    SchedInfoServiceImpl service;
    auto input = workload("legacy", "legacy_task", 20000, 1000, 20000);
    input.set_temporal_class(TEMPORAL_SPORADIC);
    grpc::ServerContext context;
    Response response;
    ASSERT_TRUE(service.AddSchedInfo(&context, &input, &response).ok());
    ASSERT_EQ(response.status(), 0);
    const auto tables = service.GetScheduleTables();
    ASSERT_EQ(tables.count("node1"), 1u);
    EXPECT_GT(tables.at("node1").partitions(0).layers(0).cbs_entries_size(), 0);
}

TEST(LegacyCompatibilityTest, DefaultStreamStillTransmitsIncrementalUpdates)
{
    timpani::orchestrator::OrchestratorServiceImpl orchestrator;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&orchestrator);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    auto stub = timpani::node::v1::OrchestratorService::NewStub(channel);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto stream = stub->NodeStream(&context);
    timpani::node::v1::NodeEvent event;
    event.set_node_id("legacy");
    event.mutable_ready();
    EXPECT_TRUE(stream->Write(event));
    EXPECT_TRUE(wait_until([&] { return !orchestrator.get_connected_node_ids().empty(); }));
    timpani::node::v1::ScheduleTableUpdate update;
    update.set_table_id("delta");
    EXPECT_TRUE(orchestrator.push_update("legacy", update));
    timpani::node::v1::ControlCommand command;
    EXPECT_TRUE(stream->Read(&command));
    EXPECT_TRUE(command.has_update());
    EXPECT_EQ(command.update().table_id(), "delta");
    context.TryCancel();
    stream->WritesDone();
    stream->Finish();
    server->Shutdown(std::chrono::system_clock::now());
    server->Wait();
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    TLOG_SET_LOG_LEVEL(LogLevel::NONE);
    return RUN_ALL_TESTS();
}
