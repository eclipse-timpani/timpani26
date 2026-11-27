<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# TIMPANI-N

**Timpani-N** is the node executor component of the TIMPANI real-time scheduling system. It uses eBPF/sched_ext for deterministic task execution with microsecond precision.

## Architecture

```
TIMPANI-O (Orchestrator)
    │
    │ HierarchicalScheduleTable (gRPC :50060)
    ▼
┌─────────────────────────────────────────────┐
│              TIMPANI-N                      │
│  ┌───────────────────────────────────────┐  │
│  │ NodeClient (→:50060)                  │──┤──► timpani-o
│  │ TimerMaster (CLOCK_REALTIME ABSTIME)  │  │
│  │ BPF Loader (sched_ext scheduler)      │  │
│  │ FaultMonitor (ringbuf → gRPC)         │  │
│  └───────────────────────────────────────┘  │
│                    │                        │
│         ┌─────────┴─────────┐               │
│         ▼                   ▼               │
│  ┌─────────────┐    ┌──────────────┐        │
│  │ BPF Maps    │    │ /dev/shm/    │        │
│  │ tt_table    │    │ timpani_ttsched│       │
│  │ task_meta   │    │ (futex)      │        │
│  └─────────────┘    └──────────────┘        │
└─────────────────────────────────────────────┘
    │
    │ Futex wakeup / sched_ext dispatch
    ▼
Time-Triggered Workloads (sample-apps, etc.)
```

| Component | Description |
|-----------|-------------|
| NodeClient | gRPC client connecting to timpani-o |
| TimerMaster | High-precision timer using CLOCK_REALTIME |
| BPF Loader | Loads sched_ext BPF scheduler |
| FaultMonitor | Monitors deadline misses via BPF ringbuf |

---

## Quick Start

### Package Installation (Recommended)

```bash
# Debian/Ubuntu
sudo apt install ./timpani-n_2026.04.1_x86_64.deb

# RedHat/CentOS
sudo dnf install ./timpani-n-2026.04.1-1.x86_64.rpm

# Configure and start
sudo vi /etc/timpani/timpani-n.conf
sudo systemctl start timpani-n
```

### Native Build

```bash
cd TIMPANI/timpani-n

# Prerequisites (Ubuntu 22.04+)
sudo apt install -y libelf-dev zlib1g-dev clang linux-tools-$(uname -r) \
    libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

# Build
mkdir build && cd build
cmake .. && make

# Verify version (includes Git commit and build time)
./timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC

# Run (requires root for BPF)
sudo ./timpani-n 192.168.1.100  # timpani-o host
```

---

## Build

### Prerequisites

**Ubuntu 22.04+:**
```bash
# BPF dependencies
sudo apt install -y libelf-dev zlib1g-dev clang linux-tools-$(uname -r)

# gRPC dependencies
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

**CentOS Stream 10:**
```bash
# Development tools
sudo dnf group install -y "Development Tools"
sudo dnf install -y cmake

# BPF dependencies
sudo dnf install -y clang bpftool elfutils-libelf-devel zlib-devel

# gRPC dependencies
sudo dnf install -y grpc-devel protobuf-devel
```

### Native Build

```bash
git clone --recurse-submodules https://github.com/eclipse-timpani/timpani26
cd TIMPANI/timpani-n
mkdir build && cd build

# Local build — Git hash is auto-detected from the repo
cmake .. && make

# CI / package build — no git history available, pass explicitly
VERSION=$(cat ../../VERSION)
GIT_HASH=$(git rev-parse --short HEAD)
cmake -DGIT_COMMIT_HASH=$GIT_HASH .. && make

