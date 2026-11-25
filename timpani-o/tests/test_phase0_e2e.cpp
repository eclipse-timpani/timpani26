/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

// DDR-011 Phase 0 P0-T06: target end-to-end driver. It plays Timpani-O for
// one real Timpani-N and runs the TC-P0-01..04 and TC-P0-06 sequence through
// the O -> N full_table path. Skipped unless TIMPANI_P0_E2E=1; the target
// procedure is in doc/design/DDR-011-runtime-table-update-phase0/.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "orchestrator_service.h"
#include "schedinfo_service.h"

class Phase0TestPeer
{
  public:
    static SchedInfoServiceImpl& Service(SchedInfoServer& server)
    {
        return server.service_;
    }
    static bool Replace(SchedInfoServiceImpl& service,
                        const std::vector<SchedInfo>& inputs)
    {
        return service.SubmitPhase0Update(
            SchedInfoServiceImpl::Phase0Operation::Replace, inputs);
    }
    static bool Clear(SchedInfoServiceImpl& service)
    {
        return service.SubmitPhase0Update(
            SchedInfoServiceImpl::Phase0Operation::Clear, {});
    }
    static std::string NodeId(SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->node_id;
    }
    // Empty when no update is in flight.
    static std::string CandidateId(SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        if (!service.phase0_->attempt) return std::string();
        return service.phase0_->attempt->candidate.table.table_id();
    }
    static bool Sent(SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->attempt && service.phase0_->attempt->sent;
    }
    static bool Stopped(SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        return service.phase0_->stopped;
    }
    static std::string ConfirmedId(SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        if (!service.phase0_->confirmed) return std::string();
        return service.phase0_->confirmed->table.table_id();
    }
    static std::set<std::string> ConfirmedWorkloads(
        SchedInfoServiceImpl& service)
    {
        std::lock_guard<std::mutex> lock(service.phase0_->mutex);
        std::set<std::string> result;
        if (service.phase0_->confirmed) {
            for (const auto& item : service.phase0_->confirmed->workloads) {
                result.insert(item.first);
            }
        }
        return result;
    }
};

namespace {

using Clock = std::chrono::steady_clock;
using Workloads = std::set<std::string>;

long EnvNumber(const char* name, long fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value <= 0) {
        ADD_FAILURE() << name << " must be a positive integer: " << text;
        return fallback;
    }
    return value;
}

std::string Join(const Workloads& ids)
{
    std::string result;
    for (const auto& id : ids) {
        if (!result.empty()) result += ',';
        result += id;
    }
    return result.empty() ? "-" : result;
}

}  // namespace

