#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/fault_client.h"
#include "../src/orchestrator_service.h"

namespace {

class RecordingFaultService final : public schedinfo::v1::FaultService::Service
{
  public:
    grpc::Status NotifyFault(grpc::ServerContext*,
                             const schedinfo::v1::FaultInfo* request,
                             schedinfo::v1::Response* response) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        received_faults_.push_back(*request);
        response->set_status(0);
        return grpc::Status::OK;
    }

    std::vector<schedinfo::v1::FaultInfo> GetReceivedFaults() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_faults_;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        received_faults_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<schedinfo::v1::FaultInfo> received_faults_;
};

RecordingFaultService fault_service;
std::unique_ptr<grpc::Server> fault_server;

class OrchestratorServiceTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&fault_service);
        fault_server = builder.BuildAndStart();
        ASSERT_NE(fault_server, nullptr);
        ASSERT_GT(port, 0);

        const std::string address = "127.0.0.1:" + std::to_string(port);
        ASSERT_TRUE(FaultServiceClient::GetInstance().Initialize(address));
    }

    static void TearDownTestSuite()
    {
        if (fault_server) {
            fault_server->Shutdown();
            fault_server.reset();
        }
    }

    void SetUp() override
    {
        fault_service.Clear();
        service_ =
            std::make_unique<timpani::orchestrator::OrchestratorServiceImpl>();

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_GT(port, 0);

        const std::string address = "127.0.0.1:" + std::to_string(port);
        channel_ =
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        ASSERT_TRUE(channel_->WaitForConnected(
            std::chrono::system_clock::now() + std::chrono::seconds(3)));
        stub_ = timpani::node::v1::OrchestratorService::NewStub(channel_);
        client_context_ = std::make_unique<grpc::ClientContext>();
        stream_ = stub_->NodeStream(client_context_.get());
        ASSERT_NE(stream_, nullptr);
    }

    void TearDown() override
    {
        if (stream_) {
            EXPECT_TRUE(stream_->WritesDone());
            EXPECT_TRUE(stream_->Finish().ok());
            stream_.reset();
            EXPECT_TRUE(WaitForConnectedNodeCount(0));
        }
        if (server_) {
            server_->Shutdown();
        }
    }

    void ConnectNode(const std::string& node_id = "node-1")
    {
        timpani::node::v1::NodeEvent event;
        event.set_node_id(node_id);
        event.mutable_ready()->set_cpu_count(4);
        event.mutable_ready()->set_memory_mb(2048);
        ASSERT_TRUE(stream_->Write(event));
        ASSERT_TRUE(WaitForConnectedNodeCount(1));
    }

    bool WaitForConnectedNodeCount(size_t expected_count) const
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (service_->get_connected_node_ids().size() == expected_count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return service_->get_connected_node_ids().size() == expected_count;
    }

    bool WaitForFaultCount(size_t expected_count) const
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (fault_service.GetReceivedFaults().size() == expected_count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return fault_service.GetReceivedFaults().size() == expected_count;
    }

    std::unique_ptr<timpani::orchestrator::OrchestratorServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<timpani::node::v1::OrchestratorService::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> client_context_;
    std::unique_ptr<grpc::ClientReaderWriter<timpani::node::v1::NodeEvent,
                                             timpani::node::v1::ControlCommand>>
        stream_;
};

TEST_F(OrchestratorServiceTest, ConnectedNodeIdsTrackStreamLifecycle)
{
    EXPECT_TRUE(service_->get_connected_node_ids().empty());

    ConnectNode();

    const auto node_ids = service_->get_connected_node_ids();
    ASSERT_EQ(node_ids.size(), 1U);
    EXPECT_EQ(node_ids.front(), "node-1");
}

TEST_F(OrchestratorServiceTest, PushFullTableWritesToConnectedNode)
{
    timpani::node::v1::HierarchicalScheduleTable table;
    table.set_table_id("table-1");
    table.set_node_id("node-1");
    EXPECT_FALSE(service_->push_full_table("missing-node", table));

    ConnectNode();
    ASSERT_TRUE(service_->push_full_table("node-1", table));

    timpani::node::v1::ControlCommand command;
    ASSERT_TRUE(stream_->Read(&command));
    ASSERT_TRUE(command.has_full_table());
    EXPECT_EQ(command.full_table().table_id(), "table-1");
    EXPECT_EQ(command.full_table().node_id(), "node-1");
}

