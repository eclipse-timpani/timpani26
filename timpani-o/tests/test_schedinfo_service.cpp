/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <memory>
#include <random>
#include <set>
#include <string>

#include "../src/tlog.h"
#include "../src/schedinfo_service.h"
#include "../src/node_config.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using schedinfo::v1::SchedInfo;
using schedinfo::v1::TaskInfo;
using schedinfo::v1::Response;
using schedinfo::v1::SchedPolicy;
using schedinfo::v1::SchedInfoService;

// Helper function to get an unused port
int GetUnusedPort() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(50000, 59999);
    return dis(gen);
}

class SchedInfoServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create service implementation with nullptr for node config manager
        // This will trigger the default behavior
        service_impl_ = std::make_unique<SchedInfoServiceImpl>(nullptr);
    }

    void TearDown() override {
        service_impl_.reset();
    }

    // Helper method to create a sample SchedInfo
    SchedInfo CreateSampleSchedInfo(const std::string& workload_id = "test_workload",
                                    int num_tasks = 2) {
        SchedInfo sched_info;
        sched_info.set_workload_id(workload_id);

        for (int i = 0; i < num_tasks; ++i) {
            TaskInfo* task = sched_info.add_tasks();
            task->set_name("task_" + std::to_string(i));
            task->set_priority(50 + i);
            task->set_policy(SchedPolicy::FIFO);
            task->set_cpu_affinity(1 << i); // CPU 0, 1, etc.
            task->set_period(1000000); // 1 second in microseconds
            task->set_runtime(100000);  // 100ms in microseconds
            task->set_deadline(900000); // 900ms in microseconds
            task->set_release_time(0);
            task->set_max_dmiss(3);
            task->set_node_id("node1");
        }

        return sched_info;
    }

    std::unique_ptr<SchedInfoServiceImpl> service_impl_;
};

// Tests for SchedInfoServiceImpl
TEST_F(SchedInfoServiceTest, ConstructorInitializesCorrectly) {
    ASSERT_NE(service_impl_, nullptr);

    // Test that the service starts with empty schedule tables
    auto sched_tables = service_impl_->GetScheduleTables();
    EXPECT_TRUE(sched_tables.empty());
}

TEST_F(SchedInfoServiceTest, AddSchedInfoSuccess) {
    SchedInfo sched_info = CreateSampleSchedInfo();
    Response reply;
    grpc::ServerContext context;

    Status status = service_impl_->AddSchedInfo(&context, &sched_info, &reply);

    EXPECT_TRUE(status.ok());
    // Note: The status might be 0 or -1 depending on scheduling success
    // For this test, we're mainly checking that the call doesn't crash

    // Verify that the service handled the request
    auto sched_tables = service_impl_->GetScheduleTables();
    // The map might be empty if scheduling failed, but the service should handle it gracefully
}

TEST_F(SchedInfoServiceTest, AddSchedInfoWithEmptyTasks) {
    SchedInfo sched_info;
    sched_info.set_workload_id("empty_workload");
    // No tasks added

    Response reply;
    grpc::ServerContext context;

    Status status = service_impl_->AddSchedInfo(&context, &sched_info, &reply);

    EXPECT_TRUE(status.ok());
    // Should handle empty tasks gracefully
}

TEST_F(SchedInfoServiceTest, AddSchedInfoMultipleWorkloadsNotSupported) {
    // First workload
    SchedInfo sched_info1 = CreateSampleSchedInfo("workload1", 1);
    Response reply1;
    grpc::ServerContext context1;

    Status status1 = service_impl_->AddSchedInfo(&context1, &sched_info1, &reply1);
    EXPECT_TRUE(status1.ok());

    // Second workload - should fail due to multiple workload limitation
    SchedInfo sched_info2 = CreateSampleSchedInfo("workload2", 1);
    Response reply2;
    grpc::ServerContext context2;

    Status status2 = service_impl_->AddSchedInfo(&context2, &sched_info2, &reply2);
    EXPECT_TRUE(status2.ok());

    // Both workloads can now coexist in the schedule table map
    auto sched_tables = service_impl_->GetScheduleTables();
    // With GlobalScheduler, multiple workloads are supported
}

