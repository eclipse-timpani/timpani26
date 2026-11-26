<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# TIMPANI-O

**Timpani-O** is the orchestrator component of the TIMPANI real-time scheduling system. It generates and distributes schedule tables to Timpani-N instances via gRPC.

| | |
|---|---|
| **Version** | 2026.04.1 ([CalVer](https://calver.org/)) |
| **Changelog** | [CHANGELOG.md](CHANGELOG.md) |
| **License** | MIT |

## Architecture

```
Pullpiri (Workload Orchestrator)
    │
    │ WorkloadSpec (gRPC :50052)
    ▼
┌─────────────────────────────────────┐
│           TIMPANI-O                 │
│  ┌─────────────────────────────┐    │
│  │ SchedInfoServer (:50052)    │────┤◄─ Pullpiri
│  │ OrchestratorServer (:50060) │────┤◄─► Timpani-N (bidirectional)
│  │ FaultClient (→:50053)       │────┤──► Pullpiri FaultService
│  └─────────────────────────────┘    │
└─────────────────────────────────────┘
    │
    │ HierarchicalScheduleTable (gRPC :50060)
    ▼
TIMPANI-N (Node Executor)
```

| Port | Service | Direction |
|------|---------|-----------|
| 50052 | SchedInfoServer | Pullpiri → timpani-o |
| 50060 | OrchestratorServer | timpani-n ↔ timpani-o |
| 50053 | FaultService (client) | timpani-o → Pullpiri |

---

## Quick Start

### Container (Recommended)

```bash
cd timpani-o

# Build with version info
VERSION=$(cat ../VERSION)
podman build --build-arg VERSION=$VERSION \
  -t timpani-o:$VERSION .

# Verify version label
podman inspect timpani-o:$VERSION --format '{{index .Config.Labels "version"}}'
# 2026.04.1

# Run (mount examples/node_configurations.yaml)
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:$VERSION \
  -c /config/node_configurations.yaml
```

### Native Build

```bash
cd timpani-o

# Prerequisites (Ubuntu)
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

# Build
mkdir build && cd build
cmake .. && make

# Run (from build directory)
./timpani-o -c ../examples/node_configurations.yaml
```

---

## Build

### Prerequisites

**Ubuntu:**
```bash
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

**CentOS:**
```bash
sudo dnf install -y protobuf-devel protobuf-compiler epel-release
sudo dnf install -y grpc-devel
```

### Native Build

```bash
git clone https://github.com/eclipse-timpani/timpani26
cd TIMPANI/timpani-o
mkdir build && cd build
cmake ..
make
```

**Cross-compilation (ARM64):**
```bash
# From build directory
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-aarch64-gcc.cmake ..
make
```

**Packaging:**
```bash
# From build directory
cpack -G DEB   # or RPM, TGZ
```

### Container Build

timpani-o embeds version as a container image **label** (not in the binary).
`GIT_COMMIT_HASH` is not supported — pass `VERSION` only.

```bash
cd timpani-o

# Build with version info
VERSION=$(cat ../VERSION)
podman build --build-arg VERSION=$VERSION \
  -t timpani-o:$VERSION .

# Verify version label
podman inspect timpani-o:$VERSION --format '{{index .Config.Labels "version"}}'
```

---

## Run

### Native

```bash
# From build directory (default config)
./timpani-o -c ../examples/node_configurations.yaml

# Without config
./timpani-o

# Help
./timpani-o -h
```

### Container

```bash
# Default (mount host config)
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:latest \
  -c /config/node_configurations.yaml

# Without config
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  timpani-o:latest

# Connect to Pullpiri on host
podman run -d --name timpani-o \
  --network=host \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:latest \
  -c /config/node_configurations.yaml -f localhost -p 50053

# Using compose
podman-compose up -d
```

---

## Testing

```bash
# From timpani-o directory
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make

# Run all tests
make test

# Run individual tests
./tests/test_schedinfo_service
./tests/test_fault_client
./tests/test_global_scheduler
./tests/test_node_config
./tests/test_runtime_table_update
```

**Test prerequisites:**
```bash
# Ubuntu
sudo apt install -y libgtest-dev

# CentOS
sudo dnf install -y gtest-devel
```

---

## Opt-in Phase 0 runtime table updates

The default path is unchanged: CBS, multiple nodes and the existing replay/retry
behavior remain available. Enable the separate Phase 0 path explicitly:

```bash
# Edit the node ID and isolated CPU in this example before a target trial.
./build/timpani-o --runtime-table-update-phase0 -c examples/phase0_node.yaml
```

Phase 0 requires one configured node, one already isolated CPU, L1 TT only,
and an ACK-confirmed fixed epoch/hyperperiod. It does not configure OS affinity.
Connect N and pre-register trial processes before submitting the baseline.

- `AddSchedInfo.status=0` means candidate acceptance, not application. The
  existing non-RT main loop sends the candidate once through `full_table`.
- Normal `RemoveWorkload` submits a removal candidate. REPLACE/CLEAR use private
  O test/control helpers, not new RPCs. All four operations use O's generator.
- Confirmed inputs and tables change only after a matching successful
  `(node_id, table_id)` ACK. `GetScheduleTables` exposes confirmed tables only.
  Generation failure and a matching negative ACK leave them unchanged.
- Only one attempt is retained, including while awaiting its first connection.
  Further requests are rejected immediately; no queue, merge, retry or replay.
- ADD/REMOVE preserve retained slots. A candidate minimum LCM must divide the
  confirmed H; generated slots are repeated over H instead of overwriting it.
  A multiple or non-divisor H is rejected. CLEAR preserves E/H for later ADDs.
- Limits: 32 unique tasks, 2048 slots, ASCII workload/task IDs of 1..15 bytes.
  CBS, MODIFY, multiple nodes/CPUs, changed E/H/CPU and reused task identities
  across workloads are outside Phase 0.
- Write failure, disconnect, unexpected ACK or recovery STOP stops subsequent
  submissions until an externally re-established fresh trial. Existing fault
  reporting and STOP forwarding stay enabled; STOP preserves the last confirmed
  O model rather than pretending to complete a normal REMOVE.

The two opt-in C++ connection methods are `SchedInfoServer::EnablePhase0` and
`PushPendingTables`. Enable before either gRPC server starts; the orchestrator
must outlive the SchedInfo server. No existing public method, proto or SHM ABI is
removed or changed. Default container/Compose configuration is unchanged.

Run the focused suite with
`ctest --test-dir build -R '^RuntimeTableUpdateTest$' --output-on-failure`.
It uses the plan's W1/W2/W3 fixture and real local gRPC streams/ACKs, and checks
both opt-in invariants and default CBS/delta behavior.

`test_phase0_e2e` is the target E2E driver (P0-T06). It acts as timpani-o for
one real timpani-n and runs the plan's REPLACE, ADD, REMOVE, CLEAR, rejection
and busy sequence through `full_table`. CTest reports it as skipped unless
`TIMPANI_P0_E2E=1` is set:

```bash
TIMPANI_P0_E2E=1 ./build/tests/test_phase0_e2e
```

The driver listens on `TIMPANI_P0_E2E_PORT` (default 50060) and loads
`TIMPANI_P0_E2E_NODE_CONFIG` (default `examples/phase0_node.yaml`). It waits up
to `TIMPANI_P0_E2E_CONNECT_S` (60) seconds for the node and up to
`TIMPANI_P0_E2E_ACK_TIMEOUT_S` (10) seconds for each ACK. It runs
`TIMPANI_P0_E2E_ROUNDS` (3) rounds. Start the sample apps and
`timpani-n --runtime-table-update-phase0` first, and restart timpani-n before
each run. See §5.4 of the
[Phase 0 plan](../doc/design/DDR-011-runtime-table-update-phase0/implementation_plan.md)
for the target procedure and the evidence to collect.

timpani-n acknowledges a table only after the boundary switch and the old-state
cleanup (P0-T06). Host and mock-node results do not prove BPF/SHM activation;
Linux target evidence is still required.

---

## Development

### Coding Style

- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with modifications:
  - 4 spaces indentation
  - Line break before opening brace (functions/classes)
- Format with `clang-format -i <file>`

---

## Registry (Optional)

For deployment to internal registry:

```bash
podman login sdv.lge.com
podman tag timpani-o:latest sdv.lge.com/timpani/timpani-o:v0.1.0
podman push sdv.lge.com/timpani/timpani-o:v0.1.0
```
