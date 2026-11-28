#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# Run the CTest suite of a component that was built with build.sh and
# produce a JUnit XML report plus a Markdown summary.
#
# Usage: test.sh <component>
#
# Environment (all optional):
#   BUILD_DIR   Build directory (default: dist/build/<component>)
#   REPORT_TAG  Suffix for the report file names (default: none)
set -euo pipefail

COMPONENT="${1:?usage: test.sh <component>}"

PROJECT_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$PROJECT_ROOT"

BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/dist/build/$COMPONENT}"
TAG="${REPORT_TAG:+-$REPORT_TAG}"
TEST_DIR="$PROJECT_ROOT/dist/tests"
REPORT_DIR="$PROJECT_ROOT/dist/reports/test"
JUNIT="$TEST_DIR/${COMPONENT}${TAG}-junit.xml"
LOG_FILE="$REPORT_DIR/${COMPONENT}${TAG}_test.log"
SUMMARY="$REPORT_DIR/${COMPONENT}${TAG}_summary.md"
mkdir -p "$TEST_DIR" "$REPORT_DIR"

if [ ! -d "$BUILD_DIR" ]; then
  echo "Build directory $BUILD_DIR does not exist; run build.sh first" >&2
  exit 2
fi

# Sanitizer runtime options (no-ops when the build is not instrumented).
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

status="passed"
if ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 120 \
     --output-junit "$JUNIT" 2>&1 | tee "$LOG_FILE"; then
  status="passed"
else
  status="FAILED"
fi

summary_line=$(grep -E "tests passed|No tests were found" "$LOG_FILE" | tail -n 1 || true)
{
  echo "# Test report: $COMPONENT${TAG}"
  echo
  echo "- Status: **$status**"
  echo "- Result: ${summary_line:-n/a}"
} > "$SUMMARY"

if [ "$status" != "passed" ]; then
  echo "::error ::Tests of $COMPONENT failed (see $LOG_FILE)"
  exit 1
fi
