#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# Build timpani-o with coverage instrumentation, run its unit tests and
# produce gcovr reports (Cobertura XML, LCOV, HTML and a Markdown summary).
#
# Environment (all optional):
#   COVERAGE_MIN_LINES  Minimum line coverage in percent; the script fails
#                       below this value (default: 0 = report only)
set -euo pipefail

PROJECT_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$PROJECT_ROOT"

COMPONENT="timpani-o"
export BUILD_DIR="$PROJECT_ROOT/dist/build/${COMPONENT}-coverage"
export BUILD_TYPE=Debug
export COVERAGE=1
export REPORT_TAG=coverage
OUT="$PROJECT_ROOT/dist/coverage/$COMPONENT"
mkdir -p "$OUT/html"

rm -rf "$BUILD_DIR"
"$PROJECT_ROOT/scripts/cpp/build.sh" "$COMPONENT"
"$PROJECT_ROOT/scripts/cpp/test.sh" "$COMPONENT"

# Only measure product code, not tests or generated protobuf/gRPC sources.
gcovr --root "$PROJECT_ROOT" \
  --filter "$PROJECT_ROOT/$COMPONENT/src/" \
  --exclude-throw-branches \
  --xml "$OUT/cobertura.xml" \
  --lcov "$OUT/lcov.info" \
  --html-details "$OUT/html/index.html" \
  --print-summary \
  "$BUILD_DIR" | tee "$OUT/summary.txt"

line_cov=$(sed -nE 's/^lines: *([0-9.]+)%.*/\1/p' "$OUT/summary.txt" | head -n 1)
min="${COVERAGE_MIN_LINES:-0}"

{
  echo "# Coverage report: $COMPONENT"
  echo
  echo "- Line coverage: **${line_cov:-unknown}%**"
  echo "- Minimum required: ${min}%"
} > "$OUT/coverage_summary.md"

if [ -n "$line_cov" ] && awk -v c="$line_cov" -v m="$min" 'BEGIN{exit !(c < m)}'; then
  echo "::error ::Line coverage ${line_cov}% is below the required ${min}%"
  exit 1
fi