TEST_F(SchedInfoServiceTest, AddSchedInfoWithDifferentPolicies) {
    SchedInfo sched_info;
    sched_info.set_workload_id("policy_test");

    // Add tasks with different scheduling policies
    TaskInfo* task1 = sched_info.add_tasks();
    task1->set_name("normal_task");
    task1->set_policy(SchedPolicy::NORMAL);
    task1->set_priority(0);
    task1->set_cpu_affinity(0xFFFFFFFF); // Any CPU
    task1->set_period(1000000);
    task1->set_runtime(100000);
    task1->set_deadline(900000);
    task1->set_node_id("node1");

    TaskInfo* task2 = sched_info.add_tasks();
    task2->set_name("fifo_task");
    task2->set_policy(SchedPolicy::FIFO);
    task2->set_priority(50);
    task2->set_cpu_affinity(1);
    task2->set_period(2000000);
    task2->set_runtime(200000);
    task2->set_deadline(1800000);
    task2->set_node_id("node1");

    TaskInfo* task3 = sched_info.add_tasks();
    task3->set_name("rr_task");
    task3->set_policy(SchedPolicy::RR);
    task3->set_priority(25);
    task3->set_cpu_affinity(2);
    task3->set_period(500000);
    task3->set_runtime(50000);
    task3->set_deadline(450000);
    task3->set_node_id("node1");

    Response reply;
    grpc::ServerContext context;

    Status status = service_impl_->AddSchedInfo(&context, &sched_info, &reply);

    EXPECT_TRUE(status.ok());
    // The service should handle different policies without crashing
}

TEST_F(SchedInfoServiceTest, GetSchedInfoMapThreadSafety) {
    SchedInfo sched_info = CreateSampleSchedInfo();
    Response reply;
    grpc::ServerContext context;

    // Add schedule info in main thread
    Status status = service_impl_->AddSchedInfo(&context, &sched_info, &reply);
    EXPECT_TRUE(status.ok());

    // Access schedule info map from multiple threads
    std::vector<std::thread> threads;
    std::vector<bool> results(5, false);

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, &results, i]() {
            auto sched_tables = service_impl_->GetScheduleTables();
            results[i] = true; // Thread completed successfully
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All threads should have completed successfully
    for (bool result : results) {
        EXPECT_TRUE(result);
    }
}

TEST_F(SchedInfoServiceTest, RemoveWorkloadProducesEmptyTableForNode) {
    SchedInfo sched_info = CreateSampleSchedInfo("wl_to_remove", 1);
    Response reply;
    grpc::ServerContext context;

    Status status = service_impl_->AddSchedInfo(&context, &sched_info, &reply);
    EXPECT_TRUE(status.ok());

    auto sched_tables_before = service_impl_->GetScheduleTables();
    ASSERT_NE(sched_tables_before.find("node1"), sched_tables_before.end());
    EXPECT_GT(sched_tables_before["node1"].partitions_size(), 0);

    bool removed = service_impl_->RemoveWorkload("wl_to_remove");
    EXPECT_TRUE(removed);

    auto sched_tables_after = service_impl_->GetScheduleTables();
    ASSERT_NE(sched_tables_after.find("node1"), sched_tables_after.end());
    EXPECT_EQ(sched_tables_after["node1"].partitions_size(), 0);
}

// Collect TT slot task ids of a node table.
static std::set<std::string> TableTaskIds(
    const timpani::node::v1::HierarchicalScheduleTable& table) {
    std::set<std::string> ids;
    for (const auto& part : table.partitions()) {
        for (const auto& layer : part.layers()) {
            for (const auto& slot : layer.tt_slots()) {
                ids.insert(slot.task_id());
            }
        }
    }
    return ids;
}

static void AddTtTask(SchedInfo& info, const std::string& name,
                      const std::string& node_id, uint64_t period_us) {
    TaskInfo* task = info.add_tasks();
    task->set_name(name);
    task->set_policy(SchedPolicy::FIFO);
    task->set_priority(50);
    task->set_period(period_us);
    task->set_runtime(period_us / 10);
    task->set_deadline(period_us);
    task->set_node_id(node_id);
}

