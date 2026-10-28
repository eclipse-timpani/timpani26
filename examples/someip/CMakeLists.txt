# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)
project(vsomeip_examples C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sample-apps/src
)

# TIMPANI Real-Time client library mockup
add_library(timpani_rt SHARED
    src/libtimpani_rt.cpp
    ../../sample-apps/src/libttsched.c
)

# Add subdirectories
add_subdirectory(radar_based_fcw)
