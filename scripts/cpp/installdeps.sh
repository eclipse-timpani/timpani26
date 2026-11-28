#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# Install the system packages needed to build / test / lint the C/C++
# components (timpani-o, libtrpc, sample-apps).
#
# Usage: installdeps.sh [timpani-o|libs|lint|all]...
#   Several groups may be given. Default: all.
set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

groups=("$@")
if [ ${#groups[@]} -eq 0 ]; then
  groups=(all)
fi

packages=(build-essential cmake git pkg-config)

add_grpc() {
  packages+=(libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc)
}

for g in "${groups[@]}"; do
  case "$g" in
    timpani-o)
      add_grpc
      packages+=(libgtest-dev)
      ;;
    libs)
      packages+=(libsystemd-dev)
      ;;
    lint)
      packages+=(clang-format cppcheck gcovr lcov)
      ;;
    all)
      add_grpc
      packages+=(libgtest-dev libsystemd-dev
                 clang-format cppcheck gcovr lcov)
      ;;
    *)
      echo "Unknown dependency group: $g" >&2
      exit 2
      ;;
  esac
done

echo "Installing packages: ${packages[*]}"
$SUDO apt-get update -y
DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y --no-install-recommends \
  $(printf '%s\n' "${packages[@]}" | sort -u)