# Verify
./timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_TRACE_BPF` | ON | Enable eBPF sched_ext scheduler |
| `CONFIG_TRACE_EVENT` | ON | Enable ftrace for sched/timer events |
| `CONFIG_TRACE_BPF_EVENT` | OFF | Track sched_switch/waking events |
| `BUILD_TESTS` | OFF | Build host unit tests (no BPF or target needed) |
| `TIMPANI_KERNEL_BTF_FILE` | `/sys/kernel/btf/vmlinux` | Target kernel BTF ELF used for API detection and `vmlinux.h` generation |
| `TIMPANI_SCX_API` | `AUTO` | Select `AUTO`, `LEGACY`, or `DSQ` sched_ext kfunc family |

With `TIMPANI_SCX_API=AUTO`, CMake detects the helper set from the target BTF.
Mainline Linux 6.12 uses the legacy API; the DSQ API is available from 6.13.
The BTF-based selection also handles kernels that backport either helper set.
For cross-builds, point `TIMPANI_KERNEL_BTF_FILE` at the target kernel's BTF ELF.
Set `TIMPANI_SCX_API` explicitly only when the target BTF exposes both complete
helper sets and the build should prefer one of them.

```bash
# Disable BPF (fallback to timer-only mode)
cmake -DCONFIG_TRACE_BPF=OFF ..
```

### Unit Tests

The DDR-011 Phase 0 host tests build without BPF, so they can run on a
development host:

| Test | Covers |
|------|--------|
| `test_phase0_prepare` | Table pre-checks and `NPrepared` assembly (P0-T02) |
| `test_bpf_loader` | TT bank routing, handover, and staging helpers (P0-T03) |
| `test_libttsched` | `/timpani_ttsched` even/odd generation publication and libttsched wake rules (P0-T05) |
| `test_phase0_handover` | Boundary handover between the coordinator and the resident RT engine, and the SHM image written during the TT-free window (P0-T04) |
| `test_phase0_attempt` | One Phase 0 attempt on the main loop: the checks and old-bank cleanup before a successful `TableApplied`, rejections without side effects, and trial failures (P0-T06) |

```bash
cmake -S . -B build-tests -DCONFIG_TRACE_BPF=OFF -DBUILD_TESTS=ON
cmake --build build-tests --target test_phase0_prepare test_bpf_loader \
    test_libttsched test_phase0_handover test_phase0_attempt
cd build-tests && ctest --output-on-failure
```

These tests do not cover BPF, `/proc`, or cgroup behavior on the target.
`test_libttsched` uses in-process memory and a scripted wait instead of the
real SHM object and futex. `test_phase0_handover` simulates time and the
kernel operations; it does not exercise the real gate, BPF references, or RT
latency. `test_phase0_attempt` replaces TimerMaster, BPF, SHM and FaultMonitor
with a fake. Memory ordering must still be verified on the target.
`test_bpf_loader` needs `<linux/types.h>` (a Linux host or a header shim).

### Cross-compilation (ARM64)

```bash
mkdir build-arm64 && cd build-arm64
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64-gcc.cmake ..
make
```

---

## Packaging

Build DEB, RPM, or TGZ packages:

```bash
cd build

# Debian/Ubuntu
cpack -G DEB
# Output: timpani-n_2026.04.1_x86_64.deb

# RedHat/CentOS
cpack -G RPM
# Output: timpani-n-2026.04.1-1.x86_64.rpm

# Tarball
cpack -G TGZ
```

The version in the package filename and the version embedded in the binary both come from the root `VERSION` file at build time. Verify after installation:

```bash
timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC
```

The package includes:
- `/usr/bin/timpani-n` - Binary
- `/lib/systemd/system/timpani-n.service` - Systemd unit
- `/etc/timpani/timpani-n.conf` - Configuration

---

## Installation

### Package Installation (Recommended)

**Debian/Ubuntu:**
```bash
sudo apt install ./timpani-n_2026.04.1_x86_64.deb
```

**RedHat/CentOS:**
```bash
sudo dnf install ./timpani-n-2026.04.1-1.x86_64.rpm
```

The package automatically:
- Installs binary and systemd service
- Creates `/etc/timpani/` configuration directory
- Runs `systemctl daemon-reload`
- Enables the service (doesn't start automatically)

### Manual Installation

```bash
# Binary
sudo cp build/timpani-n /usr/bin/