class Phase0E2ETest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const char* enabled = std::getenv("TIMPANI_P0_E2E");
        if (enabled == nullptr || std::string(enabled) != "1") {
            GTEST_SKIP() << "target-only; set TIMPANI_P0_E2E=1 and start "
                            "timpani-n --runtime-table-update-phase0";
        }
        port_ = static_cast<int>(EnvNumber("TIMPANI_P0_E2E_PORT", 50060));
        ack_timeout_ =
            std::chrono::seconds(EnvNumber("TIMPANI_P0_E2E_ACK_TIMEOUT_S", 10));
        const auto connect_timeout =
            std::chrono::seconds(EnvNumber("TIMPANI_P0_E2E_CONNECT_S", 60));
        rounds_ = EnvNumber("TIMPANI_P0_E2E_ROUNDS", 3);
        const char* config = std::getenv("TIMPANI_P0_E2E_NODE_CONFIG");
        if (config == nullptr || *config == '\0') config = PHASE0_NODE_CONFIG;

        config_ = std::make_shared<NodeConfigManager>();
        ASSERT_TRUE(config_->LoadFromFile(config)) << config;
        server_ = std::make_unique<SchedInfoServer>(config_);
        ASSERT_TRUE(server_->EnablePhase0(orchestrator_));
        node_ = Phase0TestPeer::NodeId(Service());

        grpc::ServerBuilder builder;
        int bound = 0;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(port_),
                                 grpc::InsecureServerCredentials(), &bound);
        builder.RegisterService(&Service());
        builder.RegisterService(&orchestrator_);
        grpc_server_ = builder.BuildAndStart();
        ASSERT_NE(grpc_server_, nullptr);
        ASSERT_EQ(bound, port_) << "cannot listen on port " << port_;
        sched_stub_ = SchedInfoService::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                grpc::InsecureChannelCredentials()));

        Log("waiting node_id=" + node_ + " port=" + std::to_string(port_));
        const auto deadline = Clock::now() + connect_timeout;
        while (!Connected()) {
            ASSERT_LT(Clock::now(), deadline)
                << "Timpani-N " << node_ << " did not connect";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override
    {
        if (grpc_server_) {
            grpc_server_->Shutdown(std::chrono::system_clock::now() +
                                   std::chrono::seconds(1));
            grpc_server_->Wait();
        }
    }

    SchedInfoServiceImpl& Service()
    {
        return Phase0TestPeer::Service(*server_);
    }

    bool Connected()
    {
        for (const auto& id : orchestrator_.get_connected_node_ids()) {
            if (id == node_) return true;
        }
        return false;
    }

    void AddTask(SchedInfo& input, const std::string& name, int period,
                 int runtime, int deadline) const
    {
        auto* task = input.add_tasks();
        task->set_name(name);
        task->set_period(period);
        task->set_runtime(runtime);
        task->set_deadline(deadline);
        task->set_max_dmiss(3);
        task->set_node_id(node_);
    }

    SchedInfo Workload(const std::string& id, const std::string& name,
                       int period, int runtime, int deadline) const
    {
        SchedInfo result;
        result.set_workload_id(id);
        AddTask(result, name, period, runtime, deadline);
        return result;
    }

    // Task names are the comm names of the sample apps on the target.
    SchedInfo W1() const
    {
        auto result = Workload("W1", "w1_fast", 10000, 1000, 2000);
        AddTask(result, "w1_anchor", 20000, 8000, 9000);
        return result;
    }
    SchedInfo W2() const
    {
        return Workload("W2", "w2_task", 20000, 1000, 10000);
    }
    SchedInfo W3() const
    {
        return Workload("W3", "w3_task", 20000, 2000, 20000);
    }
    // No process has this name, so Timpani-N rejects the table.
    SchedInfo W4() const
    {
        return Workload("W4", "p0_absent", 20000, 500, 20000);
    }

    int Add(const SchedInfo& input)
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(2));
        Response response;
        const auto status =
            sched_stub_->AddSchedInfo(&context, input, &response);
        EXPECT_TRUE(status.ok()) << status.error_message();
        return status.ok() ? response.status() : -1;
    }

    bool Replace(const std::vector<SchedInfo>& inputs)
    {
        return Phase0TestPeer::Replace(Service(), inputs);
    }

    // TC-P0-06: every operation is refused while one update is in flight.
    void ExpectBusy()
    {
        EXPECT_EQ(Add(W4()), -1);
        EXPECT_FALSE(server_->RemoveWorkload("W1"));
        EXPECT_FALSE(Replace({W1(), W2()}));
        EXPECT_FALSE(Phase0TestPeer::Clear(Service()));
    }

    void Log(const std::string& text)
    {
        std::cout << "[RTU-P0-E2E] " << text << std::endl;
    }

    // Submits one update, sends it to Timpani-N and waits for its ACK. A
    // missing ACK or a stopped trial fails the run, and no further update is
    // submitted afterwards (REQ-RTU-P0-F-018).
    void Step(const std::string& name, const std::function<bool()>& submit,
              bool applied, const Workloads& expected,
              const std::function<void()>& in_flight = nullptr)
    {
        ASSERT_TRUE(submit()) << name << ": Timpani-O refused the update";
        const std::string table_id = Phase0TestPeer::CandidateId(Service());
        ASSERT_FALSE(table_id.empty()) << name;
        if (in_flight) in_flight();

        const auto deadline = Clock::now() + ack_timeout_;
        while (!Phase0TestPeer::CandidateId(Service()).empty()) {
            ASSERT_LT(Clock::now(), deadline)
                << name << ": no TableApplied for table_id=" << table_id;
            server_->PushPendingTables();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_FALSE(Phase0TestPeer::Stopped(Service()))
            << name << ": the trial stopped";

        const std::string confirmed_id = Phase0TestPeer::ConfirmedId(Service());
        const Workloads confirmed =
            Phase0TestPeer::ConfirmedWorkloads(Service());
        Log("round=" + std::to_string(round_) + " step=\"" + name +
            "\" table_id=" + table_id +
            " result=" + (confirmed_id == table_id ? "applied" : "rejected") +
            " confirmed=" + Join(confirmed));
        EXPECT_EQ(confirmed_id == table_id, applied) << name;
        EXPECT_EQ(confirmed, expected) << name;
    }

    std::shared_ptr<NodeConfigManager> config_;
    timpani::orchestrator::OrchestratorServiceImpl orchestrator_;
    std::unique_ptr<SchedInfoServer> server_;
    std::unique_ptr<grpc::Server> grpc_server_;
    std::unique_ptr<SchedInfoService::Stub> sched_stub_;
    std::string node_;
    int port_ = 0;
    std::chrono::seconds ack_timeout_{0};
    long rounds_ = 0;
    long round_ = 0;
};

