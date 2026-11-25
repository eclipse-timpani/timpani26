## SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
## SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Protobuf REQUIRED)
if(NOT TARGET gRPC::grpc++)
    pkg_search_module(GRPC REQUIRED IMPORTED_TARGET grpc)
    pkg_search_module(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    add_library(gRPC::grpc ALIAS PkgConfig::GRPC)
    add_library(gRPC::grpc++ ALIAS PkgConfig::GRPCPP)
endif()

# Find Google Test and Google Mock
find_package(GTest REQUIRED)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../src)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../proto)
include_directories(${GTEST_INCLUDE_DIRS})

# Create test executable
add_executable(test_schedinfo_service
    test_schedinfo_service.cpp
    ../src/schedinfo_service.cpp
    ../src/phase0_runtime.cpp
    ../src/orchestrator_service.cpp
    ../src/fault_client.cpp
    ../src/node_config.cpp
    ../src/global_scheduler.cpp
)

# Link libraries
target_link_libraries(test_schedinfo_service
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto  # Proto library from proto directory
    node_control_proto
    pthread
)

# Add test
add_test(NAME SchedInfoServiceTest COMMAND test_schedinfo_service)

# Set test properties
set_tests_properties(SchedInfoServiceTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(test_runtime_table_update
    test_runtime_table_update.cpp
    ../src/schedinfo_service.cpp
    ../src/phase0_runtime.cpp
    ../src/orchestrator_service.cpp
    ../src/fault_client.cpp
    ../src/node_config.cpp
    ../src/global_scheduler.cpp
)
target_compile_definitions(test_runtime_table_update PRIVATE
    PHASE0_NODE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../examples/phase0_node.yaml"
    MULTI_NODE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../examples/node_configurations.yaml"
)
target_link_libraries(test_runtime_table_update
    GTest::gtest GTest::gtest_main gRPC::grpc++ protobuf::libprotobuf
    schedinfo_proto node_control_proto pthread
)
add_test(NAME RuntimeTableUpdateTest COMMAND test_runtime_table_update)
set_tests_properties(RuntimeTableUpdateTest PROPERTIES TIMEOUT 30)

# DDR-011 Phase 0 target E2E driver (P0-T06). It needs a real Timpani-N on the
# target, so it reports SKIPPED unless TIMPANI_P0_E2E=1.
add_executable(test_phase0_e2e
    test_phase0_e2e.cpp
    ../src/schedinfo_service.cpp
    ../src/phase0_runtime.cpp
    ../src/orchestrator_service.cpp
    ../src/fault_client.cpp
    ../src/node_config.cpp
    ../src/global_scheduler.cpp
)
target_compile_definitions(test_phase0_e2e PRIVATE
    PHASE0_NODE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../examples/phase0_node.yaml"
)
target_link_libraries(test_phase0_e2e
    GTest::gtest GTest::gtest_main gRPC::grpc++ protobuf::libprotobuf
    schedinfo_proto node_control_proto pthread
)
add_test(NAME Phase0E2ETest COMMAND test_phase0_e2e)
set_tests_properties(Phase0E2ETest PROPERTIES
    TIMEOUT 600
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
)

# Create FaultServiceClient test executable
add_executable(test_fault_client
    test_fault_client.cpp
    ../src/fault_client.cpp
)

# Link libraries for FaultServiceClient test
target_link_libraries(test_fault_client
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto  # Proto library from proto directory
    pthread
)

# Add FaultServiceClient test
add_test(NAME FaultServiceClientTest COMMAND test_fault_client)

# Set test properties for FaultServiceClient test
set_tests_properties(FaultServiceClientTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Create GlobalScheduler test executable
add_executable(test_global_scheduler
    test_global_scheduler.cpp
    ../src/global_scheduler.cpp
    ../src/node_config.cpp
)

# Link libraries for GlobalScheduler test
target_link_libraries(test_global_scheduler
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto
    node_control_proto
    pthread
)

# Add GlobalScheduler test
add_test(NAME GlobalSchedulerTest COMMAND test_global_scheduler)

# Set test properties for GlobalScheduler test
set_tests_properties(GlobalSchedulerTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Create PublicationCoordinator test executable
add_executable(test_publication_coordinator
    test_publication_coordinator.cpp
    ../src/publication_coordinator.cpp
    ../src/global_scheduler.cpp
    ../src/node_config.cpp
)

# Link libraries for PublicationCoordinator test
target_link_libraries(test_publication_coordinator
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto
    node_control_proto
    pthread
)

# Add PublicationCoordinator test
add_test(NAME PublicationCoordinatorTest COMMAND test_publication_coordinator)

# Set test properties for PublicationCoordinator test
set_tests_properties(PublicationCoordinatorTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Create NodeConfig test executable
add_executable(test_node_config
    test_node_config.cpp
    ../src/node_config.cpp
)

# Link libraries for NodeConfig test
target_link_libraries(test_node_config
    GTest::gtest
    GTest::gtest_main
    pthread
)

# Add NodeConfig test
add_test(NAME NodeConfigTest COMMAND test_node_config)

# Set test properties for NodeConfig test
set_tests_properties(NodeConfigTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Create RecoveryService test executable
add_executable(test_recovery_service
    test_recovery_service.cpp
    ../src/recovery_service.cpp
    ../src/schedinfo_service.cpp
    ../src/phase0_runtime.cpp
    ../src/node_config.cpp
    ../src/global_scheduler.cpp
    ../src/orchestrator_service.cpp
    ../src/fault_client.cpp
)

target_link_libraries(test_recovery_service
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto
    node_control_proto
    pthread
)

add_test(NAME RecoveryServiceTest COMMAND test_recovery_service)

set_tests_properties(RecoveryServiceTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Create OrchestratorService test executable
add_executable(test_orchestrator_service
    test_orchestrator_service.cpp
    ../src/orchestrator_service.cpp
    ../src/fault_client.cpp
)

target_link_libraries(test_orchestrator_service
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
    protobuf::libprotobuf
    schedinfo_proto
    node_control_proto
    pthread
)

add_test(NAME OrchestratorServiceTest COMMAND test_orchestrator_service)

set_tests_properties(OrchestratorServiceTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