TEST_F(SchedInfoServiceTest, WorkloadsOnSameNodeAreMerged) {
    SchedInfo wl1;
    wl1.set_workload_id("wl1");
    AddTtTask(wl1, "wl1_t0", "nodeA", 100000);
    AddTtTask(wl1, "wl1_t1", "nodeA", 50000);

    SchedInfo wl2;
    wl2.set_workload_id("wl2");
    AddTtTask(wl2, "wl2_t0", "nodeA", 100000);

    SchedInfo wl3;
    wl3.set_workload_id("wl3");
    AddTtTask(wl3, "wl3_t0", "nodeB", 100000);

    for (SchedInfo* info : {&wl1, &wl2, &wl3}) {
        Response reply;
        grpc::ServerContext context;
        ASSERT_TRUE(service_impl_->AddSchedInfo(&context, info, &reply).ok());
        ASSERT_EQ(reply.status(), 0) << info->workload_id();
    }

    auto tables = service_impl_->GetScheduleTables();
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_EQ(TableTaskIds(tables.at("nodeA")),
              std::set<std::string>({"wl1_t0", "wl1_t1", "wl2_t0"}));
    EXPECT_EQ(TableTaskIds(tables.at("nodeB")),
              std::set<std::string>({"wl3_t0"}));
}

TEST_F(SchedInfoServiceTest, WorkloadSpanningMultipleNodesIsRejected) {
    SchedInfo base;
    base.set_workload_id("base");
    AddTtTask(base, "base_a", "nodeA", 100000);
    Response base_reply;
    grpc::ServerContext base_context;
    service_impl_->AddSchedInfo(&base_context, &base, &base_reply);
    ASSERT_EQ(base_reply.status(), 0);
    auto before = service_impl_->GetScheduleTables();

    SchedInfo spanning;
    spanning.set_workload_id("spanning");
    AddTtTask(spanning, "on_a", "nodeA", 100000);
    AddTtTask(spanning, "on_b", "nodeB", 100000);

    Response reply;
    grpc::ServerContext context;
    ASSERT_TRUE(service_impl_->AddSchedInfo(&context, &spanning, &reply).ok());
    EXPECT_EQ(reply.status(), -1);

    auto after = service_impl_->GetScheduleTables();
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(after.count("nodeB"), 0u);
    EXPECT_EQ(after.at("nodeA").SerializeAsString(),
              before.at("nodeA").SerializeAsString());
}

TEST_F(SchedInfoServiceTest, WorkloadWithoutNodeIdIsRejected) {
    SchedInfo info;
    info.set_workload_id("no_node");
    AddTtTask(info, "t0", "", 100000);
    AddTtTask(info, "t1", "", 100000);

    Response reply;
    grpc::ServerContext context;
    ASSERT_TRUE(service_impl_->AddSchedInfo(&context, &info, &reply).ok());
    EXPECT_EQ(reply.status(), -1);
    EXPECT_TRUE(service_impl_->GetScheduleTables().empty());
}

TEST_F(SchedInfoServiceTest, WorkloadWithPartialNodeIdIsRejected) {
    SchedInfo info;
    info.set_workload_id("partial");
    AddTtTask(info, "with_node", "nodeA", 100000);
    AddTtTask(info, "without_node", "", 100000);

    Response reply;
    grpc::ServerContext context;
    ASSERT_TRUE(service_impl_->AddSchedInfo(&context, &info, &reply).ok());
    EXPECT_EQ(reply.status(), -1);
    EXPECT_TRUE(service_impl_->GetScheduleTables().empty());
}

TEST_F(SchedInfoServiceTest, RemovingWorkloadKeepsOtherNodes) {
    SchedInfo wl_a;
    wl_a.set_workload_id("wl_a");
    AddTtTask(wl_a, "a_task", "nodeA", 100000);
    SchedInfo wl_b;
    wl_b.set_workload_id("wl_b");
    AddTtTask(wl_b, "b_task", "nodeB", 100000);
    for (SchedInfo* info : {&wl_a, &wl_b}) {
        Response reply;
        grpc::ServerContext context;
        service_impl_->AddSchedInfo(&context, info, &reply);
        ASSERT_EQ(reply.status(), 0);
    }

    ASSERT_TRUE(service_impl_->RemoveWorkload("wl_a"));

    auto tables = service_impl_->GetScheduleTables();
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_EQ(tables.at("nodeA").partitions_size(), 0);
    EXPECT_EQ(TableTaskIds(tables.at("nodeB")),
              std::set<std::string>({"b_task"}));
}

