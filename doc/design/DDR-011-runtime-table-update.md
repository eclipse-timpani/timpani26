<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-011: Runtime Table Update — Phase 0 Design Decisions

**Date:** 2026-10-28  
**Status:** Draft — Phase 0 design proposal, subject to PL review  
**Parent baseline:** [REQ-011](REQ-011-runtime-table-update.md), [SRS-011](SRS-011-runtime-table-update.md)  
**Related design:** [HLD-011](HLD-011-runtime-table-update.md), [LLD-011](LLD-011-runtime-table-update.md)

## 1. Decision Scope

This document records only the decisions needed to confirm four normal updates on a single node, with one or more pre-designated isolated CPUs (core pinning of retained tasks), L1 TT, and a fixed time grid. It does not replace the approval history or decision numbers of the existing full-feature DDRs. Decisions in this folder are identified by `D-P0-##`. The adoptions below apply only to the design proposals in this document and do not imply approval of implementation, hardware verification, or interface changes.

## 2. D-P0-01 — Single Path Through the Existing Full Snapshot

**Decision:** REPLACE, ADD, REMOVE (including recovery STOP), and CLEAR all use the existing `ControlCommand.full_table`. ADD and REMOVE are semantic results, not separate delta messages. In Phase 0, the recovery STOP sends only a replacement `full_table` to the owning node, and it does not use a `RecoverySignal` broadcast or node-side synthesis of a removal table. An empty `partitions` is also a valid table with a positive H and the same E. `ControlCommand.update` is rejected on the Phase 0 path without changing state.

**Rationale:** The existing O→N transfer is reused and no delta-merge or baseline-revision protocol is created, while the four outcomes in the original requirement text can still be verified. The operation type and target are kept in O's request context or test log, and no proto field is added.

**Rejected alternatives:** parallel delta messages, new RPCs, multi-node transactions.

**Traceability:** REQ-RTU-P0-F-001, REQ-RTU-P0-F-004. REQ OI-01.

## 3. D-P0-02 — O Input, Recovery STOP, and One-at-a-Time Confirmation

**Decision:** On the single node's pre-designated isolated CPU set (`0..63`), workload-set changes are connected to the existing O generation path. `confirmed` and one `candidate` are kept separate, and additional requests are rejected while one is in progress. The confirmed node-wide execution frame `H` and epoch `E` remain fixed throughout Phase 0, including CLEAR. For each non-empty candidate, O derives the candidate periods' minimum LCM, verifies that the periods form a harmonic set and that the minimum LCM divides `H`, then expands the slots over the same `H`; this minimum repeat cycle does not replace the execution frame. Retained tasks keep their repeated slots and originally assigned CPUs, and only new workloads are assigned CPUs, preferring unused isolated CPUs (or, when there are not enough, the least-loaded CPU). Spreading a single task across multiple CPUs, or moving a retained task to another CPU, is rejected (cross-core migration is in Phase 1). The candidate is sent only after the generation check, schedulability judgment, and confirmation that retained parameters are preserved, and it is confirmed after a matching successful ACK.

Recovery removal uses the existing `EnforceRecoveryPolicy(RECOVERY_STOP)`. General REMOVE, CLEAR, and REPLACE tests use an O-internal test entry point, and no new RPC is added for normal deployment release. `RECOVERY_STOP` also follows the same candidate-removal path (an empty table when the last workload is removed) and the single in-flight rejection rule, but, so that the caller can terminate a workload only after node runtime cleanup is complete, it returns completion after confirming a successful correlated `TableApplied` within a finite wait deadline, and it keeps the session. Pre-rejection, in-progress conflict, or an unconfirmed result returns failure; harmless convergence of repeated STOPs is not included. `table_id` uses an existing string that is unique within a test run, and no persistent incarnation or retransmission version count is designed.

**Rationale:** A single path is sufficient to verify O generation → N execution, including recovery STOP, without corrupting the existing confirmed model on verification failure. Tests that inject only hand-crafted tables into N are used solely as auxiliary unit tests.

**Traceability:** REQ-RTU-P0-F-002..F-004, REQ-RTU-P0-F-007, REQ-RTU-P0-F-016. REQ OI-17.

## 4. D-P0-03 — Inactive TT Configuration and Actual Kernel Selection

