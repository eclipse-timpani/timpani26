<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-009: Data-Driven Workload — Phase 0 Low-Level Design

**Date:** 2026-10-27
**Last Updated:** 2026-11-01
**Status:** Draft — proposed internal-processing and interface contract (implementation not complete)
**Parent Baseline:** [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, [SRS-009](SRS-009-data-driven-workload.md), [HLD-009](HLD-009-data-driven-workload.md)
**Design Decisions:** [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01`..`04`, [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, `D-P0-06`

---

## 1. Design Boundary, Pre-Approval Items, and Existing Integration Points

This document details only the Phase 0 common DAG data structure, ready/dispatch separation, TT/CBS execution adapters, and prechecks and test integration points. Phase 1 Window/mixed `E2E` and the Phase 2 `SKIP`/recovery state machine are not included.

| Existing Integration Point | Phase 0 Detailed Design Intent | Approval / Compatibility Constraints |
|:---|:---|:---|
| `timpani-o/proto/*.proto` | Task-level execution-class information and common Pipeline dependency metadata independent of the class | **`OI-DD-01` approval** |
| `sample-apps/src/libttsched.h/c` | Common ready/dispatch sequences, `wait_data`/`signal_ready`, and TT/CBS adapter connection | **`OI-DD-02` approval** |
| `timpani-o/src/global_scheduler.*` | Generate each Task's existing TT/CBS placement after DAG validation and link graph metadata to the hash | Preserve existing APIs |
| `timpani-n/src/main.cpp` | Locally verify and register TT/CBS entries together with the common graph metadata | Do not recalculate per-layer feasibility |
| `timpani-n/src/timer_master.*` | Dispatch pending ready for an L1 Task in an allowed slot | Do not perform L2 release |
| `timpani-n/src/bpf/timpani.bpf.c` | Reuse the existing `SCHED_TYPE_TT`/`CBS` dispatch gates | No separate DSQ |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` Shared-Memory Layout (`libttsched.h`)

According to [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` and [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, retain the existing header and `tasks[TIMPANI_MAX_TASKS]` (offset `0..783`) byte for byte, and place the `pipeline[TIMPANI_MAX_TASKS]` region after them.

```c
#define TIMPANI_TTSCHED_MAGIC 0x54494D50U /* "TIMP" */
#define TIMPANI_MAX_TASKS     32
#define TIMPANI_MAX_OUT_EDGES 8

/* Trigger mode constants */
#define TIMPANI_TRIG_ALL 0U /* Wake when all immediate predecessor Tasks complete (required in Phase 0) */
#define TIMPANI_TRIG_ANY 1U /* Wake when one or more immediate predecessors complete (constant reserved for extension) */

/* Predecessor-completion bit region (up to 16 immediate predecessors) */
#define TRIG_READY_MASK  0x0000FFFFU

/* Existing slot structure — no changes to size (24B) or field offsets */
struct timpani_task_slot {
    char     name[16]; /* Task comm name (PR_SET_NAME, <=15 characters + NUL) */
    uint32_t counter;  /* Timer Master periodic slot firing counter */
    uint32_t _pad;
};

struct timpani_out_edge {
    uint16_t dst_slot_idx; /* SHM index of the Successor Task (0..TIMPANI_MAX_TASKS-1) */
    uint16_t bit_idx;      /* Bit position to set in the Successor ready_mask (0..15) */
};

/* Backward-extended region: Pipeline slot sharing index i with tasks[i] */
#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    uint32_t data_counter;      /* Logical ready sequence incremented when ALL is satisfied */
    uint32_t dispatch_counter;  /* Dispatch sequence allowed by a TT slot or CBS release */
    uint32_t ready_mask;        /* Atomic predecessor-completion bitmask */
    uint32_t required_mask;     /* Required predecessor bits (0 means Source/normal Task) */
    uint16_t num_out_edges;
    uint16_t trigger_mode;      /* TIMPANI_TRIG_ALL(0) */
    uint16_t execution_model;   /* TT or CBS; independent of the Source/Successor role */
    uint16_t _reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

struct timpani_ttsched_shm {
    uint32_t magic;      /* TIMPANI_TTSCHED_MAGIC when stable publication is complete; 0 during publication/termination */
    uint32_t n_tasks;    /* Upper-bound index of occupied slots + 1 */
    uint32_t generation; /* Odd = publication in progress; even (>0) = stably published state */
    uint32_t _pad;
    struct timpani_task_slot     tasks[TIMPANI_MAX_TASKS];    /* Preserve existing region */
    struct timpani_pipeline_slot pipeline[TIMPANI_MAX_TASKS]; /* Phase 0 backward extension */
};
```

---

## 3. L-DD-P0-02 — `libttsched` Synchronization and Stable-Publication Checks (`libttsched.c`)

### 3.1 SHM Mapping and Public API Contract

- `ttsched_init()` attempts to map the entire `sizeof(struct timpani_ttsched_shm)` with `shm_open("/timpani_ttsched", O_RDWR, 0666)` and `PROT_READ | PROT_WRITE`. If `EACCES` occurs in a read-only normal TT container, it falls back to the existing `O_RDONLY` / `PROT_READ` mapping and records `g_shm_writable = 0`.
- Public API signatures *(subject to prior approval, `OI-DD-02`)*:
  ```c
  int ttsched_wait_data(void);    /* 0 on a valid Data-Ready wakeup; -1 when unready/unstable/revoked */
  int ttsched_signal_ready(void); /* 0 on completed signal handling; -1 when unready/unstable/unauthorized/not found */
  ```

### 3.2 Stable-Generation Check, and Detailed `ttsched_signal_ready()` and `ttsched_wait_data()` Logic

```c
static __thread int g_shm_writable = 0;
static __thread char g_my_name[16] = {};
static __thread int g_name_cached = 0;
static __thread uint32_t g_last_dispatch_counter = 0;
static __thread int g_dispatch_counter_synced = 0;

/* Check a stable publication state with acquire ordering (valid magic + nonzero even generation) */
static inline int read_stable_gen(const volatile struct timpani_ttsched_shm* shm,
                                  uint32_t* out_gen)
{
    uint32_t magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    uint32_t gen   = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    if (magic != TIMPANI_TTSCHED_MAGIC || gen == 0U || (gen & 1U) != 0U)
        return -1;
    *out_gen = gen;
    return 0;
}

static inline int is_trigger_satisfied(uint32_t mask, uint32_t req_mask, uint16_t mode)
{
    uint32_t ready = mask & TRIG_READY_MASK;
    if (req_mask == 0U)
        return 0;
    if (mode == TIMPANI_TRIG_ALL)
        return (ready & req_mask) == req_mask;
    return (ready & req_mask) != 0U;
}

int ttsched_signal_ready(void)
{
    uint32_t gen;
    if (!g_shm || !g_shm_writable || read_stable_gen(g_shm, &gen) != 0)
        return -1;

    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }
    if (gen != g_shm_generation) {
        g_shm_generation = gen;
        g_task_idx = -1;
        g_dispatch_counter_synced = 0;
    }
    if (g_task_idx < 0) {
        g_task_idx = find_task_slot(g_shm, g_my_name);
        if (g_task_idx < 0)
            return -1;
    }
    /* Recheck the slot identity (name) */
    if (strncmp((const char*)g_shm->tasks[g_task_idx].name, g_my_name, 15) != 0)
        return -1;

    const volatile struct timpani_pipeline_slot* src = &g_shm->pipeline[g_task_idx];
    uint16_t n_edges = __atomic_load_n(&src->num_out_edges, __ATOMIC_ACQUIRE);
    if (n_edges > TIMPANI_MAX_OUT_EDGES)
        n_edges = TIMPANI_MAX_OUT_EDGES;

    struct timpani_out_edge local_edges[TIMPANI_MAX_OUT_EDGES];
    for (uint16_t i = 0; i < n_edges; i++) {
        local_edges[i].dst_slot_idx = src->out_edges[i].dst_slot_idx;
        local_edges[i].bit_idx      = src->out_edges[i].bit_idx;
    }

    /* After copying the edge snapshot, recheck that the same stable generation is retained */
    if (__atomic_load_n(&g_shm->generation, __ATOMIC_ACQUIRE) != gen ||
        __atomic_load_n(&g_shm->magic, __ATOMIC_ACQUIRE) != TIMPANI_TTSCHED_MAGIC)
        return -1;

    for (uint16_t i = 0; i < n_edges; i++) {
        uint16_t dst_idx = local_edges[i].dst_slot_idx;
        uint16_t bit_idx = local_edges[i].bit_idx;
        if (dst_idx >= TIMPANI_MAX_TASKS || bit_idx >= 16)
            continue;

        volatile struct timpani_pipeline_slot* dst = &g_shm->pipeline[dst_idx];
        uint32_t req  = __atomic_load_n(&dst->required_mask, __ATOMIC_ACQUIRE);
        uint16_t mode = __atomic_load_n(&dst->trigger_mode, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_shm->generation, __ATOMIC_ACQUIRE) != gen)
            return -1;

        uint32_t bit = (1U << bit_idx);
        int do_wake = 0;
        uint32_t old_val = __atomic_load_n(
            (const uint32_t*)&dst->ready_mask, __ATOMIC_ACQUIRE);

        while (1) {
            uint32_t merged = old_val | bit;
            uint32_t next_val = merged;
            do_wake = 0;
            if (is_trigger_satisfied(merged, req, mode)) {
                next_val = 0U; /* Commit the wakeup and consume to 0 for the next period */
                do_wake = 1;
            }
            if (__atomic_compare_exchange_n(
                    (uint32_t*)&dst->ready_mask, &old_val, next_val,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }

        if (do_wake) {
            uint32_t ready = __atomic_add_fetch(
                (uint32_t*)&dst->data_counter, 1U, __ATOMIC_SEQ_CST);

            if (__atomic_load_n(&dst->execution_model, __ATOMIC_ACQUIRE) ==
                TIMPANI_EXEC_CBS) {
                __atomic_store_n((uint32_t*)&dst->dispatch_counter, ready,
                                 __ATOMIC_RELEASE);
                syscall(SYS_futex, (uint32_t*)&dst->dispatch_counter,
                        FUTEX_WAKE, 1, NULL, NULL, 0);
            }
            /* TT target: TimerMaster opens dispatch_counter in an allowed slot. */
        }
    }
    return 0;
}
```

- **`ttsched_wait_data()` consumer contract**:
  1. Verify stable publication and its own Task identity, then synchronize `dispatch_counter`.
  2. Return `0` only when `dispatch_counter` differs from `g_last_dispatch_counter`. This guarantees that the layer-specific scheduler gate, rather than ready itself, has opened.
  3. Use `FUTEX_WAIT(&dispatch_counter, cur)`. A simple wake return, EAGAIN, or EINTR is not a success; recheck for an actual sequence change.
  4. An L2 CBS Task opens `dispatch_counter` in the `ALL` CAS. An L1 TT Task opens it after TimerMaster verifies `data_counter > dispatch_counter` in an allowed slot.

---

## 4. L-DD-P0-03 — `Timpani-N` Prechecks, Stable Publication, and Timer Discipline

### 4.1 Non-RT Node-Local Suitability Checks (`main.cpp`)

Check the following on the non-RT path before applying the table. If a check fails, reject the candidate without changing the active state.
- Name/hash of the common DAG metadata, predecessor references, and edge/capacity limits
- Whether the execution layer in each Task's common metadata corresponds to exactly one TT entry or CBS entry
- Representation ranges for the TT entry's CPU/slot/timing and the CBS entry's CPU/budget/period/deadline
- Verify that the Source/Successor role and execution layer are independent and that the class has not been forcibly changed
- *(N does not recalculate DAG or TT/CBS feasibility.)*

### 4.2 Non-RT Common SHM Graph and Layer Registration

1. The SHM publisher creates a common `pipeline[]` index from the Task hashes for all TT/CBS Tasks.
2. Initialize `execution_model`, `required_mask`, `out_edges`, `data_counter`, and `dispatch_counter` in each pipeline slot.
3. Traverse the predecessor hashes in the common DAG metadata to populate `required_mask` and `out_edges`.
4. Register TT Tasks with the existing `TtSlot`/TimerMaster and CBS Tasks with the existing `CbsConfig`/CbsState.
5. Publication itself does not increment ready/dispatch sequences; it releases them in a stable generation.

### 4.3 RT TT/CBS Execution Adapters

- **L1 TT Source or normal TT:** If `required_mask == 0`, dispatch in the same manner as the existing slot firing.
- **L1 TT Successor:** At a slot time, advance `dispatch_counter` once to the corresponding ready state and perform a futex wake only when `data_counter > dispatch_counter`; do not execute if no ready state exists.
- **L2 CBS Source or normal CBS:** Retain the existing Event release path.
- **L2 CBS Successor:** After the `ALL` CAS creates ready, immediately advance `dispatch_counter` and perform a futex wake. BPF applies the CBS CPU, budget, and Gap rules.

| Path | Allowed | Prohibited |
|:---|:---|:---|
| **Non-RT preparation** | Register common graph + TT/CBS entries | Partially reflect a candidate |
| **TT adapter** | Dispatch pending ready in a slot | Wake an L1 Successor with no ready state |
| **CBS adapter** | Release `ALL` ready to CBS | Bypass TT/CBS timing |
| **libttsched** | Atomic operations and futex | Consume a control wake as execution |
| **BPF** | Existing TT/CBS gates, budget, and slot rules | Separate Data-Driven DSQ |

### 4.4 Per-Layer Enforcement in `scx_timpani`

- A TT Task uses the existing `SCHED_TYPE_TT` path, but a dependent Task does not become runnable until the TT adapter opens the dispatch wake.
- A CBS Task uses the existing `SCHED_TYPE_CBS` path, and `ALL` ready is an additional condition for CBS release.
- For both TT and CBS, `DMISS`, budget, and trace decisions use the existing Task-layer rules. Do not mistake Data-Ready itself for a scheduler fault.

---

## 5. L-DD-P0-04 — `Timpani-O` Candidate Validation and Deterministic Topological Placement (`global_scheduler.{h,cpp}`)

1. **Protect the committed model and preserve Task class**: Construct and validate the candidate first. O calculates the DAG role only from `predecessors` and preserves each Task's explicit/inherited `TemporalClass` as is. On rejection, retain the committed model.
2. **Deterministic DAG validation and topological ordering (`ValidateAndSortDag`)**:
   - Use only `std::map<std::string, size_t> task_index`, `std::map<std::string, uint32_t> in_degree`, and `std::map<std::string, std::vector<std::string>> adj` (`std::unordered_map` is prohibited).
   - Checks: ① Task name length (`1..15` bytes ASCII) and `fnv1a_hash()` collisions; ② self-reference (`pred == t.task_id`) or nonexistent reference (`task_index.find(pred) == end`); ③ `> 16` immediate predecessors or `> 8` outgoing edges per vertex; and ④ after Kahn's Algorithm based on the lexicographically ordered `std::set<std::string> ready_queue`, `out_topo_rank.size() != task_index.size()` (cyclic dependency). Return `InfeasibleReason::InvalidDag` on violation.
3. **Class-independent table construction (`build_table`)**:
   - Create a common `PipelineTaskConfig` for every Task hash.
   - Create a `TtSlot` from the input timing for an L1 Task and a `CbsConfig` from the input timing for an L2 Task. Source/Successor does not determine this branch.
   - O uses the DAG topological order only as a tie-breaker for TT slot collision checks or CBS admission checks.
4. **Phase 0 placement**:
   - Use the existing TT placement for TT Tasks and existing CBS admission/worst-fit for CBS Tasks.
   - Phase 1 sums TT Window and CBS Gap/budget delay for each path in a mixed DAG.

---

## 6. Minimum Protobuf Interface Extension (`OI-DD-01`, Subject to Prior Approval)

> **Caution**: Update the actual `.proto` file only after explicit approval by the Project Leader (`CON-DD-05`).

```protobuf
// timpani-o/proto/schedinfo.proto (package schedinfo.v1)
message TaskInfo {
  // ... existing fields ...
  // Task-level TemporalClass is explicit/inherited, independent of predecessors.
  optional TemporalClass temporal_class = <approved_number>;
  repeated string predecessors = <approved_number>;
}

message PipelineTaskConfig {
  uint64 task_id_hash = 1;
  repeated uint64 predecessor_hashes = 2;
  uint32 trigger_mode = 3; // ALL
}

message HierarchicalScheduleTable {
  // ... existing fields ...
  repeated PipelineTaskConfig pipeline_tasks = <approved_number>;
}

// TT/CBS entries remain unchanged except for separately approved fields.
```

---

## 7. L-DD-P0-05 — Test Integration Points

Unit tests shall verify that the DAG is processed without inferring an execution layer and that L1/L2 connections execute through the gate for each layer.

| Test | Verification |
|:---|:---|
| O DAG/placement | `predecessors` and the execution class are independent, and topological order and TT/CBS placement are independent of input order |
| N registration | Common pipeline metadata is connected to the correct TT/CBS entry and the class is unchanged |
| SHM | `ALL` ready occurs once; partial/control wake does not execute; ready≠dispatch is distinguished |
| L1 Successor | If ready precedes the slot, dispatch only in the slot; if no ready exists, no slot dispatch occurs |
| L2 Successor | `ALL` ready connects to the existing CBS release, and budget/Gap is observed |
| Combinations | Pass each of TT→TT, TT→CBS, CBS→TT, and CBS→CBS at least once |

Record hardware verification as not performed if target execution is not performed.