TEST_F(OrchestratorServiceTest, PushUpdateWritesToConnectedNode)
{
    timpani::node::v1::ScheduleTableUpdate update;
    update.set_table_id("table-2");
    update.mutable_remove()->add_workload_ids("workload-1");
    EXPECT_FALSE(service_->push_update("missing-node", update));

    ConnectNode();
    ASSERT_TRUE(service_->push_update("node-1", update));

    timpani::node::v1::ControlCommand command;
    ASSERT_TRUE(stream_->Read(&command));
    ASSERT_TRUE(command.has_update());
    EXPECT_EQ(command.update().table_id(), "table-2");
    ASSERT_TRUE(command.update().has_remove());
    ASSERT_EQ(command.update().remove().workload_ids_size(), 1);
    EXPECT_EQ(command.update().remove().workload_ids(0), "workload-1");
}

TEST_F(OrchestratorServiceTest, BroadcastRecoverySignalWritesAction)
{
    EXPECT_TRUE(service_->broadcast_recovery_signal(
        "workload-1", timpani::node::v1::RecoverySignal::ACTION_STOP));

    ConnectNode();
    ASSERT_TRUE(service_->broadcast_recovery_signal(
        "workload-1", timpani::node::v1::RecoverySignal::ACTION_RESTART));

    timpani::node::v1::ControlCommand command;
    ASSERT_TRUE(stream_->Read(&command));
    ASSERT_TRUE(command.has_recovery());
    EXPECT_EQ(command.recovery().workload_id(), "workload-1");
    EXPECT_EQ(command.recovery().action(),
              timpani::node::v1::RecoverySignal::ACTION_RESTART);
}

TEST_F(OrchestratorServiceTest, NodeStreamDispatchesEventsAndForwardsFaults)
{
    ConnectNode();

    timpani::node::v1::NodeEvent status_event;
    status_event.mutable_status()->set_active_workloads(2);
    status_event.mutable_status()->set_isolated_cpus_used(1);
    ASSERT_TRUE(stream_->Write(status_event));

    timpani::node::v1::NodeEvent applied_event;
    applied_event.mutable_applied()->set_table_id("table-3");
    applied_event.mutable_applied()->set_success(true);
    ASSERT_TRUE(stream_->Write(applied_event));

    timpani::node::v1::NodeEvent dmiss_event;
    auto* dmiss = dmiss_event.mutable_fault();
    dmiss->set_workload_id_hash(42);
    dmiss->set_task_id_hash(99);
    dmiss->set_fault_type(timpani::node::v1::FaultType::DMISS);
    dmiss->set_dmiss_count(2);
    dmiss->set_current_limit(2);
    ASSERT_TRUE(stream_->Write(dmiss_event));

    timpani::node::v1::NodeEvent budget_event;
    auto* budget = budget_event.mutable_fault();
    budget->set_workload_id("workload-2");
    budget->set_task_id("task-2");
    budget->set_fault_type(timpani::node::v1::FaultType::BUDGET_EXCEED);
    budget->set_dmiss_count(3);
    ASSERT_TRUE(stream_->Write(budget_event));

    ASSERT_TRUE(WaitForFaultCount(2));
    const auto forwarded_faults = fault_service.GetReceivedFaults();
    ASSERT_EQ(forwarded_faults.size(), 2U);
    EXPECT_EQ(forwarded_faults[0].workload_id(), "42");
    EXPECT_EQ(forwarded_faults[0].node_id(), "node-1");
    EXPECT_EQ(forwarded_faults[0].task_name(), "99");
    EXPECT_EQ(forwarded_faults[0].type(), schedinfo::v1::FaultType::DMISS);
    EXPECT_EQ(forwarded_faults[0].cumulative_dmiss(), 2U);
    EXPECT_EQ(forwarded_faults[1].workload_id(), "workload-2");
    EXPECT_EQ(forwarded_faults[1].task_name(), "task-2");
    EXPECT_EQ(forwarded_faults[1].type(), schedinfo::v1::FaultType::UNKNOWN);
    EXPECT_EQ(forwarded_faults[1].cumulative_dmiss(), 3U);
}

}  // namespace