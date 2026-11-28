#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# clang-format check for the C/C++ sources (timpani-o, sample-apps).
#
# The existing code base is not fully clang-format clean, so the check is
# incremental:
#   * BASE_REF set   -> only lines changed since BASE_REF are checked
#                       (git clang-format --diff); the script fails if any of
#                       those lines are not formatted per .clang-format.
#   * BASE_REF unset -> the whole tree is scanned and reported, but the script
#                       does not fail unless FMT_STRICT=1.
#
# libtrpc and libbpf follow the kernel style (see .editorconfig) and are not
# part of this check.
set -euo pipefail

PROJECT_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$PROJECT_ROOT"

REPORT_DIR="$PROJECT_ROOT/dist/reports/cpp-fmt"
SUMMARY="$REPORT_DIR/fmt_summary.md"
DIFF_FILE="$REPORT_DIR/fmt.diff"
mkdir -p "$REPORT_DIR"
: > "$DIFF_FILE"

git config --global --add safe.directory "$PROJECT_ROOT" || true

ROOTS=(timpani-o sample-apps)
EXTENSIONS="c,cpp,h,hpp"

# Tracked C/C++ files that are subject to the check.
list_files() {
  git ls-files -- "${ROOTS[@]}" | grep -E '\.(c|cpp|h|hpp)$' || true
}

status="clean"
mode=""

if [ -n "${BASE_REF:-}" ] && git rev-parse --verify --quiet "${BASE_REF}^{commit}" >/dev/null; then
  mode="incremental (changed lines since ${BASE_REF})"
  files=$(list_files)
  if [ -z "$files" ]; then
    echo "No C/C++ files to check"
  else
    # shellcheck disable=SC2086
    out=$(git clang-format --diff --extensions "$EXTENSIONS" "$BASE_REF" -- $files 2>&1 || true)
    case "$out" in
      *"did not modify any files"* | *"no modified files to format"* | "")
        status="clean"
        ;;
      *)
        status="unformatted"
        printf '%s\n' "$out" > "$DIFF_FILE"
        ;;
    esac
  fi
  unformatted_count="n/a"
else
  mode="full-tree scan (report only unless FMT_STRICT=1)"
  unformatted=()
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    if ! clang-format --dry-run --Werror "$f" >/dev/null 2>&1; then
      unformatted+=("$f")
      clang-format "$f" | diff -u "$f" - >> "$DIFF_FILE" || true
    fi
  done < <(list_files)
  unformatted_count=${#unformatted[@]}
  if [ "$unformatted_count" -gt 0 ]; then
    status="unformatted"
  fi
fi

{
  echo "# C/C++ format report (clang-format)"
  echo
  echo "- Mode: $mode"
  echo "- Status: **$status**"
  echo "- clang-format: $(clang-format --version)"
  echo "- Files not conforming (full-tree scan only): ${unformatted_count:-n/a}"
  if [ "$status" = "unformatted" ]; then
    echo
    echo "Apply the suggested formatting with:"
    echo
    echo '```bash'
    echo 'git clang-format --extensions c,cpp,h,hpp <base-branch>'
    echo '```'
  fi
} > "$SUMMARY"
cat "$SUMMARY"

if [ "$status" = "unformatted" ]; then
  if [ -n "${BASE_REF:-}" ] || [ "${FMT_STRICT:-0}" = "1" ]; then
    echo "::error ::clang-format check failed; see $DIFF_FILE"
    head -n 80 "$DIFF_FILE" || true
    exit 1
  fi
  echo "::warning ::Existing files are not clang-format clean (report only)"
fi
