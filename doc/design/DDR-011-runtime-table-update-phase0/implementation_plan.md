<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-011 Phase 0 Implementation Plan

## 1. Scope

This plan covers one Phase 0 runtime-table-update trial on one configured
node. It is limited to TT workloads, a fixed epoch and hyperperiod, one
pre-designated isolated CPU set, and the operations `REPLACE`, `ADD`, `REMOVE`
(including recovery `STOP`), and `CLEAR`.

The authoritative behavior is defined by
[DDR-011](../DDR-011-runtime-table-update.md),
[SRS-011](../SRS-011-runtime-table-update.md), and
[LLD-011](../LLD-011-runtime-table-update.md). The Phase 0 path does not add a
protobuf field or alter the existing SHM layout. CBS, `MODIFY`, cross-core
migration, multi-node transactions, rollback, and automatic retry are out of
scope.

## 2. Work Breakdown

| Task | Component | Result |
|:--|:--|:--|
| P0-T01 | `timpani-o` | Stage candidate tables until a matching ACK |
| P0-T02 | `timpani-n` | Validate a full table before staging any active state |
| P0-T03 | BPF loader | Prepare inactive TT/task maps and protect the old bank |
| P0-T04 | Timer/BPF | Hand over at a TT-free boundary without restarting the RT thread |
| P0-T05 | SHM/sample apps | Publish stable generations and distinguish jobs from control wakes |
| P0-T06 | O/N integration | Apply, clean up, and acknowledge before the next request |

Each task must preserve the active baseline on pre-rejection and must keep the
confirmed epoch, hyperperiod, CPU assignment, retained slot repetitions, and
workload identity rules. A failure after the handover starts stops the Phase 0
trial instead of claiming rollback.

## 3. Host Verification

Build and run the focused component tests after each task:

```bash
cmake -S timpani-o -B timpani-o/build -DBUILD_TESTS=ON
cmake --build timpani-o/build
ctest --test-dir timpani-o/build --output-on-failure

cmake -S timpani-n -B timpani-n/build \
  -DBUILD_TESTS=ON -DCONFIG_TRACE_BPF=ON
cmake --build timpani-n/build
ctest --test-dir timpani-n/build --output-on-failure
```

Required host evidence includes the Phase 0 candidate/ACK tests, bank and SHM
protocol tests, handover timeout/cancellation tests, and default-path
compatibility tests. `Phase0E2ETest` is target-gated and remains skipped when
`TIMPANI_P0_E2E` is unset.

## 4. Target Setup

1. Build the current branch of `sample-apps`, `timpani-o`, and `timpani-n`.
2. Configure a single node with one pre-designated isolated CPU set and the
   target-compatible `phase0_node.yaml` fixture.
3. Start the sample workloads named by the fixture before starting Timpani-N.
4. Start Timpani-O with `--runtime-table-update-phase0` and wait for its gRPC
   services to listen.
5. Start Timpani-N with `--runtime-table-update-phase0`, the matching node ID,
   and the Timpani-O address.
6. Confirm that both sides loaded the same epoch/hyperperiod and that the
   target BPF object passed the kernel verifier.

## 5. Target Procedure

Run the `test_phase0_e2e` driver after the target node is ready:

```bash
TIMPANI_P0_E2E=1 \
TIMPANI_P0_E2E_NODE_CONFIG=timpani-o/examples/phase0_node.yaml \
  ./timpani-o/build/tests/test_phase0_e2e
```

The driver performs these ordered checks:

1. Apply baseline workloads W1 and W2.
2. `REPLACE` them with W1 and W3.
3. `ADD` and reject the absent-task workload W4.
4. `REMOVE` W2/W3 according to the active fixture.
5. `CLEAR` the candidate while preserving epoch and hyperperiod.
6. Rebuild a table from the remaining fixture workloads.
7. Verify that operations submitted during an in-flight update are rejected.

Stop after the first fatal protocol failure. A failed application, missing ACK,
unexpected node disconnect, non-deterministic workload set, or stale bank
reference invalidates the remaining sequence. Restart Timpani-N before a new
trial.

## 5.4 Evidence to Collect

Collect the following artifacts from the same run:

- Timpani-O logs containing each `table_id`, operation, target workloads, and
  matching `TableApplied` result.
- Timpani-N logs containing `planned_boundary_ns`, `actual_apply_ns`, active
  bank, SHM generation, held-task drain result, and old-bank cleanup result.
- Kernel/BPF load and sched_ext verification output, including isolated CPU
  detection and scheduler attach status.
- Perfetto and gpdata artifacts that show retained TT slots remain on the fixed
  grid across the update.
- Sample-app runtime/deadline output and the target test summary.

Host tests alone do not approve target timing, kernel memory ordering, BPF
verifier behavior, or first-job wake correctness. Record those results as
separate evidence.

## 6. Completion Criteria

Phase 0 is complete for review only when every normal operation in the target
sequence succeeds, rejected operations leave the confirmed baseline unchanged,
all host tests pass, no held task executes from an old table, and no old BPF bank
is cleared while referenced. The evidence must identify the exact commit and
record that the work remains target-validation material, not a safety approval.