**Decision:** TT slots and task metadata (with `assigned_cpu` recorded as the assigned CPU of each task record) are prepared in two banks, inactive and active, and the same selector is used for both. Writes to the inactive bank SHALL NOT be visible to the current execution, and BPF SHALL consistently use one bank within each processing interval. The per-core count of runnable TT tasks is maintained with 64-bit atomic increments and decrements. While the handover gate is closed, a woken TT task is held in an assigned-CPU queue that no select, enqueue, or dispatch path consumes; it is not inserted into `SCX_DSQ_LOCAL_ON` or `SCX_DSQ_GLOBAL`. After boundary publication completes and the gate opens, tasks present in the new active table are released according to their scheduled slots. A task absent from the new table is moved out of the holding queue through a bounded revalidation-only control wake on its prior assigned CPU: SHM generation and task mapping are already stable, so the application returns without executing a removed TT job. Its held-map state and counters are cleaned before application acknowledgement. This path must not use an unconsumed quarantine/sink DSQ, and a later ADD must be able to reuse the same surviving process. Mutable runtime values in task metadata are not initialized in the active bank because of candidate preparation.

The current user-space index toggle in `BpfLoader::swap_shadow_maps()` and the TODO in `apply_table_update()` are not accepted as an actual switchover. While preserving the names and signatures of the existing public methods, the internal write path and the BPF lookup path SHALL be changed together. CBS maps, recovery maps, and full scheduler reloading are outside the scope of this decision.

The old bank is cleaned up and reused on the non-RT path only after BPF references and use of the old timer context have ended. It uses bounded per-bank reference records and handover confirmation, and it does not perform release or waiting on the RT path. Before completion is confirmed, the next candidate is not accepted.

**Rationale:** Clearing the active maps and refilling them in turn cannot guarantee, even on the normal path, that no mixed table is produced or that pre-rejection has no side effects. Configuration selection and lifetime rules are the minimum synchronization needed for basic atomicity.

**Rejected alternatives:** in-place update of a single live map, changing only the C++ index, duplicating CBS runtime state, a generalized configuration-set retirement framework.

**Traceability:** REQ-RTU-P0-F-005, REQ-RTU-P0-F-006, REQ-RTU-P0-F-008. REQ OI-05.

## 5. D-P0-04 — Resident Timer and TT-free Handover

**Decision:** On the fixed E, the node-wide H, and the pre-designated isolated CPU set, a single resident RT timer is maintained. The next firing time of each slot is computed from the next slot time on the same CPU. Candidate slots, SHM images, and control values are prepared on the non-RT path, and handover is performed in the common TT-free interval after the last old TT slot completes on all isolated CPUs. After confirming that no old execution remains on any isolated CPU involved in the active or candidate tables, TT dispatch is closed, the new kernel configuration is selected, and slot state is initialized for each involved CPU; their effect is not enabled before the future boundary T.

RT processing at the boundary is limited to a fixed number of atomic operations (selecting the prepared context, publication validation, and execution permission) and the existing slot firing. Kernel control values are accessed from the shared control region prepared at load time, and because of the boundary switchover, no `bpf_map_update_elem`, thread creation or join, dynamic allocation, or file/network I/O is added to the RT path. The alignment, memory ordering, and kernel visibility of the shared control region are verified in target testing.

If preparation is not complete before handover starts, the next `E+kH` is selected. A failure after the start of handover is not subject to automatic restoration and is a Phase 0 test failure. The length of the TT-free interval is recorded in the test configuration but is not guaranteed as a worst-case application latency or commit-window SLA. Even for CLEAR, E/H and the resident control path are maintained.

**Rationale:** Stopping and restarting the timer on each update would truncate in-progress slots and add unnecessary thread lifetime management. By pinning retained tasks to their assigned CPUs and sharing a node-wide H, multi-isolated-CPU switchover can be performed with only a single resident timer and fixed boundaries, without a new per-core timer pool or a multithreaded barrier.

**Traceability:** REQ-RTU-P0-F-009..F-011, REQ-RTU-P0-N-001, REQ-RTU-P0-N-002. REQ OI-05.

## 6. D-P0-05 — Valid Publication and Job-Wake Separation on the Existing SHM Layout

**Decision:** The structs, sizes, magic, and return values of the [existing SHM header](../../sample-apps/src/libttsched.h) are maintained. `generation` is used as a monotonically increasing publication order; an odd generation is interpreted as publication in progress, and an even generation as a stable state. At the start of writing, the generation is switched to odd and the data is then recorded, and at boundary T the even generation and magic are made valid in release order. Consumers discard snapshots that are being published or have changed, using acquire reads and a before-and-after comparison of the generation.

The indices and job counters of retained tasks are preserved. Publication does not increment the counters of all tasks; only actual slot firings increment them. A consumer does not treat the futex return alone as success, but verifies validity, generation, task identity, and the actual counter change. If a task is woken by another task because of removal or slot reuse, it does not execute the job.

When the table is unreadable, the existing `-1` contract and bounded re-checks are used, and no busy-spin is performed. If invalidity persists, it is recorded as a test abort at the test harness limit. Quantitative verification of the wake-up guarantee and bound immediately after removal, and of no loss of the first job, is deferred to Phase 1.

