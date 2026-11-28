#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# cppcheck static analysis for the C/C++ sources.
#
# The script fails when cppcheck reports any finding of severity "error".
# Warning / performance / portability findings are listed in the report and
# only fail the run when CPPCHECK_STRICT=1.
set -euo pipefail

PROJECT_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$PROJECT_ROOT"

REPORT_DIR="$PROJECT_ROOT/dist/reports/cppcheck"
FINDINGS="$REPORT_DIR/cppcheck_findings.txt"
SUMMARY="$REPORT_DIR/cppcheck_summary.md"
mkdir -p "$REPORT_DIR"

TARGETS=(timpani-o/src libtrpc/src sample-apps/src)

cppcheck \
  --enable=warning,performance,portability \
  --std=c++17 \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  -I timpani-o/src -I libtrpc/src -I sample-apps/src \
  -j "$(nproc)" \
  --quiet \
  --template='{file}:{line}:{severity}:{id}:{message}' \
  "${TARGETS[@]}" 2> "$FINDINGS" || true

errors=$(grep -c ':error:' "$FINDINGS" || true)
warnings=$(grep -c ':warning:' "$FINDINGS" || true)
perf=$(grep -c ':performance:' "$FINDINGS" || true)
portability=$(grep -c ':portability:' "$FINDINGS" || true)

{
  echo "# cppcheck report"
  echo
  echo "- $(cppcheck --version)"
  echo "- Targets: ${TARGETS[*]}"
  echo "- Errors: **$errors**"
  echo "- Warnings: $warnings"
  echo "- Performance: $perf"
  echo "- Portability: $portability"
  if [ -s "$FINDINGS" ]; then
    echo
    echo "## Findings"
    echo
    echo '```'
    head -n 200 "$FINDINGS"
    echo '```'
  fi
} > "$SUMMARY"
head -n 12 "$SUMMARY"

if [ "$errors" -gt 0 ]; then
  echo "::error ::cppcheck reported $errors error(s); see $FINDINGS"
  grep ':error:' "$FINDINGS" | head -n 50
  exit 1
fi
if [ "${CPPCHECK_STRICT:-0}" = "1" ] && [ -s "$FINDINGS" ]; then
  echo "::error ::cppcheck reported findings and CPPCHECK_STRICT=1"
  exit 1
fi
