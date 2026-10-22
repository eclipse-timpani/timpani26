#include <gtest/gtest.h>

#include <memory>

#include "../src/orchestrator_service.h"
#include "../src/recovery_service.h"

std::unique_ptr<timpani::orchestrator::OrchestratorServiceImpl>
    g_orchestrator_service;

class RecoveryServiceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        g_orchestrator_service.reset();
    }
};

TEST_F(RecoveryServiceTest, UnknownPolicyReturnsSuccess)
{
    RecoveryServiceImpl service(nullptr);
    RecoveryCommand request;
    request.set_workload_id("workload-1");
    request.set_recovery_policy(schedinfo::v1::RECOVERY_UNKNOWN);
    Response response;
    grpc::ServerContext context;

    const grpc::Status status =
        service.EnforceRecoveryPolicy(&context, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.status(), 0);
}

TEST_F(RecoveryServiceTest, RestartPolicyReturnsSuccess)
{
    RecoveryServiceImpl service(nullptr);
    RecoveryCommand request;
    request.set_workload_id("workload-1");
    request.set_recovery_policy(schedinfo::v1::RECOVERY_RESTART);
    Response response;
    grpc::ServerContext context;

    const grpc::Status status =
        service.EnforceRecoveryPolicy(&context, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.status(), 0);
}

TEST_F(RecoveryServiceTest, TerminatePolicyReturnsSuccess)
{
    RecoveryServiceImpl service(nullptr);
    RecoveryCommand request;
    request.set_workload_id("workload-1");
    request.set_recovery_policy(schedinfo::v1::RECOVERY_TERMINATE);
    Response response;
    grpc::ServerContext context;

    const grpc::Status status =
        service.EnforceRecoveryPolicy(&context, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.status(), 0);
}

TEST_F(RecoveryServiceTest, StopPolicyReportsFailureWithoutOrchestrator)
{
    RecoveryServiceImpl service(nullptr);
    RecoveryCommand request;
    request.set_workload_id("workload-1");
    request.set_recovery_policy(schedinfo::v1::RECOVERY_STOP);
    Response response;
    grpc::ServerContext context;

    const grpc::Status status =
        service.EnforceRecoveryPolicy(&context, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.status(), -1);
}

TEST_F(RecoveryServiceTest, StopPolicySucceedsWithOrchestrator)
{
    g_orchestrator_service =
        std::make_unique<timpani::orchestrator::OrchestratorServiceImpl>();
    RecoveryServiceImpl service(nullptr);
    RecoveryCommand request;
    request.set_workload_id("workload-1");
    request.set_recovery_policy(schedinfo::v1::RECOVERY_STOP);
    Response response;
    grpc::ServerContext context;

    const grpc::Status status =
        service.EnforceRecoveryPolicy(&context, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.status(), 0);
}