TEST_F(Phase0E2ETest, RunsFourOperationsAndRejectionsOnTarget)
{
    const Workloads a = {"W1", "W2"};
    ASSERT_NO_FATAL_FAILURE(
        Step("baseline A", [&] { return Replace({W1(), W2()}); }, true, a));

    for (round_ = 1; round_ <= rounds_; ++round_) {
        ASSERT_NO_FATAL_FAILURE(Step("TC-P0-01 REPLACE {W1,W3}",
                                     [&] { return Replace({W1(), W3()}); },
                                     true, {"W1", "W3"}));
        ASSERT_NO_FATAL_FAILURE(
            Step("REPLACE A", [&] { return Replace({W1(), W2()}); }, true, a));

        ASSERT_NO_FATAL_FAILURE(Step(
            "TC-P0-02/06 ADD W3", [&] { return Add(W3()) == 0; }, true,
            {"W1", "W2", "W3"}, [&] { ExpectBusy(); }));
        ASSERT_NO_FATAL_FAILURE(Step(
            "TC-P0-02 REMOVE W2", [&] { return server_->RemoveWorkload("W2"); },
            true, {"W1", "W3"}));
        ASSERT_NO_FATAL_FAILURE(
            Step("REPLACE A", [&] { return Replace({W1(), W2()}); }, true, a));

        ASSERT_NO_FATAL_FAILURE(
            Step("TC-P0-03 CLEAR",
                 [&] { return Phase0TestPeer::Clear(Service()); }, true, {}));
        ASSERT_NO_FATAL_FAILURE(Step(
            "TC-P0-03 ADD W1", [&] { return Add(W1()) == 0; }, true, {"W1"}));

        ASSERT_NO_FATAL_FAILURE(Step("TC-P0-04 ADD absent PID",
                                     [&] { return Add(W4()) == 0; }, false,
                                     {"W1"}));
        // Unschedulable input: refused by Timpani-O, nothing is sent.
        EXPECT_EQ(Add(Workload("W5", "p0_overload", 20000, 19000, 20000)), -1);
        EXPECT_TRUE(Phase0TestPeer::CandidateId(Service()).empty());

        ASSERT_NO_FATAL_FAILURE(
            Step("ADD W2 to A", [&] { return Add(W2()) == 0; }, true, a));
    }
}
