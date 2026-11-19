<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-009: Data-Driven Workload — Phase 0 Software Requirements Specification

**Date:** 2026-10-27
**Last Updated:** 2026-11-01
**Status:** Draft — Phase 0 only (not implementation completion or approval for functional-safety operation)
**Parent Baseline:** [SRS-000](SRS-000-timpani-system.md) §5.1 `REQ-DD-001`, [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, §9
**Related Documents:** [HLD-009](HLD-009-data-driven-workload.md), [DDR-009](DDR-009-data-driven-workload.md), [LLD-009](LLD-009-data-driven-workload.md), [REQ-011](REQ-011-runtime-table-update.md), [DDR-011](DDR-011-runtime-table-update.md)
**Terminology Baseline:** [REQ-009](REQ-009-data-driven-workload.md) §2 and [DDR-012](DDR-012-fault-management.md) §2

---

## 1. Purpose and Applicability Rules

This document specifies only the software requirements needed to achieve **Phase 0 (basic chain operation verification)** of the Data-Driven Workload defined in [REQ-009](REQ-009-data-driven-workload.md). The Phase 1 requirements (`Activation Window` control, offline E2E verification, and successful runtime updates of tables that contain a Pipeline) and the Phase 2 requirements (reporting input omission as `SKIP` and `DATA_TIMEOUT`, and safe initialization of recovery state) are not automatically inherited through this document.

Requirement IDs use the prefixes `REQ-DD-P0-F-###` (functional) and `REQ-DD-P0-N-###` (non-functional) to distinguish them from full-phase requirements (`REQ-DD-###`). All active entries in the tables are **SHALL** requirements (mandatory) within the Phase 0 scope. Mapping to the compound-phase parent requirements (`REQ-DD-001`, `002`, `015`, and `013`) is **limited to the Phase 0 applicability in [REQ-009](REQ-009-data-driven-workload.md) §1.5.3** and does not signify completion of the corresponding parent IDs in their entirety.

---

## 2. Phase 0 Test Boundary

| Category | Phase 0 Condition |
|:---|:---|
| **Execution Target** | On a single node, connect Tasks into the same DAG when their original `TemporalClass` is L1 TT or L2 CBS. Do not change the execution layer according to the DAG role. |
| **Chain Composition** | Use a same-period basic chain and combinations of mixed L1/L2 Tasks. Each Task retains the TT timing or CBS timing of its existing layer. |
| **Time and State Baseline** | Use fixed `E/H/table`. `ALL` establishes a common ready state. L1 executes in an allowed TT slot, and L2 executes at a CBS release/gate. The ready state is consumed atomically once. |
| **Runtime Update Boundary** | **Do not perform runtime updates (`UPD-REPLACE / ADD / REMOVE / CLEAR`) of tables that contain a Pipeline during testing** (deferred to Phase 1). Verification of stable publication at `/timpani_ttsched` and non-execution of the control wake ([DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`) is nevertheless mandatory, as is regression prevention for normal Phase 0 updates of general TT tables that do not contain a Pipeline under [REQ-011](REQ-011-runtime-table-update.md). |
| **Inputs and Responsibilities** | Submit the DAG through the `Timpani-O` input (or test input path), and have only `Timpani-O` validate and generate it and transmit it through the existing `O → N` path. Do not determine End-to-End success solely by `Timpani-N` or direct SHM injection. |
| **Interface and Approval Constraints** | Preserve existing public C++ methods and the initial layout of `timpani_task_slot`. A `.proto` extension (`OI-DD-01`), backward extension of SHM, and a new public `libttsched` API (`OI-DD-02`) require Project Leader approval before being incorporated into code. |

Harmonic multi-period chains, precise time windows by layer, quantitative `E2E Feasibility`, `SKIP`, and `DATA_TIMEOUT` are excluded from the Phase 0 scope. Phase 0 uses the existing TT or CBS scheduler policy for CPU/slot placement.

---

## 3. Phase 0 Functional Requirements

### 3.1 Input, Generation, and Pre-Rejection (`Timpani-O` & `Timpani-N`)

| ID | Requirement Specification | Parent REQ | REQ Acceptance Criteria |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-001** | Only `Timpani-O` shall resolve and validate predecessor dependencies expressed as `TaskReference(workload_id, task_id)`. An empty `workload_id` shall normalize to the current workload, and another workload reference shall be rejected in Phase 0. Cycles, self-reference, nonexistent references, duplicate Task pairs, `(workload_id_hash, task_id_hash)` collisions, more than `16` predecessors, or more than `8` outgoing edges shall be rejected with a reason without changing the committed model or active table. | REQ-DD-001 | AC-DD-P0-01, AC-DD-P0-04 |
| **REQ-DD-P0-F-002** | O shall use an explicitly present `TaskInfo.temporal_class`, otherwise inherit `SchedInfo.temporal_class`. It shall preserve the resolved class while creating the existing `TtSlot` or `CbsConfig`, and transmit common DAG metadata separately with the `(workload_id_hash, task_id_hash)` pair. | REQ-DD-002, 009 | AC-DD-P0-01, AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-F-003** | N shall verify that each common DAG Task key maps to exactly one TT or CBS entry. The key shall be the `(workload_id_hash, task_id_hash)` pair; unsuitable candidates shall be rejected without recomputing TT/CBS admission equations. | REQ-DD-001, 002, CON-DD-03 | AC-DD-P0-04 |

### 3.2 Runtime `Data-Ready` Synchronization and Scheduling Enforcement (`Timpani-N` & `libttsched`)

| ID | Requirement Specification | Parent REQ | REQ Acceptance Criteria |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-004** | An L1/L2 Source shall retain the existing Timer Master/Event activation path. A Successor shall satisfy `ALL` for one active chain-job token and its layer-specific dispatch gate, and publication/shutdown/slot-reuse wake shall not become ready or dispatch. | REQ-DD-002, 004 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-005** | `signal_ready(job_token)` shall, inside the publication commit guard, snapshot and validate the complete outgoing-edge list and preflight every destination before any `ready_state` mutation. Edge-count overflow, invalid/self/duplicate destination, out-of-range bit, required-mask mismatch, invalid trigger/model, or a destination token/duplicate-bit failure shall mutate no mask, sequence, dispatch counter, or wake in the fan-out. Direct duplicate state returns `EALREADY`; a different token returns `ESTALE`. After preflight, each edge uses one lock-free 64-bit CAS; an identical concurrent commit that sets the bit or advances the sequence is treated as convergence. CBS `dispatch_counter` updates/wakes occur only after all edge CAS operations complete. | REQ-DD-004, 005, 007 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-006** | Before the first Pipeline-extension dereference in each `signal_ready`/`wait_data` transaction, the client shall verify the retained mapping length, the mapped descriptor's current `fstat()` size, and that the current SHM name resolves to the same file identity. It shall then verify extension `magic/version/abi_size`, stable `generation`, Task identity, and the commit guard. Only Timpani-N may perform `ftruncate()`/`shm_unlink()` object-lifecycle operations; workload clients shall not resize or unlink the object. `wait_data` shall return the dispatched job token; `dispatch_counter` shall be the sole futex wait/wake word. Truncation/replacement, ABI/generation/writer-pending, duplicate-bit, or token-mismatch failure shall mutate no state and shall set `errno`; truncation/replacement is `ESTALE`. | REQ-DD-004, 015 | AC-DD-P0-03, AC-DD-P0-05 |
| **REQ-DD-P0-F-007** | Timer Master shall increment and wake an L1 Successor only at its own TT slot when pending ready exists; a slot with no ready shall have `0` dispatch. An L2 Successor shall increment and wake `dispatch_counter` in the completed `ALL` fan-out commit only after all outgoing-edge CAS operations have finished, after which BPF CBS CPU/budget/Gap gates alone decide actual execution. | REQ-DD-008, 009 | AC-DD-P0-01, AC-DD-P0-05 |

### 3.3 Test Observation and Handling of Departures from Assumptions

| ID | Requirement Specification | Parent REQ | REQ Acceptance Criteria |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-008** | Test logs shall correlate the table identifier, job token, Task-level start/completion `CLOCK_MONOTONIC` timestamps, the time spent in `ttsched_signal_ready()`, and the pre-rejection reason. A duplicate-token or token-mismatch rejection shall record `EALREADY` or `ESTALE`. If a Phase 0 test prerequisite, such as connectivity or clock integrity, is violated, the test shall not be judged successful; instead, it shall be aborted and the cause recorded. Automatic recovery or timeout `SKIP` shall not be assumed to operate. | REQ-DD-013 | AC-DD-P0-01..06 |

---

## 4. Phase 0 Non-Functional Requirements

| ID | Requirement Specification | Parent REQ | REQ Acceptance Criteria |
|:---|:---|:---|:---|
| **REQ-DD-P0-N-001** | No dynamic memory allocation, blocking IPC/gRPC, data-bearing file/network I/O, unbounded busy-spin, or blocking lock shall be added to the modified RT critical paths. The bounded `shm_open`/`fstat`/`close` metadata calls used solely to revalidate the mapped POSIX SHM object are an explicit exception. A `publication_guard` reader shall use non-waiting fetch-add/fetch-sub and return `EAGAIN` or `ESTALE` on contention; the publisher shall defer publication after a timeout. The 64-bit `ready_state` atomic SHALL be lock-free on the target architecture, and workload containers SHALL operate without `CAP_BPF`. | REQ-DD-007, CON-DD-04 | AC-DD-P0-06 |
| **REQ-DD-P0-N-002** | `signal_ready` overhead, including current SHM size and name-identity revalidation, shall be no more than `5 µs`, and an L1/L2 mixed graph shall not degrade the existing timing criteria of either layer. | REQ-DD-007, 008, 013 | AC-DD-P0-01, AC-DD-P0-05 |
| **REQ-DD-P0-N-003** | DAG ordering, common dependency metadata, and layer-specific TT/CBS placement shall be independent of input order. The execution layer shall not be determined by the presence or absence of `predecessors`. | REQ-DD-001, 002, 009, CON-DD-04 | AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-N-004** | Interface changes shall be incorporated after approval, and inputs that exceed the TT/CBS/SHM/edge limits shall be rejected in advance. | REQ-DD-001, 015, CON-DD-05, CON-DD-06 | AC-DD-P0-04, AC-DD-P0-05 |

---

## 5. Phase 0 Verification Scenarios

Use a fixed `Epoch`/`Hyperperiod` (`100ms`) and a predesignated Isolated CPU with the output generated by `Timpani-O`. Record host-level tests and End-to-End execution evidence in a Linux 6.12+ `sched_ext` target environment separately.

| Test ID | Input / Stimulus | Observation and Success Criteria | REQ Acceptance Criteria |
|:---|:---|:---|:---|
| **TC-DD-P0-001** | In a fixed DAG, mix explicit/inherited Task classes and exercise L1→L1, L1→L2, L2→L1, and L2→L2. Give every predecessor of one chain job the same token, place L1 `ALL` before a slot, just after a slot, and omit it; also vary CBS budget and TT-active state. | L1 has `0` pre-slot `dispatch_counter` increments; ready present before a slot increments once at that slot; ready created just after a slot produces `0` wake in that slot and increments once at the next slot. Normal tests contain at most one pending ready. L2 releases exactly once immediately after `ALL`; the consumer receives the dispatched token. Class/order/cross-token regressions are `0`, and `signal_ready <= 5 µs`. | AC-DD-P0-01 |
| **TC-DD-P0-002** | Complete only some predecessors for separate L1/L2 Successors. While A's token `k` remains set and B has not signaled, inject duplicate `(A,k)`, future `A(k+1)`/`B(k+1)`, and an L1 slot or control wake. Then use a Source with two outgoing edges and invalidate only its later edge (`num_out_edges`, index, required bit, mode/model, duplicate destination), or make its later destination `ESTALE`/`EALREADY`; finally send valid `B(k)`. | Duplicate `(A,k)` returns `EALREADY`; nonactive tokens return `ESTALE`. Any later-edge validation/preflight failure leaves mask/sequence/dispatch/wake unchanged across the complete fan-out and produces `0` partial notification. The later valid `B(k)` completes exactly one `ALL` for token `k`; a valid multi-edge CBS fan-out performs all CAS operations before the first wake, and a control wake returns `ESTALE`. | AC-DD-P0-02 |
| **TC-DD-P0-003** | Submit a cycle (`A → B → A`), self-reference, nonexistent reference, a Phase 0 reference to another workload, a duplicate Task pair, hash-pair collision, and capacity/edge-limit overruns to the `Timpani-O` and `Timpani-N` prechecks. | Verify rejection and the reason before activation; the `Timpani-O` committed model and the `Timpani-N` active table and SHM state remain unchanged. | AC-DD-P0-04 |
| **TC-DD-P0-004** | Exercise a legacy 784-byte object and a full object mapped for only the 784-byte prefix, extension magic/ABI-size mismatch, unsupported ABI version, `magic=0`, odd `generation`, writer-pending, O_RDONLY `signal_ready`, a previously full mapping whose object is truncated below the ABI size, an unlink/recreate replacement under the SHM name, and controlled interleavings in which publication starts immediately before or after the packed CAS. Also inject publication/shutdown control wake without a dispatch increment. | Legacy/short mapping keeps only the TT API and returns `ENOTSUP`; magic/ABI-size errors return `EINVAL`, while a valid-magic unsupported ABI version returns `ENOTSUP`. Truncation/replacement, unstable publication, and control wake return `ESTALE` without `pipeline_header`/`pipeline[]` access, mutation, or `SIGBUS`. Replacement requires reinitialization. Write denial returns `EACCES`. Packed CAS, dispatch, and wake serialize completely, with `0` mixed-generation records and `0` control-wake executions. | AC-DD-P0-03 |
| **TC-DD-P0-005** | Exercise the mixed L1/L2 DAG plus existing TT/CBS and 011 normal-update regressions. For TT, create ready before a slot; for CBS, force budget=0 and an active TT interval. | Adding the DAG leaves Sources on their existing counter/Event paths. TT follows the slot gate, CBS follows release then budget/Gap; there are `0` class/scheduling regressions. | AC-DD-P0-05 |
| **TC-DD-P0-006** | Shuffle the submission order and repeatedly generate the same DAG. | The topological order, common dependency metadata, and layer-specific placement are 100% identical. | AC-DD-P0-06 |

---

## 6. Phase 0 Handling of Open and Approval Items

| REQ Item | Phase 0 Decision / Contract | Detailed Rationale |
|:---|:---|:---|
| **OI-DD-01** | `TaskInfo.temporal_class` follows the optional explicit-over-inherit rule. Transmit the DAG with same-workload `TaskReference` values and identify common O→N metadata with the `(workload_id_hash, task_id_hash)` pair. Support only `ALL` in Phase 0, and defer `E2E` to OI-DD-05 **(Interface Approval)**. | DDR-009 `D-DD-P0-03` |
| **OI-DD-02** | Append an extension header and `pipeline[]` after the 784-byte prefix. Conditionally approve the design on the ABI/permission/`errno` contract, fixed-size/replacement lifetime, per-transaction mapping-size/current-object identity revalidation, complete fan-out prevalidation with mutation-free failure and deferred wake, lock-free 64-bit `ready_state` CAS, exact job-token/ready-sequence matching, duplicate/stale-token rejection, dispatch-only futex, O_RDWR/O_RDONLY fallback, legacy-ABI compatibility, and control-wake tests. Withhold implementation approval for any unmet condition **(Conditional SHM/API Approval)**. | DDR-009 `D-DD-P0-01`, `D-DD-P0-04` |
| **OI-DD-03** | In Phase 0, Sources retain existing TT/CBS paths. L1 increments `dispatch_counter` only at its TT slot when ready is pending; L2 increments in the completed `ALL` fan-out commit after all outgoing-edge CAS operations and wakes, after which BPF enforces budget/Gap. Conditionally approve missed-slot retention, at most one normal pending ready, mutation-free rejection of an invalid late edge, and deferral of `Activation Window`/`SKIP`; withhold implementation approval if no-ready, mixed-class, late-ready, or fan-out prevalidation tests fail **(Conditional Phase 0 Execution-Connection Approval)**. | DDR-009 `D-DD-P0-03`, `D-DD-P0-05` |

---

## 7. Traceability and Deferral to Subsequent Phases

All active requirements in §3 and §4 cite a parent [REQ-009](REQ-009-data-driven-workload.md) ID and `AC-DD-P0-01` through `06`; conversely, `AC-DD-P0-01` through `06` correspond to `TC-DD-P0-001` through `006` in §5. [HLD-009](HLD-009-data-driven-workload.md) specifies component responsibilities and flow, [DDR-009](DDR-009-data-driven-workload.md) specifies the key design decisions, and [LLD-009](LLD-009-data-driven-workload.md) specifies the internal-data, synchronization, and verification interfaces and cites the active IDs in this SRS.

| Subsequent Phase | Deferred Scope (Not Included in This Document) |
|:---|:---|
| **Phase 1** | Window, Harmonic periods, `E2E Feasibility` for mixed L1/L2 DAGs, refinement of layer-specific CPU/slot/budget interference, and Pipeline updates |
| **Phase 2** | `SKIP`, root-cause `DATA_TIMEOUT`, Sink Fallback, and common recovery-state initialization including writer-pending/active-commit state |

Do not pre-implement, in Phase 0, the status flags, timer hooks, or recovery state machine for the deferred scope.