**Conditions:** Tests use N and libttsched that implement this publication rule together. It does not claim that old consumer binaries satisfy the new behavior. Layout and public API are not changed; if implementation review determines that a breaking change to the existing interface contract is required, work does not proceed before separate approval. The initial publication is also made a stable even generation, and test inputs that require a generation wrap-around are not accepted.

**Rationale:** Magic-last alone does not guarantee both that a control wake after waiting is not misidentified and that reads overlapping with writes are consistent. Using the existing generation field and counter makes it possible to distinguish the two events without a new SHM ABI.

**Traceability:** REQ-RTU-P0-F-012..F-014. REQ OI-06.

## 7. D-P0-06 — Rejection Within the Current Representation and Capacity

**Decision:** The proto schema is not changed. N only checks lossless conversion, fixed test conditions, and local references, and does not recalculate schedulability.

| Representation | Phase 0 Criteria |
|:--|:--|
| Time | Uses the proto's `uint32 hyperperiod_us` and slot us values, and `uint64 epoch_ns`. Nanosecond conversion and boundary calculations check for overflow, and H/E changes are rejected |
| TT slots | Within 2048 entries per bank of the current BPF map. Two-bank duplication is not interpreted as an increase in logical capacity |
| Tasks | The smaller of the current BPF task map size (4096) and SHM `TIMPANI_MAX_TASKS=32`, i.e., **at most 32 unique TT tasks** |
| CPU | Matches the pre-designated isolated CPU set within the 64-bit mask range (`0..63`). Each task belongs to only one isolated CPU, and changes to the assigned CPU of a retained active task (cross-core migration) are rejected |
| Task reference | Uses the existing PID resolution path, but test names are limited to unique ASCII of 1–15 bytes. Name truncation, duplicate PID resolution, and hash collisions between distinct identifiers are rejected |

If the actual build constants differ, the test record and the effective minimum capacity are also checked. Relying only on the hard-coded numbers in this document, the implementation SHALL NOT exceed a smaller actual limit. Checking that slot times fall within the table range is a representation check, not an inter-slot overlap analysis.

**Traceability:** REQ-RTU-P0-F-003, REQ-RTU-P0-F-005, REQ-RTU-P0-N-003. REQ OI-12, OI-15.

## 8. D-P0-07 — ACK After Application and Normal Removal

**Decision:** The existing `TableApplied(table_id, success, error)` is used. Success is reported after the actual boundary application and the cleanup of removal references. For pre-rejection, the active state is confirmed to be unchanged and the failure reason is sent. For a failure during switchover, neither success nor preservation of the previous state is claimed.

On removal, TT maps, metadata, registration and tracking references, and SHM entries are cleaned up, but OS policy/affinity is neither changed nor restored. Container termination is outside the scope of TIMPANI; in recovery STOP, the upper-level orchestrator performs it after O's completion reply. The next request is accepted after the cleanup and O's processing of the current request's result are complete.

**Rejected alternatives:** a new result-code schema, a RESTORE_FAILED state machine, operational unconfirmed-state transitions, automatic retransmission and resynchronization.

**Traceability:** REQ-RTU-P0-F-011, REQ-RTU-P0-F-014..F-016.

## 9. D-P0-08 — Separation of Test Logs and Verification Levels

**Decision:** The existing O/N logs and test result files are used. Planned boundaries, actual completion times, and kernel selection, generation, and execution evidence are linked by the same table ID. Host unit tests and actual TT/BPF/app tests on the target Linux hardware are recorded as separate results.

The finite wait limit of the test harness and the finite ACK wait deadline of the recovery STOP response are values for judging call results and ending tests; they are not operational ACK recovery or timing SLAs. If a test precondition is violated, the test is aborted and no subsequent requests are sent. Existing safety mechanisms are retained, but Phase 0 results are not used as approval for in-vehicle or safety operation.

**Traceability:** REQ-RTU-P0-F-017, REQ-RTU-P0-F-018, REQ-RTU-P0-N-004. REQ OI-10.

## 10. Deferrals and Review Gate

Phase 1 covers CBS, MODIFY, changes to the time grid and isolated CPU configuration, cross-core migration of active tasks, request management (including the pending and repeated-STOP policy), per-core timer expansion and multi-node operation, quantitative quality, and detailed application compatibility. Phase 2 covers rollback, restore failure, unconfirmed and connection-loss recovery, blocking of re-dispatch on STOP failure and baseline resynchronization, and clock and fault management. Hooks or detailed state machines for these are not implemented in advance in Phase 0.

The review gates of the current design are the kernel/timer/SHM consistency of normal boundary switchover, bank reference lifetime, target memory ordering of the shared control region, and preservation of the existing ABI. If these are not satisfied, Phase 0 is not accepted as complete. No implementation plan is written in this document set.
