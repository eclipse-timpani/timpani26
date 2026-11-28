#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# Configure and build one C/C++ component with CMake and write a summary.
#
# Usage: build.sh <timpani-o|libtrpc|sample-apps>
#
# Environment (all optional):
#   BUILD_DIR          Build directory     (default: dist/build/<component>)
#   BUILD_TYPE         CMake build type    (default: Release)
#   SANITIZE           e.g. "address,undefined" -> -fsanitize=<value>
#   COVERAGE           "1" -> add --coverage and build with -O0
#   BUILD_TESTS        timpani-o only: ON/OFF for the unit tests   (default: ON)
#   EXTRA_CMAKE_ARGS   Extra arguments appended to the cmake configure step
set -euo pipefail

COMPONENT="${1:?usage: build.sh <timpani-o|libtrpc|sample-apps>}"

PROJECT_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$PROJECT_ROOT"

BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/dist/build/$COMPONENT}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
REPORT_DIR="$PROJECT_ROOT/dist/reports/build"
LOG_FILE="$REPORT_DIR/${COMPONENT}_build.log"
SUMMARY="$REPORT_DIR/${COMPONENT}_summary.md"
mkdir -p "$REPORT_DIR" "$BUILD_DIR"

cmake_args=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")
c_flags=""

case "$COMPONENT" in
  timpani-o)
    cmake_args+=(-DBUILD_TESTS="${BUILD_TESTS:-ON}")
    ;;
  libtrpc | sample-apps)
    ;;
  *)
    echo "Unknown component: $COMPONENT" >&2
    exit 2
    ;;
esac

if [ -n "${SANITIZE:-}" ]; then
  c_flags="$c_flags -fsanitize=${SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
fi
if [ "${COVERAGE:-0}" = "1" ]; then
  c_flags="$c_flags --coverage -O0 -g"
fi
if [ -n "$c_flags" ]; then
  cmake_args+=(-DCMAKE_C_FLAGS="$c_flags" -DCMAKE_CXX_FLAGS="$c_flags")
  if [ -n "${SANITIZE:-}" ]; then
    cmake_args+=(-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${SANITIZE}"
                 -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=${SANITIZE}")
  fi
fi

# shellcheck disable=SC2206
cmake_args+=(${EXTRA_CMAKE_ARGS:-})

git config --global --add safe.directory "$PROJECT_ROOT" || true

echo "Configuring $COMPONENT (${BUILD_TYPE}) ..." | tee "$LOG_FILE"
# pipefail (set above) makes each pipeline fail when cmake fails.
status="FAILED"
if cmake -S "$COMPONENT" -B "$BUILD_DIR" "${cmake_args[@]}" 2>&1 | tee -a "$LOG_FILE" &&
   cmake --build "$BUILD_DIR" --parallel "$(nproc)" 2>&1 | tee -a "$LOG_FILE"; then
  status="succeeded"
fi

warnings=$(grep -c "warning:" "$LOG_FILE" || true)
{
  echo "# Build report: $COMPONENT"
  echo
  echo "- Status: **$status**"
  echo "- Build type: $BUILD_TYPE"
  echo "- Sanitizer: ${SANITIZE:-none}"
  echo "- Coverage instrumentation: ${COVERAGE:-0}"
  echo "- Compiler warnings: $warnings"
} > "$SUMMARY"

if [ "$status" != "succeeded" ]; then
  echo "::error ::Build of $COMPONENT failed (see $LOG_FILE)"
  exit 1
fi
echo "Build of $COMPONENT succeeded"