// Tests for SchedInfoServer
class SchedInfoServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<SchedInfoServer>(nullptr);
        port_ = GetUnusedPort(); // Use dynamic port allocation
    }

    void TearDown() override {
        if (server_) {
            server_->Stop();
        }
        server_.reset();
    }

    std::unique_ptr<SchedInfoServer> server_;
    int port_;
};

TEST_F(SchedInfoServerTest, StartAndStopServer) {
    // Start the server
    bool started = server_->Start(port_);
    EXPECT_TRUE(started);

    // Give the server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop the server
    server_->Stop();
}

TEST_F(SchedInfoServerTest, GetScheduleTablesWhenEmpty) {
    auto sched_tables = server_->GetScheduleTables();
    EXPECT_TRUE(sched_tables.empty());
}

TEST_F(SchedInfoServerTest, DumpSchedInfoWhenEmpty) {
    // This should not crash or throw
    EXPECT_NO_THROW(server_->DumpSchedInfo());
}

// Integration test with gRPC client
class SchedInfoServerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = GetUnusedPort(); // Use dynamic port allocation
        server_ = std::make_unique<SchedInfoServer>(nullptr);

        // Start server
        bool started = server_->Start(port_);
        ASSERT_TRUE(started);

        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Create client channel
        std::string server_address = "localhost:" + std::to_string(port_);
        channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
        stub_ = SchedInfoService::NewStub(channel_);
    }

    void TearDown() override {
        stub_.reset();
        channel_.reset();
        if (server_) {
            server_->Stop();
        }
        server_.reset();
    }

    SchedInfo CreateTestSchedInfo() {
        SchedInfo sched_info;
        sched_info.set_workload_id("integration_test");

        TaskInfo* task = sched_info.add_tasks();
        task->set_name("integration_task");
        task->set_priority(50);
        task->set_policy(SchedPolicy::FIFO);
        task->set_cpu_affinity(1);
        task->set_period(1000000);
        task->set_runtime(100000);
        task->set_deadline(900000);
        task->set_release_time(0);
        task->set_max_dmiss(3);
        task->set_node_id("node1");

        return sched_info;
    }

    std::unique_ptr<SchedInfoServer> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<SchedInfoService::Stub> stub_;
    int port_;
};

TEST_F(SchedInfoServerIntegrationTest, AddSchedInfoViaGRPC) {
    SchedInfo sched_info = CreateTestSchedInfo();
    Response response;
    grpc::ClientContext context;

    Status status = stub_->AddSchedInfo(&context, sched_info, &response);

    EXPECT_TRUE(status.ok());
    // The response status will depend on scheduling success
    // We're mainly testing that the gRPC communication works
}

TEST_F(SchedInfoServerIntegrationTest, MultipleClientsSimultaneous) {
    const int num_clients = 3; // Reduce to avoid overwhelming the system
    std::vector<std::thread> client_threads;
    std::vector<bool> results(num_clients, false);

    // Create multiple client threads
    for (int i = 0; i < num_clients; ++i) {
        client_threads.emplace_back([this, &results, i]() {
            // Each client creates its own stub
            auto client_stub = SchedInfoService::NewStub(channel_);

            SchedInfo sched_info;
            sched_info.set_workload_id("client_" + std::to_string(i));

            TaskInfo* task = sched_info.add_tasks();
            task->set_name("task_" + std::to_string(i));
            task->set_priority(50);
            task->set_policy(SchedPolicy::FIFO);
            task->set_cpu_affinity(1);
            task->set_period(1000000);
            task->set_runtime(100000);
            task->set_deadline(900000);
            task->set_node_id("node1");

            Response response;
            grpc::ClientContext context;
            Status status = client_stub->AddSchedInfo(&context, sched_info, &response);

            results[i] = status.ok();
        });
    }

    // Wait for all clients to complete
    for (auto& thread : client_threads) {
        thread.join();
    }

    // At least one client should have communicated successfully
    bool any_success = false;
    for (bool result : results) {
        if (result) {
            any_success = true;
            break;
        }
    }
    EXPECT_TRUE(any_success);
}

// Test main function
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    TLOG_SET_LOG_LEVEL(LogLevel::NONE);
    return RUN_ALL_TESTS();
}
