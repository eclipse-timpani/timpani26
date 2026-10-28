# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

# Radar Producer executable (TT Simulator)
add_executable(mock_radar_producer
    mock_radar_producer.cpp
)
# Mock vsomeip stub lives here, separate from real vsomeip3 headers used by hello_world/.
target_include_directories(mock_radar_producer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../mock_include)
target_link_libraries(mock_radar_producer timpani_rt pthread)

# FCW Consumer executable (CBS Simulator)
add_executable(fcw_consumer
    fcw_consumer.cpp
)
target_include_directories(fcw_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../mock_include)
target_link_libraries(fcw_consumer timpani_rt pthread)

# Dummy Tasks executable (for task_A, task_B, task_C)
add_executable(dummy_tasks
    dummy_tasks.cpp
)
target_link_libraries(dummy_tasks timpani_rt pthread)

