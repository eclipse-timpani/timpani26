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
| `timpani-o/proto/*.proto` | Optional Task-class override, structured `TaskReference`, and workload/task hash-pair common DAG metadata | **`OI-DD-01` approval** |
| `sample-apps/src/libttsched.h/c` | Versioned Pipeline extension, packed `ready_state`, dispatch-only futex, `wait_data`/`signal_ready`, and ABI/`errno` contract | **`OI-DD-02` approval** |
| `timpani-o/src/global_scheduler.*` | Resolve the Task class with explicit-over-inherit and generate TT/CBS placement separately from graph metadata after DAG validation | Preserve existing APIs |
| `timpani-n/src/main.cpp` | Locally verify and register TT/CBS entries together with the common graph metadata | Do not recalculate per-layer feasibility |
| `timpani-n/src/timer_master.*` | Dispatch pending ready for an L1 Task in an allowed slot | Do not perform L2 release |
| `timpani-n/src/bpf/timpani.bpf.c` | Reuse the existing `SCHED_TYPE_TT`/`CBS` dispatch gates | No separate DSQ |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` Shared-Memory Layout (`libttsched.h`)

According to [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` and [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, retain the existing header and `tasks[TIMPANI_MAX_TASKS]` (offset `0..783`) byte for byte. Append a versioned Pipeline extension header and the `pipeline[TIMPANI_MAX_TASKS]` region after that prefix.

```c
#define TIMPANI_TTSCHED_MAGIC       0x54494D50U /* "TIMP" */
#define TIMPANI_PIPELINE_MAGIC      0x50495045U /* "PIPE" */
#define TIMPANI_PIPELINE_ABI_VERSION 1U
#define TIMPANI_MAX_TASKS           32
#define TIMPANI_MAX_OUT_EDGES       8

#define TIMPANI_PUB_WRITER_PENDING 0x80000000U
#define TIMPANI_PUB_ACTIVE_MASK    0x7FFFFFFFU

#define TIMPANI_TRIG_ALL 0U /* Sole valid trigger in Phase 0 */
#define TIMPANI_TRIG_ANY 1U /* Reserved for a later phase; Phase 0 must not publish it */

#define TIMPANI_READY_MASK         0x0000FFFFULL
#define TIMPANI_READY_SEQUENCE_SHIFT 32
#define TIMPANI_READY_SEQUENCE_MASK  0xFFFFFFFFULL

/* Existing slot structure — no changes to size (24B) or field offsets */
struct timpani_task_slot {
    char     name[16]; /* Task comm name (PR_SET_NAME, <=15 characters + NUL) */
    uint32_t counter;  /* Timer Master periodic slot firing counter */
    uint32_t _pad;
};

struct timpani_out_edge {
    uint16_t dst_slot_idx; /* SHM index of the Successor Task (0..TIMPANI_MAX_TASKS-1) */
    uint16_t bit_idx;      /* Bit position in the Successor ready mask (0..15) */
};

#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    uint64_t workload_id_hash;
    uint64_t task_id_hash;
    uint64_t ready_state;
    /* ready_state: [63:32] ready sequence, [31:16] reserved, [15:0] ready mask */
    uint32_t required_mask;
    uint32_t dispatch_counter; /* TT/CBS execution count; sole Data-Driven futex word */
    uint16_t num_out_edges;
    uint16_t trigger_mode;     /* Phase 0: TIMPANI_TRIG_ALL */
    uint16_t execution_model;  /* TT/CBS; independent of Source/Successor */
    uint16_t reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

/* Backward extension header placed at byte 784, immediately after tasks[] */
struct timpani_pipeline_header {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t reserved;
    uint32_t abi_size;          /* Total sizeof(struct timpani_ttsched_shm) */
    uint32_t publication_guard; /* bit31 = writer pending; lower 31 bits = active commits */
};

struct timpani_ttsched_shm {
    uint32_t magic;
    uint32_t n_tasks;
    uint32_t generation;
    uint32_t _pad;
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
    struct timpani_pipeline_header pipeline_header;
    struct timpani_pipeline_slot pipeline[TIMPANI_MAX_TASKS];
};

_Static_assert(offsetof(struct timpani_ttsched_shm, pipeline_header) == 784,
               "preserve the existing 784-byte SHM prefix");
_Static_assert(sizeof(struct timpani_pipeline_header) == 16,
               "keep the pipeline slots 8-byte aligned");
_Static_assert(sizeof(struct timpani_pipeline_slot) % sizeof(uint64_t) == 0,
               "pipeline slot must be 64-bit aligned");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
               "64-bit ready_state CAS must be lock-free");
```

A new client first uses `fstat()` to inspect the actual object size. If the object is shorter than the 784-byte prefix, even the TT path returns `EINVAL`. If the prefix exists but the complete extension does not, the existing `ttsched_wait_next_period()` path remains usable. Pipeline APIs access `pipeline[]` only after the actual size and extension `magic`, ABI `version`, and `abi_size` have all been verified; otherwise they return `ENOTSUP` or `EINVAL`.

The `publication_guard` in the extension header closes the TOCTOU window between generation validation and state mutation. The publisher atomically sets writer-pending to block new commits, waits only for a bounded time until active commits reach `0`, makes `generation` odd, and updates SHM. A reader increments the active count only while writer-pending is clear and rechecks the same stable generation. Therefore, either the old-generation reader completes before publication or a reader rejected after publication starts changes no state.

---

## 3. L-DD-P0-02 — `libttsched` Synchronization and Stable-Publication Checks (`libttsched.c`)

### 3.1 SHM Mapping and Public API Contract

- Retain the existing APIs: `ttsched_init()` continues to support a read-only mapping for the TT prefix, and the signature and behavior of `ttsched_wait_next_period()` do not change.
- `ttsched_init()` calls `fstat()` after `shm_open()`. At least the 784-byte prefix must exist for the legacy TT path. Pipeline access is enabled only when the object is large enough for the full `struct timpani_ttsched_shm` and the extension `magic/version/size` is valid.
- A Data-Driven Source that must write opens `O_RDWR` + `PROT_READ|PROT_WRITE`. If access is denied, it records `g_shm_writable = 0` and falls back to the read-only TT path. A read-only Successor may use `ttsched_wait_data()`, but `ttsched_signal_ready()` returns `EACCES`.
- Timpani-N owns the SHM object. Normal TT/Successor Tasks require at least O_RDONLY, while Tasks that emit Data-Driven completion require O_RDWR. Phase 0 SHALL define the UID/GID/group or equivalent deployment ACL used to grant that distinction; unapproved world-write access SHALL NOT be treated as a production ABI guarantee. The current PoC `0666` mode may remain for regression tests only or be replaced with an equivalent deployment ACL.
- Public API signatures *(subject to prior approval, `OI-DD-02`)*:
  ```c
  int ttsched_wait_next_period(void); /* Existing TT API; unchanged */
  int ttsched_wait_data(void);        /* 0 only for a real dispatch wake */
  int ttsched_signal_ready(void);     /* Commit completion after publishing output */
  ```
- New APIs return `-1` with `errno`: `EACCES` (no write permission), `ENOTSUP` (old/missing/unsupported extension), `EINVAL` (invalid extension magic/size), `ESTALE` (publication/control wake/generation transition), `EAGAIN` (commit contention), and `ENOENT` (Task or Successor reference not found).
- In Phase 0, an app SHALL call `ttsched_signal_ready()` at most once per completed execution job, after publishing its output, and SHALL NOT call it again after a `0` result. A job-sequence parameter is deferred unless Phase 1 demonstrates a need for caller idempotency.

### 3.2 Stable-Generation Check, and Detailed `ttsched_signal_ready()` and `ttsched_wait_data()` Logic

```c
static __thread int g_shm_writable = 0;
static __thread int g_pipeline_supported = 0;
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
    if (magic != TIMPANI_TTSCHED_MAGIC || gen == 0U || (gen & 1U) != 0U) {
        errno = ESTALE;
        return -1;
    }
    *out_gen = gen;
    return 0;
}

static inline int read_stable_pipeline_gen(
    const volatile struct timpani_ttsched_shm* shm, uint32_t* out_gen)
{
    if (!g_pipeline_supported) {
        errno = ENOTSUP;
        return -1;
    }
    if (read_stable_gen(shm, out_gen) != 0)
        return -1;
    if (__atomic_load_n(&shm->pipeline_header.magic, __ATOMIC_ACQUIRE) !=
            TIMPANI_PIPELINE_MAGIC ||
        __atomic_load_n(&shm->pipeline_header.abi_version,
                        __ATOMIC_ACQUIRE) != TIMPANI_PIPELINE_ABI_VERSION ||
        __atomic_load_n(&shm->pipeline_header.abi_size,
                        __ATOMIC_ACQUIRE) != sizeof(*shm)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline uint32_t ready_sequence(uint64_t state)
{
    return (uint32_t)((state >> TIMPANI_READY_SEQUENCE_SHIFT) &
                      TIMPANI_READY_SEQUENCE_MASK);
}

static inline uint32_t ready_mask(uint64_t state)
{
    return (uint32_t)(state & TIMPANI_READY_MASK);
}

static inline uint64_t make_ready_state(uint32_t sequence, uint32_t mask)
{
    return ((uint64_t)sequence << TIMPANI_READY_SEQUENCE_SHIFT) |
           (uint64_t)(mask & TIMPANI_READY_MASK);
}

static inline int counter_ahead(uint32_t current, uint32_t previous)
{
    return (int32_t)(current - previous) > 0;
}

static inline int pipeline_commit_enter(
    const volatile struct timpani_ttsched_shm* shm, uint32_t* out_gen)
{
    uint32_t expected_gen;
    if (read_stable_pipeline_gen(shm, &expected_gen) != 0)
        return -1;

    uint32_t guard_before = __atomic_fetch_add(
        (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
        __ATOMIC_ACQ_REL);
    if ((guard_before & TIMPANI_PUB_WRITER_PENDING) != 0U) {
        __atomic_sub_fetch(
            (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
            __ATOMIC_ACQ_REL);
        errno = ESTALE;
        return -1;
    }
    if ((guard_before & TIMPANI_PUB_ACTIVE_MASK) ==
            TIMPANI_PUB_ACTIVE_MASK) {
        __atomic_sub_fetch(
            (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
            __ATOMIC_ACQ_REL);
        errno = EAGAIN;
        return -1;
    }

    uint32_t confirmed_gen;
    if (read_stable_pipeline_gen(shm, &confirmed_gen) != 0 ||
        confirmed_gen != expected_gen) {
        __atomic_sub_fetch(
            (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
            __ATOMIC_ACQ_REL);
        errno = ESTALE;
        return -1;
    }

    *out_gen = expected_gen;
    return 0;
}

static inline void pipeline_commit_exit(
    const volatile struct timpani_ttsched_shm* shm)
{
    uint32_t after = __atomic_sub_fetch(
        (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
        __ATOMIC_ACQ_REL);
    if ((after & TIMPANI_PUB_WRITER_PENDING) != 0U &&
        (after & TIMPANI_PUB_ACTIVE_MASK) == 0U) {
        syscall(SYS_futex,
                (uint32_t*)&shm->pipeline_header.publication_guard,
                FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

static inline int is_trigger_satisfied(
    uint32_t mask, uint32_t req_mask, uint16_t mode)
{
    uint32_t ready = mask & TIMPANI_READY_MASK;
    if (req_mask == 0U)
        return 0;
    if (mode == TIMPANI_TRIG_ALL)
        return (ready & req_mask) == req_mask;
    return 0; /* TIMPANI_TRIG_ANY is reserved and rejected in Phase 0. */
}

int ttsched_signal_ready(void)
{
    uint32_t gen;
    if (!g_shm) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_shm_writable) {
        errno = EACCES;
        return -1;
    }
    if (pipeline_commit_enter(g_shm, &gen) != 0)
        return -1;

    int result = -1;
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
        if (g_task_idx < 0) {
            errno = ENOENT;
            goto out;
        }
    }
    /* Recheck the slot identity (name) */
    if (strncmp((const char*)g_shm->tasks[g_task_idx].name,
               g_my_name, 15) != 0) {
        errno = EINVAL;
        goto out;
    }

    const volatile struct timpani_pipeline_slot* src =
        &g_shm->pipeline[g_task_idx];
    uint16_t n_edges =
        __atomic_load_n(&src->num_out_edges, __ATOMIC_ACQUIRE);
    if (n_edges > TIMPANI_MAX_OUT_EDGES)
        n_edges = TIMPANI_MAX_OUT_EDGES;

    struct timpani_out_edge local_edges[TIMPANI_MAX_OUT_EDGES];
    for (uint16_t i = 0; i < n_edges; i++) {
        local_edges[i].dst_slot_idx = src->out_edges[i].dst_slot_idx;
        local_edges[i].bit_idx      = src->out_edges[i].bit_idx;
    }

    for (uint16_t i = 0; i < n_edges; i++) {
        uint16_t dst_idx = local_edges[i].dst_slot_idx;
        uint16_t bit_idx = local_edges[i].bit_idx;
        if (dst_idx >= TIMPANI_MAX_TASKS || bit_idx >= 16) {
            errno = EINVAL;
            goto out;
        }

        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[dst_idx];
        uint32_t req  = __atomic_load_n(&dst->required_mask,
                                        __ATOMIC_ACQUIRE);
        uint16_t mode = __atomic_load_n(&dst->trigger_mode,
                                        __ATOMIC_ACQUIRE);
        if (mode != TIMPANI_TRIG_ALL) {
            errno = EINVAL;
            goto out;
        }

        uint32_t bit = (1U << bit_idx);
        int do_wake = 0;
        uint64_t old_state = __atomic_load_n(
            (const uint64_t*)&dst->ready_state, __ATOMIC_ACQUIRE);

        while (1) {
            uint32_t sequence = ready_sequence(old_state);
            uint32_t merged = ready_mask(old_state) | bit;
            uint64_t next_state = make_ready_state(sequence, merged);
            do_wake = 0;
            if (is_trigger_satisfied(merged, req, mode)) {
                next_state = make_ready_state(sequence + 1U, 0U);
                do_wake = 1;
            }
            if (__atomic_compare_exchange_n(
                    (uint64_t*)&dst->ready_state, &old_state, next_state,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }

        if (do_wake &&
            __atomic_load_n(&dst->execution_model, __ATOMIC_ACQUIRE) ==
                TIMPANI_EXEC_CBS) {
            __atomic_add_fetch((uint32_t*)&dst->dispatch_counter, 1U,
                               __ATOMIC_RELEASE);
            syscall(SYS_futex, (uint32_t*)&dst->dispatch_counter,
                    FUTEX_WAKE, 1, NULL, NULL, 0);
        }
        /* TT target: TimerMaster opens dispatch_counter in an allowed slot. */
    }
    result = 0;

out:
    {
        int saved_errno = result < 0 ? errno : 0;
        pipeline_commit_exit(g_shm);
        if (result < 0)
            errno = saved_errno;
    }
    return result;
}
```

- **`ttsched_wait_data()` consumer contract**:
  1. While holding the commit guard, verify the extension ABI, stable publication, and its own Task identity; synchronize the current `dispatch_counter`; and release the guard. Do not hold the guard while sleeping in `FUTEX_WAIT`.
  2. Return `0` only when `counter_ahead(current, previous)` is true. A dispatch decision, rather than ready itself, has then opened the layer-specific scheduler gate.
  3. Use `FUTEX_WAIT(&dispatch_counter, current)`. A simple wake return, EAGAIN, or EINTR is not success; reacquire the guard and recheck the counter and generation. A publication/shutdown control wake without a counter increment returns `ESTALE`.
  4. For an L2 CBS Task, the packed `ready_state` CAS and `dispatch_counter` increment/futex wake occur in the same commit guard. For an L1 TT Task, Timer Master acquires the guard, compares `ready_sequence(ready_state)` to `dispatch_counter` modulo wrap, and increments the latter in an allowed slot.

A successful `signal_ready` means that the commit interval was fully serialized with the publisher and, for `ALL`, mask reset plus ready-sequence increment completed in one 64-bit CAS. `pipeline_commit_enter()` does not wait; only the publisher waits, with a bounded limit, for active commits to drain.

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

1. The SHM publisher uses each TT/CBS entry's `(workload_id_hash, task_id_hash)` pair as the canonical key for the common `pipeline[]` index. Retain the Phase 0 unique-Task-name constraint for compatibility with SHM name lookup.
2. Initialize each pipeline slot's `workload_id_hash`, `task_id_hash`, `execution_model`, `required_mask`, and `out_edges`, and set `ready_state=0` and `dispatch_counter=0`.
3. Traverse predecessor keys in the common DAG metadata to populate `required_mask` and `out_edges`.
4. Register TT Tasks with the existing `TtSlot`/TimerMaster and CBS Tasks with the existing `CbsConfig`/CbsState.
5. Immediately before publication, set writer-pending in `pipeline_header.publication_guard` to prevent new Pipeline commits. Existing active commits must finish first; the publisher waits for the active count to reach `0` through bounded `FUTEX_WAIT` or bounded yield. When the last reader exits, it detects writer-pending and wakes the guard futex.
6. After the active count reaches `0`, publish in the order `magic=0`, odd `generation`, `tasks[]`/`pipeline_header`/`pipeline[]` updates, valid `pipeline_header.magic/version/abi_size`, even `generation`, valid base `magic`, and writer-pending release.
7. Publication itself does not increment ready/dispatch counters. If the active count does not reach `0` before the deadline, reject that publication boundary or defer it to the next one. Publication/shutdown control wake must not change `dispatch_counter`.

This ordering removes a concurrent interval in which `pipeline_commit_enter()` can start mutating state during initial publication or invalidation in Phase 0. Phase 1 reuses the same guard for normal updates of Pipeline-containing tables at an application boundary. Phase 2 adds reclamation and initialization of stale writer-pending/active-commit state after publication failure, rollback, termination, or restart.

### 4.3 RT TT/CBS Execution Adapters

- **L1 TT Source or normal TT:** If `required_mask == 0`, retain the existing `tasks[i].counter` slot-firing path. Do not use the Pipeline `dispatch_counter`.
- **L1 TT Successor:** At the slot time, Timer Master first updates `current_slot` and the next TT-start state, then acquires `pipeline_commit_enter()`. Only if `counter_ahead(ready_sequence(ready_state), dispatch_counter)` is true does it increment `dispatch_counter` by exactly 1 and perform a futex wake. If no pending ready exists or guard acquisition fails, it SHALL change neither `tasks[i].counter` nor `dispatch_counter`.
- **L2 CBS Source or normal CBS:** Retain the existing Event release path.
- **L2 CBS Successor:** Inside the `signal_ready` commit guard, finalize `ALL` with the packed `ready_state` CAS, increment `dispatch_counter` by 1, and perform a futex wake. After the thread becomes Runnable, BPF applies only the existing CBS CPU, budget, and Gap rules. OI-DD-03 does not add a separate CBS release API or Data-Driven DSQ.

If ready arrives after an L1 slot, Phase 0 retains it as pending until the next TT slot. Normal Phase 0 tests allow at most one pending ready after modulo-sequence comparison; two or more pending ready sequences are a backlog/missed-slot precondition violation. Pending expiration, deadline handling, `Activation Window`, and `SKIP` are deferred to Phase 1/2.

| Path | Owner | Allowed | Prohibited |
|:---|:---|:---|:---|
| **L1 Source** | Timer Master | Existing `tasks[i].counter` slot wake | Pipeline pending check |
| **L1 Successor adapter** | Timer Master | `dispatch_counter++` at its own TT slot | Before-slot/no-ready wake, `Window` enforcement |
| **L2 Successor adapter** | `signal_ready` | `dispatch_counter++` in the `ALL` commit | TT-slot wait, separate release daemon |
| **CBS CPU execution** | `scx_timpani` | Existing budget/Gap gate after Runnable | Data-Ready graph evaluation |
| **libttsched** | Application API | Atomic commit and dispatch futex | Consuming control wake as execution |
| **BPF** | Kernel scheduler | Existing TT/CBS DSQ, budget/slot rules | Separate Data-Driven DSQ |

### 4.4 Per-Layer Enforcement in `scx_timpani`

- An L1 TT Successor is not Runnable until the TT adapter opens the dispatch wake at its slot. After that wake, BPF uses the existing `SCHED_TYPE_TT` path.
- An L2 CBS Successor becomes Runnable from the `ALL` commit wake and enters the existing `SCHED_TYPE_CBS` path. `ALL` is only an additional release condition and does not alter CBS budget, TT-active, or Gap decisions.
- For both TT and CBS, existing Task-layer rules determine `DMISS`, budget, and trace behavior. Data-Ready is neither a scheduler fault nor a BPF dispatch queue.

---

## 5. L-DD-P0-04 — `Timpani-O` Candidate Validation and Deterministic Topological Placement (`global_scheduler.{h,cpp}`)

1. **Protect the committed model and resolve the Task class**: Construct and validate the candidate first. For each Task, use `TaskInfo.temporal_class` when present; otherwise inherit `SchedInfo.temporal_class`. Verify that `predecessors` or the Source/Successor role does not alter the resolved class. Retain the committed model on rejection.
2. **Normalize Task references and validate/sort the DAG deterministically (`ValidateAndSortDag`)**:
   - Use `TaskNameKey = std::pair<std::string, std::string>` `(workload_id, task_id)` as the internal identity. Use only `std::map` and lexicographically ordered `std::set` (`std::unordered_map` is prohibited).
   - If `TaskReference.workload_id` is empty, fill it with the enclosing `SchedInfo.workload_id`. Reject it with `InfeasibleReason::InvalidDag` in Phase 0 if the normalized result differs from the current workload.
   - Preserve node-wide uniqueness and the `1..15`-byte ASCII length limit for the Task name because Phase 0 retains the existing SHM name lookup. Reject duplicate normalized Task keys, self-reference, nonexistent references, and collisions in the `(fnv1a(workload_id), fnv1a(task_id))` hash pair.
   - Enforce a maximum of `16` immediate predecessors and `8` outgoing edges per vertex. After Kahn's Algorithm over a lexicographically ordered ready queue, `out_topo_rank.size() != task_index.size()` indicates a cycle and shall be rejected.
3. **Class-independent table construction (`build_table`)**:
   - `PipelineTaskConfig` carries no execution-layer field. It contains `PipelineTaskKey(workload_id_hash, task_id_hash)` and predecessor key values. Create TT/CBS entries separately according to the resolved class.
   - Create a `TtSlot` from the input timing for an L1 Task and a `CbsConfig` from the input timing for an L2 Task. Source/Successor does not determine this branch.
   - O uses the DAG topological order only as a tie-breaker for TT slot collision checks or CBS admission checks.
4. **Phase 0 placement and deferral**:
   - Support explicit/inherited class mixtures within the same workload, using existing TT placement and existing CBS admission/worst-fit.
   - Phase 1 adds cross-workload `TaskReference`, TT Window/CBS Gap and budget delay aggregation in a mixed DAG, `E2E` validation, and Pipeline updates. Do not include an `E2E` field in the Phase 0 OI-DD-01 protobuf.

---

## 6. Minimum Protobuf Interface Extension (`OI-DD-01`, Subject to Prior Approval)

> **Caution**: Update the actual `.proto` file only after explicit approval by the Project Leader (`CON-DD-05`).

```protobuf
// timpani-o/proto/schedinfo.proto (package schedinfo.v1)
message TaskReference {
  // Phase 0: empty means the enclosing SchedInfo.workload_id.
  string workload_id = 1;
  string task_id = 2;
}

message TaskInfo {
  // ... existing fields 1..10 ...
  // If absent, inherit SchedInfo.temporal_class.
  optional TemporalClass temporal_class = 11;
  repeated TaskReference predecessors = 12;
}

// timpani-o/proto/node_control.proto (package timpani.node.v1)
enum PipelineTriggerMode {
  PIPELINE_TRIGGER_ALL = 0;
}

message PipelineTaskKey {
  uint64 workload_id_hash = 1;
  uint64 task_id_hash = 2;
}

message PipelineTaskConfig {
  PipelineTaskKey task_key = 1;
  repeated PipelineTaskKey predecessor_keys = 2;
  PipelineTriggerMode trigger_mode = 3;
}

message HierarchicalScheduleTable {
  // ... existing fields 1..5 ...
  repeated PipelineTaskConfig pipeline_tasks = 6;
}
```

The recommended field numbers `11`, `12`, and `6` use numbers currently free in their messages and shall be finalized through interface approval before implementation. Retain `SchedInfo.temporal_class` as the existing workload default. O shall use presence detection for `TaskInfo.temporal_class`, so a plain proto enum is insufficient; `optional` is required. `PIPELINE_TRIGGER_ALL` is the only valid trigger enum value in Phase 0.

Do not duplicate dependencies in TT/CBS entries. Put O→N DAG metadata only in `PipelineTaskConfig`, and require N to connect each key pair to exactly one `TtSlot` or `CbsConfig`. Retain node-wide uniqueness of the existing `1..15`-byte Task name for SHM lookup in Phase 0.

---

## 7. L-DD-P0-05 — Test Integration Points

Unit tests shall verify that the DAG is processed without inferring an execution layer and that L1/L2 connections execute through the gate for each layer.

| Test | Verification |
|:---|:---|
| Class resolution | An explicit Task class takes precedence over the workload default, an omitted Task class inherits the default, and `predecessors` do not affect the class |
| Reference normalization/rejection | An empty `workload_id` resolves to the current workload; Phase 0 cross-workload references, duplicate keys, hash-pair collisions, and nonexistent references are rejected before activation |
| O DAG/placement | Normalized keys, topological order, and TT/CBS placement are input-order independent, and the execution class equals the explicit/inherited value |
| N registration | The `(workload_id_hash, task_id_hash)` pair connects to exactly one TT/CBS entry and the class is unchanged |
| SHM ABI | On a legacy 784-byte object, only the TT API works and Pipeline APIs return `ENOTSUP`; extension magic/version/size mismatch returns `EINVAL` and no `fstat()`-prohibited access occurs |
| Permissions | O_RDWR failure for a Pipeline writer returns `EACCES`; a read-only Successor may call `wait_data`, while `signal_ready` returns `EACCES` |
| Concurrency | The target reports the 64-bit `ready_state` CAS as lock-free; the final predecessor CAS atomically resets the mask and increments the sequence, and concurrent predecessors produce exactly one wake |
| Wake contract | Only a `dispatch_counter` increment is an execution wake; publication/shutdown control wake returns `ESTALE`; modulo comparison has no loss/duplication around `0x7fffffff` |
| L1 gate | `0` increment before `ALL` or without a slot; if ready is present before a slot, increment once at that slot; `0` duplicate wake for the same ready; a late ready remains pending for the next slot |
| L2 gate | Increment `dispatch_counter` and futex-wake exactly once immediately after the packed `ALL` CAS; actual CPU execution may wait for CBS budget/Gap |
| CBS BPF boundary | TT-active or budget=0 delays CPU dispatch without cancelling the release; existing throttled/replenishment paths remain responsible |
| Combinations | Mix Source/Successor roles and pass TT→TT, TT→CBS, CBS→TT, and CBS→CBS at least once |

Record hardware verification as not performed if target execution is not performed.