# Configuration
sudo mkdir -p /etc/timpani
sudo cp timpani-n.conf /etc/timpani/

# Systemd service
sudo cp timpani-n.service /lib/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable timpani-n
```

---

## Configuration

Edit `/etc/timpani/timpani-n.conf`:

```bash
# Orchestrator (timpani-o) connection
ORCHESTRATOR_HOST=192.168.1.100
ORCHESTRATOR_PORT=50060

# Node identity (empty = use hostname)
NODE_ID=node1
```

---

## Systemd Service

### Service Management

```bash
# Start/stop/restart
sudo systemctl start timpani-n
sudo systemctl stop timpani-n
sudo systemctl restart timpani-n

# Status and logs
sudo systemctl status timpani-n
journalctl -u timpani-n -f
```

### Required Capabilities

timpani-n requires elevated privileges for BPF and real-time scheduling:

| Capability | Purpose |
|------------|---------|
| `CAP_BPF` | Load BPF programs |
| `CAP_SYS_ADMIN` | sched_ext, BPF map operations |
| `CAP_SYS_NICE` | Real-time priority (SCHED_FIFO) |
| `CAP_SYS_RESOURCE` | setrlimit for memlock |

These are configured in the systemd unit file with security hardening:
- `ProtectSystem=strict`
- `PrivateTmp=yes`
- `NoNewPrivileges=yes`

---

## Command Line Options

```
Usage: timpani-n [options] [orchestrator_host]

Options:
  -n <name>   Node ID override (default: hostname)
  -p <port>   Orchestrator gRPC port (default: 50060)
  --runtime-table-update-phase0
              Enable the DDR-011 Phase 0 table update path
  -V          Print version, Git commit, and build timestamp
  -h          Show help
```

`--runtime-table-update-phase0` is opt-in and should be paired with the same
flag on timpani-o. Without it, the existing table path is unchanged. With it,
timpani-n:

- checks each full table on the main non-RT loop before touching active state
  (node ID, fixed epoch and hyperperiod, one isolated CPU, TT only, slot range,
  ID/hash consistency, a single live PID per task, at most 32 tasks and 2048
  slots) and rejects a failing table with `TableApplied(success=false)`;
- preserves each retained workload's complete slot multiset (task identity,
  offset, duration, deadline, CPU, current limit, layer and repetition count).
  Changes are rejected before PID resolution or staging. Snapshot order
  and slot ordinals may change; adding/removing whole workloads and CLEAR
  remain supported;
- rejects a full table that arrives while another is in progress (`busy`) and
  every delta `update` command;
- leaves the OS CPU affinity of removed tasks unchanged.

An accepted table is staged in the inactive BPF bank. The resident RT thread
of TimerMaster switches to it at a future boundary `T = E + kH`, after the old
slots end, and publishes the prepared SHM image there. timpani-n sends
`TableApplied(success=true)` only after all of the following:

- the active bank changed, the published even SHM generation is visible, and
  the actual apply time is not before `T`;
- the old bank was cleared once BPF stopped referencing it (retried every 1 ms
  for up to 100 ms);
- workload tracking was rebuilt and FaultMonitor was reset with the new
  limits.

A rejection before any active change returns `success=false` with the reason.
A failure after the handover started is not acknowledged and the previous
table is not restored. timpani-n logs `result=trial_failed` and disconnects,
and timpani-o stops the trial. Restart timpani-n before the next trial. Each
attempt logs one `[RTU-P0]` line with `node_id`, `table_id`,
`added_workloads`, `removed_workloads`, `planned_boundary_ns`,
`actual_apply_ns`, `active_bank`, `shm_gen` and `result`/`reason`. The `-g`
gpdata output does not follow Phase 0 table changes. Linux target validation
is still required; see the
[Phase 0 plan](../doc/design/DDR-011-runtime-table-update-phase0/implementation_plan.md).

**Example:**
```bash
# Connect to timpani-o at 192.168.1.100:50060 as "node1"
sudo ./timpani-n -n node1 -p 50060 192.168.1.100
```
