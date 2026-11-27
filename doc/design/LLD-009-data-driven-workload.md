<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-009: Data-Driven Workload — Phase 0 Low-Level Design

**Date:** 2026-10-27
**Last Updated:** 2026-11-25 (Phase 0 PR review remediations)
**Status:** Draft — Phase 0 design proposal; review and approval pending.
The 008 field allocation `11/12/13` is aligned, but `OI-DD-01`/`02`/`03`
remain subject to Project Leader approval and verification; implementation is
not complete.
**Parent Baseline:** [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, [SRS-009](SRS-009-data-driven-workload.md), [HLD-009](HLD-009-data-driven-workload.md)
**Design Decisions:** [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01`..`05`, [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, `D-P0-06`

---

## 1. Design Boundary, Pre-Approval Scope, and Existing Integration Points

This document details only the common DAG data structures, ready/dispatch separation, TT/CBS execution adapters, prechecks, and test integration points for Phase 0. Phase 1 Window/mixed-`E2E` support and the Phase 2 `SKIP`/recovery state machine are excluded.

| Existing Integration Point | Phase 0 Detailed Design Intent | Approval / Compatibility Constraint |
|:---|:---|:---|
| `timpani-o/proto/*.proto` | An `optional` Task class override (`protoc >= 3.15`), structured `TaskReference` (`task_id` ↔ `TaskInfo.name`), and common DAG metadata for workload/task hash pairs (`ExecutionModel.DATA_FLOW = 2` is not used) | **`OI-DD-01` approval pending (008 allocation `11/12/13` aligned)** |
| `sample-apps/src/libttsched.h/c` | A versioned Pipeline extension (`40B` header), packed `ready_state`, a dispatch-only futex, preservation of `void ttsched_init()`, `g_shm_init_errno`, bulk TLS reset and attachment-state separation before/after publication (`g_attached_before_publish`), `release_source`/`current_job_token`/`wait_data`/`signal_ready`, `pipeline_commit_alive`, `atomic_name_equals`, a read-only seqlock, and VFS-free RT validation | **`OI-DD-02` conditional-approval proposal pending** |
| `timpani-o/src/schedinfo_service.{h,cpp}` | Explicit-over-inherit resolution of `TaskInfo.temporal_class` and safe rollback to the existing workload model when `RegenerateAllSchedules()` fails | Preserve the existing API |
| `timpani-o/src/global_scheduler.*` | DAG validation, the CBS-only `hyperperiod_us > 0` guarantee, L1→L1 ordering in `place_tt_slots()` through `(topo_rank, deadline_us, ...)` + `min_start`, and separate graph metadata generation | Preserve the existing API |
| `timpani-n/src/main.cpp` | Local validation and registration of TT/CBS entries with common graph metadata, rejection of `ExecutionModel.DATA_FLOW` (`EINVAL`), and transfer of CBS/Pipeline metadata through a `TimerMaster::set_schedule_table()` overload | No per-layer feasibility recalculation |
| `timpani-n/src/timer_master.*` | A `set_schedule_table()` overload (preserving `CON-DD-06`), one `base_cycle_start` in `start()`, a Source-specific pipeline-cycle ordinal, and, in an allowed L1 Successor slot, updating `next_tt_start` and then performing `update_current_slot()`·`dispatch_counter++`·`FUTEX_WAKE` only when `tm_pipeline_commit_enter` detects `pending` (omit these when `pending == false`; do not overwrite `SLOT_NONE`) | No L2 release |
| `timpani-n/src/bpf/timpani.bpf.c` | Reuse the existing `SCHED_TYPE_TT`/`CBS` dispatch gates without change | No separate DSQ |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` Shared-Memory Layout (`libttsched.h`)

Under [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` and [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, preserve the existing header and `tasks[TIMPANI_MAX_TASKS]` (offset `0..783`) byte for byte. Append a versioned Pipeline extension header and the `pipeline[TIMPANI_MAX_TASKS]` region after that prefix.

```c
#define TIMPANI_TTSCHED_NAME        "/timpani_ttsched"
#define TIMPANI_TTSCHED_PREFIX_SIZE 784U
#define TIMPANI_TTSCHED_MAGIC       0x54494D50U /* "TIMP" */
#define TIMPANI_PIPELINE_MAGIC      0x50495045U /* "PIPE" */
#define TIMPANI_PIPELINE_ABI_VERSION 1U
#define TIMPANI_MAX_TASKS           32
#define TIMPANI_MAX_OUT_EDGES       8

#define TIMPANI_PUB_WRITER_PENDING    0x80000000U
#define TIMPANI_PUB_ACTIVE_MASK       0x7FFFFFFFU
#define TIMPANI_PUB_DRAIN_TIMEOUT_US  5000U /* Normal publication/update drain limit: effective_pub_drain_timeout_us(H) = min(5 ms, H/4) */
#define TIMPANI_PUB_MAX_DEFERS        2U    /* Phase 1+ limit for consecutive drain-timeout deferrals */
#define TIMPANI_PUB_SHUTDOWN_DRAIN_MS 50U   /* Forced-invalidation limit during shutdown/replacement (Safe Abandonment) */

#define TIMPANI_TRIG_ALL 0U /* Sole valid trigger in Phase 0 */
#define TIMPANI_TRIG_ANY 1U /* Reserved extension value for Phase 1+; publication is prohibited in Phase 0 */

#define TIMPANI_READY_MASK         0x0000FFFFULL
#define TIMPANI_SOURCE_RELEASE_VALID 0x00010000ULL
#define TIMPANI_READY_SEQUENCE_SHIFT 32
#define TIMPANI_READY_SEQUENCE_MASK  0xFFFFFFFFULL

/* Existing slot structure — size (24B) and field offsets remain unchanged */
struct timpani_task_slot {
    char     name[16]; /* Task comm name (PR_SET_NAME, <=15 characters + NUL) */
    uint32_t counter;  /* Timer Master periodic slot firing counter */
    uint32_t _pad;
};

struct timpani_out_edge {
    uint16_t dst_slot_idx; /* SHM index of the Successor Task (0..TIMPANI_MAX_TASKS-1) */
    uint16_t bit_idx;      /* Bit position to set in the Successor ready_mask (0..15) */
};

#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    /* Timpani-N records and retains this for TtSlot/CbsConfig join validation and diagnostics.
     * Phase 0 libttsched looks up the slot by the node-unique 1..15B ASCII tasks[i].name. */
    uint64_t workload_id_hash;
    uint64_t task_id_hash;
    uint64_t ready_state;
    /* Successor: [63:32] ready sequence, [31:16] reserved, [15:0] ready mask.
     * Source: [63:32] released activation ordinal, bit 16 = release-valid,
     * [15:0] = 0. */
    uint32_t required_mask;    /* Required predecessor bits (0 for a Source/ordinary Task) */
    uint32_t dispatch_counter; /* Execution count permitted by a TT slot or CBS release (the execution permission); sole futex word */
    uint16_t num_out_edges;
    uint16_t trigger_mode;     /* Phase 0: TIMPANI_TRIG_ALL */
    uint16_t execution_model;  /* TT/CBS; independent of the Source/Successor role */
    uint16_t reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

/* Backward-compatible extension header (40B) — placed at byte 784, immediately after tasks[] */
struct timpani_pipeline_header {
    uint32_t magic;              /* TIMPANI_PIPELINE_MAGIC (0 during invalidation/shutdown) */
    uint16_t abi_version;        /* TIMPANI_PIPELINE_ABI_VERSION */
    uint16_t reserved;
    uint32_t abi_size;           /* Total struct timpani_ttsched_shm size (3128B) */
    uint32_t publication_guard;  /* bit31=writer pending; lower 31 bits=active write-commit count */
    uint64_t instance_id;        /* Unique SHM-object identifier (0 is prohibited; issue a new value on replacement/regeneration) */
    uint64_t origin_epoch_ns;    /* Publisher-recorded diagnostic/Phase 1 epoch metadata (not loaded on the RT fast path or used as the job_token source) */
    uint64_t hyperperiod_us;     /* Publisher-validated (> 0) diagnostic/Phase 1 fixed-schedule hyperperiod (us; not loaded on the RT fast path) */
};

struct timpani_ttsched_shm {
    uint32_t magic;      /* TIMPANI_TTSCHED_MAGIC after stable publication, 0 during publication/shutdown */
    uint32_t n_tasks;    /* Highest occupied slot index + 1 */
    uint32_t generation; /* Odd = publication in progress; even (>0) = stable publication state */
    uint32_t _pad;
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
    struct timpani_pipeline_header pipeline_header;
    struct timpani_pipeline_slot pipeline[TIMPANI_MAX_TASKS];
};

_Static_assert(offsetof(struct timpani_ttsched_shm, pipeline_header) ==
                   TIMPANI_TTSCHED_PREFIX_SIZE,
               "preserve the existing 784-byte SHM prefix");
_Static_assert(sizeof(struct timpani_pipeline_header) == 40,
               "keep the pipeline slots 8-byte aligned");
_Static_assert(sizeof(struct timpani_pipeline_slot) % sizeof(uint64_t) == 0,
               "pipeline slot must be 64-bit aligned");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
               "64-bit ready_state atomic access must be lock-free");
```

A new client checks the SHM size with `fstat()` only on the non-RT initialization path in `void ttsched_init(void)`. The existing return type (`void`) is retained, but the thread-local `g_shm_init_errno` is recorded. If the file is shorter than the existing 784-byte prefix, initialization records the failure as `g_shm_init_errno = EINVAL`; subsequent Pipeline APIs return that same `EINVAL`, and the TT path is not entered. If the prefix is valid but the full extension (`sizeof(struct timpani_ttsched_shm)`) is absent, `g_shm_pipeline_capable = 0` is set so that only the existing `ttsched_wait_next_period()` remains usable. Thereafter, the RT critical path (`release_source`, `signal_ready`, `wait_data`, `current_job_token`) never invokes VFS/fd system calls such as `shm_open`/`fstat`/`close`; it validates the ABI, generation, identity, replacement, and invalidation solely through userspace atomic loads (`g_shm_pipeline_capable`, `shm->magic`, even `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`, and `publication_guard`) and a 16-byte atomic name comparison (`atomic_name_equals`). `origin_epoch_ns` and `hyperperiod_us` are diagnostic/Phase 1 metadata that the publisher (`Timpani-N`) records in the 40B header after validating `hyperperiod_us > 0` before publication; the RT fast path performs no unnecessary additional loads of them.

The live SHM object has a fixed size for its lifetime. Before publishing a full-layout mapping, `Timpani-N` creates it at the full ABI size (`3,128`B) and SHALL NOT `ftruncate()` it below that size while clients can map it (the no-shrink invariant). During replacement or shutdown, it sets `WRITER_PENDING`, drains active commits (up to `TIMPANI_PUB_SHUTDOWN_DRAIN_MS = 50 ms`), invalidates the object with `shm->magic = 0` and `pipeline_header.magic = 0`, invokes `FUTEX_WAKE(..., INT_MAX)` for both `tasks[i].counter` and `pipeline[i].dispatch_counter` in every slot (`0..TIMPANI_MAX_TASKS-1`), and then unlinks the old object with `shm_unlink()` without `ftruncate()` or an in-place overwrite. A new object is created with `O_CREAT | O_EXCL` and a new `instance_id`. A client retaining the old mapping immediately detects `magic == 0`, `WRITER_PENDING`, or an `instance_id` mismatch through atomic loads on its next RT call, returns `ESTALE`, and reconnects to the new object by invoking `ttsched_init()` again on the non-RT control path.

The `publication_guard` in the extension header closes the TOCTOU window between a write commit (`ttsched_release_source()`, `ttsched_signal_ready()`, and the `Timer Master` L1 adapter) and a publication-generation transition. The publisher atomically sets `TIMPANI_PUB_WRITER_PENDING` to block new write commits and waits only for a bounded interval (`effective_pub_drain_timeout_us(H) = min(5 ms, H/4)`, or `50 ms` for shutdown/replacement) until the existing active commits reach `0`; it then updates the SHM with `magic = 0` and an odd `generation`. An in-flight writer that has already entered the guard checks `pipeline_commit_alive()` immediately before each CAS/wake while excluding `WRITER_PENDING`, so it can complete commit+wake during a normal drain and then terminate the drain normally through `pipeline_commit_exit()`. In contrast, pure-read consumer APIs that do not write the SHM (`ttsched_wait_data()`, `ttsched_current_job_token()`) use a **read-only seqlock** path that does not write `publication_guard`, allowing them to operate safely on `O_RDONLY` (`PROT_READ`) mappings without `SIGSEGV` or a guard leak caused by a consumer crash.

---

## 3. L-DD-P0-02 — `libttsched` Synchronization and Stable-Publication Checks (`libttsched.c`)

### 3.1 SHM Mapping, Deployment Permission Model, and Public API Contract

- Preserve the existing public APIs: retain the `void ttsched_init(void)` signature and the `O_RDONLY` fallback for the existing TT prefix, and do not change the signature or meaning of `ttsched_wait_next_period()` (however, the remapping/shutdown `munmap` size inside `ttsched_wait_next_period()` SHALL use `g_shm_map_size` rather than the fixed `sizeof(struct timpani_ttsched_shm)` and SHALL also update `g_shm_mapping = {NULL, 0}` to prevent a double `munmap` for the legacy 784B mapping and the thread destructor).
- At the start of `void ttsched_init(void)`, if `g_shm_mapping.addr != NULL` from an existing mapping, call `munmap(g_shm_mapping.addr, g_shm_mapping.size)`, then **bulk-reset the entire TLS cache** (`g_shm`, `g_shm_mapping`, `g_shm_map_size`, `g_shm_pipeline_capable`, `g_shm_writable`, `g_shm_instance_id`, `g_shm_generation`, `g_task_idx`, `g_last_dispatch_counter`, `g_dispatch_counter_synced`, `g_attached_before_publish`, and `g_shm_init_errno`). The non-RT path then calls `shm_open()` → `fstat()` once to verify the actual object size. If `st_size < 784`, record the failure with `close(fd); g_shm_init_errno = EINVAL; errno = EINVAL; return;` (guaranteeing `g_shm_mapping.addr == NULL`). If `st_size >= 784`, map it with `mmap()` and **immediately `close()` the SHM file descriptor after `mmap()`**. Set `g_shm_pipeline_capable = 1` when the mapping size is at least `sizeof(struct timpani_ttsched_shm)`, and `g_shm_pipeline_capable = 0` for the old 784B object or a shorter mapping.
- A Pipeline-capable `ttsched_init()` distinguishes the initial attach cursor according to the SHM state when called:
  - On successful `pipeline_read_dispatch_snapshot()` (stable publication already exists and the caller's slot is present), snapshot `g_shm_generation = gen`, `g_shm_instance_id = inst`, `g_task_idx = idx`, `g_last_dispatch_counter = cur`, `g_dispatch_counter_synced = 1`, and `g_attached_before_publish = 0` (`cur == 0` preserves token `0`; a late attachment at `cur = N > 0` prevents replay of `0..N-1`).
  - If the SHM is not yet published (`magic != TIMP` or an odd/0 `generation`) or the caller's slot does not yet exist (the actual `Timpani-N` startup ordering of a preceding `/proc` lookup → SHM publication), set `g_shm_generation = 0U`, `g_last_dispatch_counter = 0U`, `g_dispatch_counter_synced = 0`, and **`g_attached_before_publish = 1`** so that entry into the first valid generation starts at cursor `0`, preventing both token `0` loss and a multi-hop `ESTALE` deadlock.
- **Resource release at thread termination**: Connect the thread-local mapping state (`g_shm`, `g_shm_map_size`) to a `pthread_key_create()` destructor registered once per process with `pthread_once`, so worker-thread termination automatically calls `munmap(g_shm, g_shm_map_size)` and prevents VMA/fd leaks from repeated thread creation and termination.
- The RT paths (`ttsched_release_source()`, `ttsched_signal_ready()`, `ttsched_wait_data()`, and `ttsched_current_job_token()`) SHALL NOT call `shm_open`/`fstat`/`close`. Their entry guards immediately return `errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP` for `!g_shm`, or `errno = ENOTSUP` for `!g_shm_pipeline_capable`, precisely distinguishing an initialization error for `< 784B` (`EINVAL`) from a legacy 784B mapping (`ENOTSUP`). They then validate `WRITER_PENDING` in `publication_guard`, `shm->magic`, an even `generation`, and `pipeline_header.{magic, abi_version, abi_size, instance_id}` through pure atomic loads and the 16-byte atomic name comparison (`atomic_name_equals`).
- A failed Pipeline call SHALL NOT silently remap/reopen on the RT path or implicitly adopt a new generation different from the bound generation (`g_shm_generation != 0U`). A caller receiving `ESTALE` calls `ttsched_init()` again on its non-RT control path. Reinitialization unmaps the old mapping, resets all TLS, and reopens the current `/timpani_ttsched` object through `shm_open()`/`fstat()`/`mmap()`/`close()` to snapshot the new generation and identity and the current `dispatch_counter` as its attach cursor. Old tokens are not replayed. Repeated calls before reinitialization safely return `ESTALE`.
- **Read-only (`O_RDONLY`) versus write (`O_RDWR`) permissions and the deployment model**:
  - A writer that records a Data-Driven completion signal (a Source, an L2 Source event adapter/test generator, or an intermediate Successor) maps with `O_RDWR` + `PROT_READ|PROT_WRITE`; if write permission is unavailable, it falls back to `O_RDONLY` with `g_shm_writable = 0`.
  - A read-only consumer (an ordinary TT Task or a Sink/read-only Successor) maps with `O_RDONLY` + `PROT_READ` and can normally call `ttsched_wait_data()` and `ttsched_current_job_token()` (using the read-only seqlock without writing `publication_guard`), while calls to `ttsched_signal_ready()` or `ttsched_release_source()` return `EACCES`.
  - Only `Timpani-N` owns the SHM create/`ftruncate()`/`shm_unlink()` lifecycle. The standard permission model for production deployment of `/dev/shm/timpani_ttsched` is **owner UID `timpani-n` (`rw-`) plus POSIX ACL/dedicated-group separation (`g:timpani-pub:rw-` for trusted writer containers, `g:timpani-sub:r--` for ordinary TT and read-only Successor/Sink containers, and other `---`)**. In a lightweight environment without ACL support, apply equivalent isolation using `0660` + the `timpani-pub` group and container mounts/read-only binds. The existing PoC mode `0666` is restricted to local regression testing and is not considered a production ABI contract.
  - **Phase 0 trust boundary**: Because `O_RDWR`/`PROT_WRITE` maps the entire SHM, members of the `timpani-pub` group can also modify the header and other Task slots at the OS level. This group MUST contain only mutually trusted cooperative writers. The self-slot/generation checks in `libttsched` detect accidental misuse and corruption; they do not isolate a malicious writer or guarantee memory integrity. Untrusted workloads MUST use the read-only path in `timpani-sub`. Per-slot capabilities, per-slot SHM, and strong isolation through a kernel mediator are outside Phase 0.
- Public API signatures *(conditional-approval proposal pending, `OI-DD-02`)*:
  ```c
  void ttsched_init(void);                            /* Preserve existing signature (failure cause retained in g_shm_init_errno) */
  int ttsched_wait_next_period(void);                 /* Existing TT API, unchanged */
  int ttsched_release_source(const char *task_name,
                             uint32_t job_token);     /* Stamp exclusively for an L2 CBS Source event adapter/generator (O_RDWR required) */
  int ttsched_current_job_token(uint32_t *job_token); /* Acquire the common L1/L2 Source activation token (O_RDONLY allowed) */
  int ttsched_wait_data(uint32_t *job_token);         /* Return 0 only for a real dispatch wake; consume FIFO-sequentially (O_RDONLY allowed) */
  int ttsched_signal_ready(uint32_t job_token);       /* Commit completion of the same job (O_RDWR required) */
  ```
- New APIs return `-1` with `errno` on failure: `EACCES` (no write
  permission); `ENOTSUP` (a legacy/short mapping or an unsupported ABI
  version with valid extension magic); `EINVAL` (a NULL/invalid argument, a
  call after initialization failed for `< 784B`, invalid extension magic/ABI
  size, a caller-name/slot identity mismatch, a `release_source` misuse
  against the Source role/release state/L1 target, or invalid outgoing-edge
  metadata); `ESTALE` (termination/replacement causing
  `magic=0`/`WRITER_PENDING`/an `instance_id` change, a publication/control
  wake, a bound-generation mismatch, or a job token different from the
  Successor's active ready sequence); `EALREADY` (the same predecessor bit
  was already committed for the active job token); `EAGAIN` (commit
  contention or the Source release has not yet been stamped); and `ENOENT`
  (the caller's own Task slot is absent or the Task slot targeted by
  `ttsched_release_source()` is absent).
- **Calling convention for `ttsched_signal_ready()` from a Sink Task**: If a Sink Task with `num_out_edges == 0` conventionally calls `ttsched_signal_ready(job_token)` immediately after completion, return `0` (a safe no-op) after normally validating write permission, SHM ABI/generation, `n_tasks`, and the caller's own slot identity (`atomic_name_equals(tasks[g_task_idx].name, g_my_name)`), without changing any outgoing edge. A Sink mapped as `O_RDONLY`, however, SHALL NOT call `signal_ready()`; if it does, return `EACCES`.
- Before the first `ready_state` modification, `ttsched_signal_ready()` snapshots and validates the entire outgoing-edge list in a bounded stack plan. Validation covers `n_tasks`, `num_out_edges`, `dst_slot_idx`, self/duplicate destinations, `bit_idx`, validity and membership of the lower 16 bits of `required_mask`, `trigger_mode`, `execution_model`, and each destination's token/duplicate-bit state. A failure in any edge returns `-1` with no predecessor mask, dispatch counter, or futex wake changed; the CBS dispatch counter and wake are performed only after all edge CAS operations have completed or have been completed by an identical concurrent call.
- **Common `job_token` domain and calling-time convention**: In Phase 0, `job_token` is the 32-bit logical activation ordinal corresponding to the current `ready_sequence`; it is neither a timestamp nor a Task-local call count. An L1 Source is stamped with a Source-specific activation ordinal `k` (`0, 1, 2, ...`, `+1` per slot firing) for its pipeline cycle after `Timer Master` establishes the single `base_cycle_start` shared by all CPU threads at `start()`. For an L2 CBS Source, an event adapter/test generator with `O_RDWR` (`timpani-pub`) permission stamps `k` through `ttsched_release_source(task_name, k)` (validated as `required_mask == 0 && num_out_edges > 0 && execution_model == TIMPANI_EXEC_CBS`) and then wakes the Task through the existing event path. **Immediately after activation**, each L1/L2 Source in the same fixed-period `H` Pipeline calls `ttsched_current_job_token(&token)` once to read this release snapshot and, after processing completes, passes it to `ttsched_signal_ready(token)` (assuming `wake_delay < Source_period`). The token SHALL NOT be recomputed from `origin_epoch_ns`, `hyperperiod_us`, `CLOCK_REALTIME`, or an app-local counter; calling `current_job_token()` without a release marker returns `EAGAIN` without changing state. A Successor passes the `token` returned in FIFO order by `ttsched_wait_data(&token)` unchanged to the next `ttsched_signal_ready(token)`. Harmonic period-to-token mapping is defined in Phase 1.
- **Application-owned payload contract**:
  - 009/libttsched does not store, copy, or parse the payload and does not own the payload location, data structure, or producer/consumer ownership-acknowledgement API.
  - The Producer SHALL write all final content to the payload slot corresponding to `job_token` and complete any required application memory barriers before calling `ttsched_signal_ready(job_token)`.
  - The release/acquire ordering of the Source/Successor ready CAS and `dispatch_counter` forms a visibility edge for the subsequent `wait_data(token)` or TT/CBS dispatch consumption. The Consumer SHALL NOT read the payload before acquiring the token.
  - The Producer SHALL NOT overwrite the same payload slot until an application-owned acknowledgement/ownership mechanism confirms that every immediate Successor that accepted the token has completed processing it. The Phase 0 PoC uses at least 2 token-addressed slots to reflect the `pending <= 1` assumption.
  - Rejection by `signal_ready()` (`EALREADY`, `ESTALE`, `EINVAL`, and so on) does not approve the new payload. The Producer SHALL isolate the rejected payload so that it cannot overwrite the approved payload for the active token.
  - 009 does not directly validate payload contents, generation agreement, or overwrites. Violation of this application contract is a departure from the Phase 0 test assumptions and a case in which payload correctness is not guaranteed.

### 3.2 Stable-Generation Checks and Detailed Logic for `ttsched_release_source()`, `ttsched_current_job_token()`, `ttsched_signal_ready()`, and `ttsched_wait_data()`

```c
/* Destructor that automatically releases the thread-local SHM mapping at thread termination */
struct ttsched_tls_mapping {
    void*  addr;
    size_t size;
};

static pthread_once_t g_tls_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t  g_tls_unmap_key;

static void ttsched_tls_unmap_dtor(void* arg)
{
    struct ttsched_tls_mapping* m = (struct ttsched_tls_mapping*)arg;
    if (m && m->addr && m->size > 0) {
        munmap(m->addr, m->size);
        m->addr = NULL;
        m->size = 0;
    }
}

static void ttsched_tls_key_init(void)
{
    pthread_key_create(&g_tls_unmap_key, ttsched_tls_unmap_dtor);
}

static __thread volatile struct timpani_ttsched_shm* g_shm = NULL;
static __thread struct ttsched_tls_mapping g_shm_mapping = { NULL, 0 };
static __thread size_t   g_shm_map_size = 0;
static __thread int      g_shm_pipeline_capable = 0;
static __thread int      g_shm_writable = 0;
static __thread uint64_t g_shm_instance_id = 0;
static __thread uint32_t g_shm_generation = 0;
static __thread int      g_task_idx = -1;
static __thread char     g_my_name[16] = {};
static __thread int      g_name_cached = 0;
static __thread uint32_t g_last_dispatch_counter = 0;
static __thread int      g_dispatch_counter_synced = 0;
static __thread int      g_attached_before_publish = 0;
static __thread int      g_shm_init_errno = 0;

/* Note: At entry, void ttsched_init(void) checks whether
 * g_shm_mapping.addr != NULL; if so, it calls
 * munmap(g_shm_mapping.addr, g_shm_mapping.size) and then bulk-resets all TLS
 * (g_shm, g_shm_mapping, g_shm_map_size, g_shm_pipeline_capable,
 * g_shm_writable, g_shm_instance_id, g_shm_generation, g_task_idx,
 * g_last_dispatch_counter, g_dispatch_counter_synced,
 * g_attached_before_publish, g_shm_init_errno).
 * On the non-RT path, shm_open() -> fstat() validates the size once.
 * If st_size < TIMPANI_TTSCHED_PREFIX_SIZE, perform close(fd);
 * g_shm_init_errno = EINVAL; errno = EINVAL; return;
 * (guaranteeing g_shm_mapping.addr == NULL to prevent a double munmap).
 * On success, close the fd immediately after mmap() and register thread-
 * termination release through pthread_once/pthread_setspecific.
 * If g_shm_map_size >= sizeof(struct timpani_ttsched_shm), set
 * g_shm_pipeline_capable = 1 and:
 *  - on pipeline_read_dispatch_snapshot() success (stable publication already
 *    exists and the slot is present):
 *    g_shm_generation = gen; g_shm_instance_id = inst; g_task_idx = idx;
 *    g_last_dispatch_counter = cur; g_dispatch_counter_synced = 1;
 *    g_attached_before_publish = 0;
 *  - when the SHM is unpublished (magic != TIMP, odd/0 generation) or the slot
 *    is absent (the startup order has a preceding /proc lookup):
 *    g_shm_generation = 0U; g_last_dispatch_counter = 0U;
 *    g_dispatch_counter_synced = 0; g_attached_before_publish = 1;
 * The existing ttsched_wait_next_period() also uses g_shm_map_size for
 * munmap and updates g_shm_mapping = { NULL, 0 }. */

/* Check stable publication with acquire ordering (valid magic + nonzero even generation) */
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

/* VFS-free pure atomic validation on the RT path: check ABI, generation, WRITER_PENDING, and instance_id */
static inline int read_stable_pipeline_gen(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_instance_id)
{
    if (!shm) {
        errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP;
        return -1;
    }
    if (!g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    uint32_t guard = __atomic_load_n(
        &shm->pipeline_header.publication_guard, __ATOMIC_ACQUIRE);
    if ((guard & TIMPANI_PUB_WRITER_PENDING) != 0U) {
        errno = ESTALE;
        return -1;
    }
    if (read_stable_gen(shm, out_gen) != 0)
        return -1;

    uint32_t pmagic = __atomic_load_n(
        &shm->pipeline_header.magic, __ATOMIC_ACQUIRE);
    if (pmagic == 0U) {
        errno = ESTALE;
        return -1;
    }
    if (pmagic != TIMPANI_PIPELINE_MAGIC) {
        errno = EINVAL;
        return -1;
    }
    if (__atomic_load_n(&shm->pipeline_header.abi_version,
                        __ATOMIC_ACQUIRE) != TIMPANI_PIPELINE_ABI_VERSION) {
        errno = ENOTSUP;
        return -1;
    }
    if (__atomic_load_n(&shm->pipeline_header.abi_size,
                        __ATOMIC_ACQUIRE) != sizeof(*shm)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t inst_id = __atomic_load_n(
        &shm->pipeline_header.instance_id, __ATOMIC_ACQUIRE);
    if (inst_id == 0ULL ||
        (g_shm_instance_id != 0ULL && inst_id != g_shm_instance_id)) {
        errno = ESTALE;
        return -1;
    }
    if (out_instance_id)
        *out_instance_id = inst_id;
    return 0;
}

/* Liveness check exclusively for an in-flight writer that already holds publication_guard.
 * Complete commit+wake during a normal drain (WRITER_PENDING=1), and stop only
 * for Safe Abandonment (magic=0 / pipeline_header.magic=0) or a generation/
 * instance_id change. */
static inline int pipeline_commit_alive(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t expected_gen, uint64_t expected_inst)
{
    uint32_t magic  = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    uint32_t gen    = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    uint32_t pmagic = __atomic_load_n(
        &shm->pipeline_header.magic, __ATOMIC_ACQUIRE);
    uint64_t inst   = __atomic_load_n(
        &shm->pipeline_header.instance_id, __ATOMIC_ACQUIRE);
    if (magic != TIMPANI_TTSCHED_MAGIC ||
        pmagic != TIMPANI_PIPELINE_MAGIC ||
        gen != expected_gen || inst != expected_inst) {
        errno = ESTALE;
        return -1;
    }
    return 0;
}

/* Exact, 16-byte NUL-padded atomic name comparison for RT paths. */
static inline int atomic_name_equals(const volatile char slot_name[16],
                                     const char* expected)
{
    char actual[16];
    char wanted[16] = {};
    size_t expected_len = 0U;

    for (size_t i = 0; i < 16; i++) {
        actual[i] = __atomic_load_n(&slot_name[i], __ATOMIC_RELAXED);
    }
    if (!expected)
        return 0;
    while (expected_len < 16U && expected[expected_len] != '\0') {
        expected_len++;
    }
    if (expected_len == 0U || expected_len >= 16U)
        return 0;
    memcpy(wanted, expected, expected_len);
    return memcmp(actual, wanted, sizeof(actual)) == 0;
}

static inline void atomic_name_store(volatile char slot_name[16],
                                     const char* src)
{
    char buf[16] = {};
    if (src) {
        strncpy(buf, src, 15);
    }
    for (size_t i = 0; i < 16; i++) {
        __atomic_store_n(&slot_name[i], buf[i], __ATOMIC_RELAXED);
    }
}

static inline int find_task_slot(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t n_tasks, const char* name)
{
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS)
        return -1;
    for (uint32_t i = 0; i < n_tasks; i++) {
        if (atomic_name_equals(shm->tasks[i].name, name))
            return (int)i;
    }
    return -1;
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

static inline uint64_t make_source_release_state(uint32_t sequence)
{
    return ((uint64_t)sequence << TIMPANI_READY_SEQUENCE_SHIFT) |
           TIMPANI_SOURCE_RELEASE_VALID;
}

static inline int counter_ahead(uint32_t current, uint32_t previous)
{
    return (int32_t)(current - previous) > 0;
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

/* Commit-guard entry for the write path (ttsched_signal_ready only; O_RDWR required) */
static inline int pipeline_commit_enter(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_inst)
{
    uint32_t expected_gen;
    uint64_t expected_inst;
    if (read_stable_pipeline_gen(shm, &expected_gen, &expected_inst) != 0)
        return -1;

    uint32_t guard_before = __atomic_fetch_add(
        (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
        __ATOMIC_ACQ_REL);
    if ((guard_before & TIMPANI_PUB_WRITER_PENDING) != 0U) {
        pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
        errno = ESTALE;
        return -1;
    }
    if ((guard_before & TIMPANI_PUB_ACTIVE_MASK) ==
            TIMPANI_PUB_ACTIVE_MASK) {
        pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
        errno = EAGAIN;
        return -1;
    }

    uint32_t confirmed_gen;
    uint64_t confirmed_inst;
    if (read_stable_pipeline_gen(shm, &confirmed_gen, &confirmed_inst) != 0) {
        int saved_errno = errno;
        pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
        errno = saved_errno;
        return -1;
    }
    if (confirmed_gen != expected_gen || confirmed_inst != expected_inst) {
        pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
        errno = ESTALE;
        return -1;
    }

    *out_gen = expected_gen;
    *out_inst = expected_inst;
    return 0;
}

static inline int is_trigger_satisfied(
    uint32_t mask, uint32_t req_mask, uint16_t mode)
{
    uint32_t ready = mask & TIMPANI_READY_MASK;
    if (req_mask == 0U || (req_mask & ~TIMPANI_READY_MASK) != 0U)
        return 0;
    if (mode == TIMPANI_TRIG_ALL)
        return (ready & req_mask) == req_mask;
    return 0; /* TIMPANI_TRIG_ANY is reserved and rejected in Phase 0. */
}

/* Stamp exclusively for an L2 CBS Pipeline Source event adapter/test generator (O_RDWR required).
 * Record the activation ordinal in the Source-slot ready_state immediately before the existing event wake. */
int ttsched_release_source(const char* task_name, uint32_t job_token)
{
    if (!task_name || task_name[0] == '\0' || strnlen(task_name, 16) > 15U) {
        errno = EINVAL;
        return -1;
    }
    if (!g_shm) {
        errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP;
        return -1;
    }
    if (!g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_shm_writable) {
        errno = EACCES;
        return -1;
    }

    uint32_t gen = 0U;
    uint64_t inst = 0ULL;
    if (pipeline_commit_enter(g_shm, &gen, &inst) != 0)
        return -1;

    int result = -1;
    if (g_shm_instance_id == 0ULL)
        g_shm_instance_id = inst;
    if (g_shm_generation == 0U) {
        g_shm_generation = gen;
        g_task_idx = -1;
    } else if (gen != g_shm_generation) {
        errno = ESTALE;
        goto out;
    }

    uint32_t n_tasks = __atomic_load_n(&g_shm->n_tasks, __ATOMIC_ACQUIRE);
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        errno = EINVAL;
        goto out;
    }

    int idx = find_task_slot(g_shm, n_tasks, task_name);
    if (idx < 0) {
        errno = ENOENT;
        goto out;
    }

    volatile struct timpani_pipeline_slot* src = &g_shm->pipeline[idx];
    uint32_t required_mask = __atomic_load_n(&src->required_mask, __ATOMIC_ACQUIRE);
    uint16_t num_out_edges = __atomic_load_n(&src->num_out_edges, __ATOMIC_ACQUIRE);
    uint16_t exec_model    = __atomic_load_n(&src->execution_model, __ATOMIC_ACQUIRE);
    /* L1 TT Sources are owned by Timer Master, so allow only TIMPANI_EXEC_CBS */
    if (required_mask != 0U || num_out_edges == 0U ||
        num_out_edges > TIMPANI_MAX_OUT_EDGES ||
        exec_model != TIMPANI_EXEC_CBS) {
        errno = EINVAL;
        goto out;
    }

    if (pipeline_commit_alive(g_shm, gen, inst) != 0)
        goto out;

    __atomic_store_n((uint64_t*)&src->ready_state,
                     make_source_release_state(job_token), __ATOMIC_RELEASE);
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

/* Acquire the common L1/L2 Source job_token — read the release snapshot (O_RDONLY allowed, VFS-free) */
int ttsched_current_job_token(uint32_t* job_token)
{
    if (!job_token) {
        errno = EINVAL;
        return -1;
    }
    if (!g_shm) {
        errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP;
        return -1;
    }
    if (!g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }

    uint32_t gen_before, gen_after;
    uint64_t inst_before, inst_after;
    if (read_stable_pipeline_gen(g_shm, &gen_before, &inst_before) != 0)
        return -1;
    if (g_shm_generation != 0U && gen_before != g_shm_generation) {
        errno = ESTALE;
        return -1;
    }

    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }

    uint32_t n_tasks = __atomic_load_n(&g_shm->n_tasks, __ATOMIC_ACQUIRE);
    int idx = -1;
    int snapshot_errno = 0;
    uint32_t released_token = 0U;
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        snapshot_errno = EINVAL;
    } else {
        idx = find_task_slot(g_shm, n_tasks, g_my_name);
        if (idx < 0) {
            snapshot_errno = ENOENT;
        } else {
            const volatile struct timpani_pipeline_slot* src =
                &g_shm->pipeline[idx];
            uint32_t required_mask = __atomic_load_n(
                &src->required_mask, __ATOMIC_ACQUIRE);
            uint16_t num_out_edges = __atomic_load_n(
                &src->num_out_edges, __ATOMIC_ACQUIRE);
            if (required_mask != 0U || num_out_edges == 0U ||
                num_out_edges > TIMPANI_MAX_OUT_EDGES) {
                snapshot_errno = EINVAL;
            } else {
                uint64_t state = __atomic_load_n(
                    &src->ready_state, __ATOMIC_ACQUIRE);
                if ((state & TIMPANI_SOURCE_RELEASE_VALID) == 0ULL) {
                    snapshot_errno = EAGAIN;
                } else {
                    released_token = ready_sequence(state);
                    if (state != make_source_release_state(released_token))
                        snapshot_errno = EINVAL;
                }
            }
        }
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (read_stable_pipeline_gen(g_shm, &gen_after, &inst_after) != 0)
        return -1;
    if (gen_after != gen_before || inst_after != inst_before) {
        errno = ESTALE;
        return -1;
    }
    if (snapshot_errno != 0) {
        errno = snapshot_errno;
        return -1;
    }
    if (g_shm_instance_id == 0ULL)
        g_shm_instance_id = inst_before;

    g_shm_generation = gen_before;
    g_task_idx = idx;
    *job_token = released_token;
    return 0;
}

/* Read-only seqlock snapshot used only by ttsched_wait_data() (does not write publication_guard) */
static inline int pipeline_read_dispatch_snapshot(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_inst,
    int* out_idx, uint32_t* out_dispatch_counter)
{
    uint32_t gen_before, gen_after;
    uint64_t inst_before, inst_after;

    if (read_stable_pipeline_gen(shm, &gen_before, &inst_before) != 0)
        return -1;

    uint32_t n_tasks = __atomic_load_n(&shm->n_tasks, __ATOMIC_ACQUIRE);
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
            gen_after != gen_before || inst_after != inst_before) {
            errno = ESTALE;
            return -1;
        }
        errno = EINVAL;
        return -1;
    }

    int idx = (gen_before == g_shm_generation &&
               inst_before == g_shm_instance_id)
                  ? g_task_idx
                  : -1;
    if (idx < 0 || (uint32_t)idx >= n_tasks) {
        idx = find_task_slot(shm, n_tasks, g_my_name);
        if (idx < 0) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
                gen_after != gen_before || inst_after != inst_before) {
                errno = ESTALE;
                return -1;
            }
            errno = ENOENT;
            return -1;
        }
    }
    if (!atomic_name_equals(shm->tasks[idx].name, g_my_name)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
            gen_after != gen_before || inst_after != inst_before) {
            errno = ESTALE;
            return -1;
        }
        errno = EINVAL;
        return -1;
    }

    uint32_t cur = __atomic_load_n(
        &shm->pipeline[idx].dispatch_counter, __ATOMIC_ACQUIRE);

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0)
        return -1;
    if (gen_after != gen_before || inst_after != inst_before) {
        errno = ESTALE;
        return -1;
    }

    *out_gen = gen_before;
    *out_inst = inst_before;
    *out_idx = idx;
    *out_dispatch_counter = cur;
    return 0;
}

/* Successor wait API — supports O_RDONLY (PROT_READ) and FIFO-sequential consumption */
int ttsched_wait_data(uint32_t* job_token)
{
    if (!job_token) {
        errno = EINVAL;
        return -1;
    }
    if (!g_shm) {
        errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP;
        return -1;
    }
    if (!g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }

    uint32_t wait_gen = 0U;
    int has_waited = 0;

    for (;;) {
        uint32_t gen = 0U;
        uint64_t inst = 0ULL;
        int idx = -1;
        uint32_t cur = 0U;

        if (pipeline_read_dispatch_snapshot(
                g_shm, &gen, &inst, &idx, &cur) != 0) {
            return -1;
        }

        /* Immediately return ESTALE for a bound-generation mismatch or a generation change while waiting */
        if ((has_waited && gen != wait_gen) ||
            (g_shm_generation != 0U && gen != g_shm_generation)) {
            errno = ESTALE;
            return -1;
        }

        if (g_shm_instance_id == 0ULL)
            g_shm_instance_id = inst;

        /* Entry into the first valid generation: if attached before SHM publication
         * (g_attached_before_publish == 1, with the /proc lookup preceding startup),
         * start at cursor 0 to preserve token 0; if attached after stable publication,
         * snapshot the current cur to prevent replay of the past. */
        if (g_shm_generation == 0U || !g_dispatch_counter_synced) {
            g_shm_generation = gen;
            g_task_idx = idx;
            g_last_dispatch_counter = g_attached_before_publish ? 0U : cur;
            g_attached_before_publish = 0;
            g_dispatch_counter_synced = 1;
        } else {
            g_task_idx = idx;
        }

        if (counter_ahead(cur, g_last_dispatch_counter)) {
            uint32_t token = g_last_dispatch_counter;
            /* Consume FIFO-sequentially rather than jumping to current - 1U,
             * preventing a permanent ESTALE deadlock with the downstream
             * ready_sequence. (cur - token > 1 violates the Phase 0 pending<=1
             * assumption and is subject to a backlog warning.) */
            g_last_dispatch_counter = token + 1U;
            *job_token = token;
            return 0;
        }

        wait_gen = gen;
        has_waited = 1;
        /* A publication, shutdown, Safe Abandonment, or generation-change control
         * wake is accompanied by magic=0, odd gen, WRITER_PENDING, or a gen change,
         * so snapshot validation at the top of the loop returns ESTALE immediately.
         * A spurious wakeup with no generation or validity change (futex(2)), by
         * contrast, safely waits again with 0 executions. */
        long rc = syscall(SYS_futex,
                          (uint32_t*)&g_shm->pipeline[idx].dispatch_counter,
                          FUTEX_WAIT, cur, NULL, NULL, 0);
        if (rc < 0 && errno != EAGAIN && errno != EINTR) {
            return -1;
        }
    }
}

int ttsched_signal_ready(uint32_t job_token)
{
    uint32_t gen;
    uint64_t inst;
    if (!g_shm) {
        errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP;
        return -1;
    }
    if (!g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_shm_writable) {
        errno = EACCES;
        return -1;
    }
    if (pipeline_commit_enter(g_shm, &gen, &inst) != 0)
        return -1;

    int result = -1;
    if (g_shm_instance_id == 0ULL)
        g_shm_instance_id = inst;

    uint32_t n_tasks = __atomic_load_n(&g_shm->n_tasks, __ATOMIC_ACQUIRE);
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        errno = EINVAL;
        goto out;
    }

    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }
    /* Allow only the initial binding (g_shm_generation == 0U); return ESTALE immediately on a bound-generation mismatch */
    if (g_shm_generation == 0U) {
        g_shm_generation = gen;
        g_task_idx = -1;
    } else if (gen != g_shm_generation) {
        errno = ESTALE;
        goto out;
    }
    if (g_task_idx < 0 || (uint32_t)g_task_idx >= n_tasks) {
        g_task_idx = find_task_slot(g_shm, n_tasks, g_my_name);
        if (g_task_idx < 0) {
            errno = ENOENT;
            goto out;
        }
    }
    /* Atomically recheck the slot identity (name); the prior n_tasks range check makes this access safe */
    if (!atomic_name_equals(g_shm->tasks[g_task_idx].name, g_my_name)) {
        errno = EINVAL;
        goto out;
    }

    struct signal_edge_plan {
        uint16_t dst_slot_idx;
        uint16_t execution_model;
        uint32_t required_mask;
        uint32_t bit;
        uint64_t old_state;
        int do_wake;
    };

    const volatile struct timpani_pipeline_slot* src =
        &g_shm->pipeline[g_task_idx];
    uint16_t n_edges =
        __atomic_load_n(&src->num_out_edges, __ATOMIC_ACQUIRE);
    if (n_edges > TIMPANI_MAX_OUT_EDGES) {
        errno = EINVAL;
        goto out;
    }
    if (n_edges == 0U) {
        /* Sink Task (num_out_edges == 0): a safe no-op after passing identity and SHM validation */
        result = 0;
        goto out;
    }

    struct signal_edge_plan plans[TIMPANI_MAX_OUT_EDGES];

    /* Validate and preflight the complete fan-out before the first modification. */
    for (uint16_t i = 0; i < n_edges; i++) {
        uint16_t dst_idx = src->out_edges[i].dst_slot_idx;
        uint16_t bit_idx = src->out_edges[i].bit_idx;
        if (dst_idx >= TIMPANI_MAX_TASKS || dst_idx >= n_tasks ||
            bit_idx >= 16 || dst_idx == g_task_idx) {
            errno = EINVAL;
            goto out;
        }
        for (uint16_t prior = 0; prior < i; prior++) {
            if (plans[prior].dst_slot_idx == dst_idx) {
                errno = EINVAL;
                goto out;
            }
        }

        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[dst_idx];
        uint32_t bit = (1U << bit_idx);
        uint32_t req = __atomic_load_n(&dst->required_mask,
                                       __ATOMIC_ACQUIRE);
        uint16_t mode = __atomic_load_n(&dst->trigger_mode,
                                        __ATOMIC_ACQUIRE);
        uint16_t model = __atomic_load_n(&dst->execution_model,
                                         __ATOMIC_ACQUIRE);
        if (mode != TIMPANI_TRIG_ALL || req == 0U ||
            (req & ~TIMPANI_READY_MASK) != 0U ||
            (req & bit) == 0U ||
            (model != TIMPANI_EXEC_TT && model != TIMPANI_EXEC_CBS)) {
            errno = EINVAL;
            goto out;
        }

        uint64_t state = __atomic_load_n(
            (const uint64_t*)&dst->ready_state, __ATOMIC_ACQUIRE);
        if (ready_sequence(state) != job_token) {
            errno = ESTALE;
            goto out;
        }
        if ((ready_mask(state) & bit) != 0U) {
            errno = EALREADY;
            goto out;
        }

        plans[i].dst_slot_idx = dst_idx;
        plans[i].execution_model = model;
        plans[i].required_mask = req;
        plans[i].bit = bit;
        plans[i].old_state = state;
        plans[i].do_wake = 0;
    }

    /* Preflight ends here. Phase 0 does not guarantee a crash transaction after
     * the first CAS. An in-flight writer that already holds the guard checks
     * pipeline_commit_alive(), excluding WRITER_PENDING, to complete commit+wake
     * during a normal drain. */
    for (uint16_t i = 0; i < n_edges; i++) {
        if (pipeline_commit_alive(g_shm, gen, inst) != 0)
            goto out;

        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[plans[i].dst_slot_idx];
        uint64_t old_state = plans[i].old_state;

        while (1) {
            plans[i].do_wake = 0;
            uint32_t sequence = ready_sequence(old_state);
            if (sequence != job_token)
                break; /* No reset is possible under this guard: already committed. */

            uint32_t current_mask = ready_mask(old_state);
            if ((current_mask & plans[i].bit) != 0U)
                break; /* An identical concurrent call set this bit. */

            uint32_t merged = current_mask | plans[i].bit;
            uint64_t next_state = make_ready_state(sequence, merged);
            if (is_trigger_satisfied(
                    merged, plans[i].required_mask, TIMPANI_TRIG_ALL)) {
                next_state = make_ready_state(sequence + 1U, 0U);
                plans[i].do_wake =
                    plans[i].execution_model == TIMPANI_EXEC_CBS;
            }
            if (__atomic_compare_exchange_n(
                    (uint64_t*)&dst->ready_state, &old_state, next_state,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }
    }

    /* Make wakes observable only after every edge CAS has completed. */
    for (uint16_t i = 0; i < n_edges; i++) {
        if (!plans[i].do_wake)
            continue;
        if (pipeline_commit_alive(g_shm, gen, inst) != 0)
            goto out;
        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[plans[i].dst_slot_idx];
        __atomic_add_fetch((uint32_t*)&dst->dispatch_counter, 1U,
                           __ATOMIC_RELEASE);
        syscall(SYS_futex, (uint32_t*)&dst->dispatch_counter,
                FUTEX_WAKE, 1, NULL, NULL, 0);
    }
    /* TT target: Timer Master opens dispatch_counter in an allowed slot. */
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

- **Summary of the `ttsched_wait_data()` consumer contract**:
  1. A **read-only seqlock** (`pipeline_read_dispatch_snapshot()`) that does not write `publication_guard` verifies that `read_stable_pipeline_gen()` (even `generation > 0`, `magic`, ABI, `instance_id`, and `WRITER_PENDING == 0`) and the 16-byte atomic name comparison (`atomic_name_equals()`, including the preceding `n_tasks` validation) agree before and after the snapshot. It therefore operates on `O_RDONLY` (`PROT_READ`) mappings without `SIGSEGV` or C11 data races, and a consumer crash (`SIGKILL`) cannot permanently occupy `publication_guard`.
  2. `ttsched_init()` records `g_attached_before_publish = 1` for the normal startup path in which the SHM is not yet published or the slot is absent (with the `/proc` lookup occurring first), thereby preserving token `0` from cursor `0` on entry into the first valid generation. If attachment occurs at `cur = N > 0` after an SHM has already been published stably, it snapshots `g_last_dispatch_counter = N` so `0..N-1` is not replayed. When `counter_ahead(cur, g_last_dispatch_counter)` is true, the API does not jump to `current - 1U`; it instead performs **FIFO-sequential consumption (`token = g_last_dispatch_counter; g_last_dispatch_counter = token + 1U; *job_token = token`)** to prevent a permanent `ESTALE` deadlock with the downstream `ready_sequence`. If `cur - token > 1U`, this is observed and warned as a departure from the Phase 0 normal-acceptance assumption (`pending <= 1`). Downstream resynchronization and payload restoration on restart/recovery are Phase 2 (`REQ-DD-014`) scope.
  3. When no progress is available, it waits in `PROT_READ` mode using the kernel-supported `FUTEX_WAIT(&dispatch_counter, cur)`. After returning (`0`/`EAGAIN`/`EINTR`), it reevaluates the snapshot and generation at the top of the loop. A control wake associated with publication, shutdown, Safe Abandonment, or a generation change therefore returns `ESTALE` immediately, while a spurious wakeup with no generation or validity change safely waits again with 0 executions.
  4. An L2 CBS Task performs the packed `ready_state` CAS and the `dispatch_counter` increment/futex wake inside the `pipeline_commit_enter()` guard of `ttsched_signal_ready()` (during a normal `WRITER_PENDING=1` drain, it completes commit+wake through `pipeline_commit_alive()`). For an L1 TT Task, `Timer Master` checks `ready_sequence > dispatch_counter` with its internal lightweight guard (`tm_pipeline_commit_enter()`), then performs `update_current_slot()`, increments `dispatch_counter` by 1, and invokes `FUTEX_WAKE` in an allowed slot.

Success from `signal_ready` means that the complete outgoing-edge list passed preflight, the supplied token matched every destination, and every predecessor bit was committed by this call or had already been completed by an identical concurrent call. If a CAS retry sees that the same bit is already set or that the destination sequence has advanced, that edge is treated as converged. Every failure rollback branch in `pipeline_commit_enter()` invokes `pipeline_commit_exit()` to prevent a Lost Wakeup for a publisher waiting for drain. The publication guard keeps static metadata and the generation stable, while the per-edge CAS protects competing predecessor updates. Every static/preflight failure occurs mutation-free before the first CAS, and the CBS `dispatch_counter` and futex wake remain unchanged until all edge CAS operations have completed.

If the Source process terminates after the first destination CAS begins, only some destination bits may remain. Phase 0 does not treat this as an atomic fan-out transaction and does not guarantee automatic rollback, an idempotent retry, or successful quarantine. The affected chain run is recorded as failed/a departure from the test assumptions; commit markers, generation quarantine, and crash recovery are designed in Phase 2 `REQ-DD-014`. `pipeline_commit_enter()` itself does not wait; only the publisher performs a bounded wait for publication.

---

## 4. L-DD-P0-03 — `Timpani-N` Prechecks, Stable Publication, and Timer Discipline

### 4.1 Non-RT Node-Local Suitability Checks (`main.cpp`)

Before applying a table, perform **(a) validation of the O→N wire input** and **(b) self-validation of the N-internal builder invariants immediately before SHM publication (§4.2 step 3)** on the non-RT path. On failure, reject the execution candidate without changing active state.
- **(a) Validate the O→N wire input (`HierarchicalScheduleTable` and `PipelineTaskConfig`):**
  - On receiving `model == ExecutionModel.DATA_FLOW (2)` in `LayerConfig`, immediately reject it as unsupported in Phase 0 with `EINVAL`.
  - Verify `hyperperiod_us > 0` and validate duplicate `(workload_id_hash, task_id_hash)` values, predecessor/Successor limits, self-references, duplicate predecessors, and `trigger_mode == PIPELINE_TRIGGER_ALL` in `pipeline_tasks`.
  - Require the common-metadata execution layer of every Task to match exactly one TT entry or CBS entry.
  - Validate the representation ranges for TT-entry CPU/slot/timing and CBS-entry CPU/budget/period/deadline.
  - Verify that the Source/Successor role and execution layer are independent and that the class was not forcibly changed.
- **(b) Self-validate N-internal builder invariants (§4.2 step 3):** For `required_mask` and `out_edges` deterministically built from `predecessor_keys`, verify before publication that the upper 16 bits of `required_mask` are 0, `dst != src`, the incoming-bit OR-union equals `required_mask`, and the bijection `incoming_edges == popcount(required_mask)` holds (unit tests validate this through fault injection into the builder output).
- *(N does not recalculate DAG or TT/CBS schedulability.)*

### 4.2 Non-RT Common SHM Graph and Layer Registration

1. The SHM publisher uses the `(workload_id_hash, task_id_hash)` pair from each TT/CBS entry as the canonical key for the common `pipeline[]` index and records that pair in `pipeline[i].{workload_id_hash, task_id_hash}`. This hash pair is **for internal `Timpani-N` join validation and consumption by diagnostic tools**; Phase 0 `libttsched` looks up a slot with `prctl(PR_GET_NAME)` and `tasks[i].name` (node-unique `1..15`-byte ASCII). When recording `tasks[i].name`, `write_tasks_and_pipeline_slots()` uses `atomic_name_store()` (16-byte `__ATOMIC_RELAXED` stores followed by the later `even_gen` `__ATOMIC_RELEASE`) to prevent C11 data races.
2. Initialize `workload_id_hash`, `task_id_hash`, `execution_model`, `trigger_mode`, `required_mask`, and `out_edges` for every pipeline slot, and set `ready_state=0` and `dispatch_counter=0`. `ready_state=0` for a Source means that it has no release-valid marker; token `k` becomes valid when the first actual release authority records `make_source_release_state(k)`. `ready_state=0` for a Successor means active `ready_sequence == 0`. Before publication, the publisher validates `hyperperiod_us > 0`, then records a new `instance_id`, `origin_epoch_ns`, and `hyperperiod_us` in `pipeline_header` as diagnostic/Phase 1 metadata; these values are not used to calculate `job_token` or validate the RT fast path.
3. Immediately before publication, **self-validate the N-internal builder invariants** while constructing and checking the destination-indexed incoming-bit map and slot metadata (`n_tasks`, `dst == src` self-reference, `trigger_mode`, `execution_model`, and contamination of the upper 16 bits of `required_mask`).
   - Traverse the common DAG metadata's predecessor keys and assign a distinct bit position from `0..15` to every immediate predecessor of each Successor. Build `required_mask` from these assignments and create one corresponding `out_edge` for every predecessor.
   - Initialize the fixed arrays `incoming_masks[TIMPANI_MAX_TASKS]` and `incoming_edges[TIMPANI_MAX_TASKS]` to 0. During the complete slot traversal, validate `trigger_mode == TIMPANI_TRIG_ALL`, `execution_model ∈ {TIMPANI_EXEC_TT, TIMPANI_EXEC_CBS}`, and `(required_mask & ~TIMPANI_READY_MASK) == 0U`. During the complete outgoing-edge traversal, validate `dst_slot_idx < n_tasks`, `dst_slot_idx != src` (no self-reference), `bit_idx < 16`, membership in the destination `required_mask`, and absence of a duplicate edge between a Source and destination. Reject the execution candidate as a duplicate incoming-bit assignment if `incoming_masks[dst]` already contains the bit; otherwise set the bit and increment `incoming_edges[dst]`.
   - After traversing every active Source, require `incoming_masks[i] == required_mask[i]` and `incoming_edges[i] == popcount(required_mask[i])` for every active Pipeline slot. Thus, every required bit in `ALL` has exactly one incoming edge and no required bit lacks an edge. Reject bit aliases, an extra required bit, an edge bit outside the required mask, or contamination of the upper 16 bits before publication. A Task with `required_mask == 0` SHALL have `incoming_edges == 0`.

   ```c
   uint32_t incoming_masks[TIMPANI_MAX_TASKS] = {};
   uint16_t incoming_edges[TIMPANI_MAX_TASKS] = {};

   if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS)
       return EINVAL;
   for (uint16_t i = 0; i < n_tasks; i++) {
       if (pipeline[i].trigger_mode != TIMPANI_TRIG_ALL)
           return EINVAL;
       if (pipeline[i].execution_model != TIMPANI_EXEC_TT &&
           pipeline[i].execution_model != TIMPANI_EXEC_CBS)
           return EINVAL;
       if ((pipeline[i].required_mask & ~TIMPANI_READY_MASK) != 0U)
           return EINVAL;
   }
   for (uint16_t src = 0; src < n_tasks; src++) {
       uint16_t n_edges = pipeline[src].num_out_edges;
       if (n_edges > TIMPANI_MAX_OUT_EDGES)
           return EINVAL;
       for (uint16_t edge = 0; edge < n_edges; edge++) {
           uint16_t dst = pipeline[src].out_edges[edge].dst_slot_idx;
           uint16_t bit_idx = pipeline[src].out_edges[edge].bit_idx;

           if (dst >= n_tasks || dst == src || bit_idx >= 16)
               return EINVAL;
           for (uint16_t prior = 0; prior < edge; prior++) {
               if (pipeline[src].out_edges[prior].dst_slot_idx == dst)
                   return EINVAL;
           }
           uint32_t bit = 1U << bit_idx;
           if ((pipeline[dst].required_mask & bit) == 0U ||
               (incoming_masks[dst] & bit) != 0U)
               return EINVAL;
           incoming_masks[dst] |= bit;
           incoming_edges[dst]++;
       }
   }

   for (uint16_t dst = 0; dst < n_tasks; dst++) {
       uint32_t required = pipeline[dst].required_mask;
       if ((required & ~TIMPANI_READY_MASK) != 0U ||
           incoming_masks[dst] != required ||
           incoming_edges[dst] != __builtin_popcount(required))
           return EINVAL;
   }
   ```

   This full-table check is a precondition for N publication. A Source-local plan in RT `signal_ready()` cannot prove global bijection by itself, so it relies on validated immutable metadata for the lifetime of the publication generation.
4. Register TT Tasks with the existing `TtSlot`/Timer Master and CBS Tasks with the existing `CbsConfig`/CbsState. **`TimerMaster` SHM publication interface (complying with `CON-DD-06`)**: Do not remove or change the existing `void TimerMaster::set_schedule_table(const std::vector<SlotEntry>&, uint64_t, uint64_t)` signature (delegate internally with empty `pipeline_entries`), and add the overload `void TimerMaster::set_schedule_table(const std::vector<SlotEntry>& slots, const std::vector<PipelineEntry>& pipeline_entries, uint64_t hyperperiod_us, uint64_t epoch_ns)` to register CBS Tasks and Pipeline metadata at the same indices in SHM `tasks[]` and `pipeline[]`. `main.cpp` invokes this overload when `table.pipeline_tasks_size() > 0` and retains the existing call for non-Pipeline tables.
5. **Drain constants and `publication_guard` serialization**:
   Immediately before the actual publication, atomically set `TIMPANI_PUB_WRITER_PENDING` in `pipeline_header.publication_guard` to block new write-commit entries, and wait with `FUTEX_WAIT` for a bounded interval until already-started active commits reach `0`. The normal publication/update drain limit is `min(5 ms, hyperperiod_us / 4)`, applied by the `effective_pub_drain_timeout_us(hyperperiod_us)` helper. The last writer detects `WRITER_PENDING` with `active == 0` in `pipeline_commit_exit()` (including failure-rollback branches) and wakes the guard futex.

   ```c
   static inline uint32_t effective_pub_drain_timeout_us(uint64_t hyperperiod_us)
   {
       uint64_t quarter_hp = hyperperiod_us / 4U;
       if (quarter_hp > 0U && quarter_hp < TIMPANI_PUB_DRAIN_TIMEOUT_US)
           return (uint32_t)quarter_hp;
       return TIMPANI_PUB_DRAIN_TIMEOUT_US;
   }
   ```

   | Constant | Value | Applies To | Action on Timeout |
   |:--|:--:|:--|:--|
   | `TIMPANI_PUB_DRAIN_TIMEOUT_US` | **`5,000 µs` (`5 ms`)**<br/>*(effective: `effective_pub_drain_timeout_us(H) = min(5 ms, H/4)`)* | Normal publication and runtime table updates | Immediately clear `WRITER_PENDING`, then reject or defer application (**in-place overwrite prohibited**) |
   | `TIMPANI_PUB_MAX_DEFERS` | **`2` times** *(Phase 1+)* | Limit on consecutive drain-timeout deferrals | If exceeded, report final update failure (`TableApplied{success=false}`) |
   | `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` | **`50 ms`** | Daemon shutdown, SHM-object replacement, and cleanup of residual SHM at startup | Force invalidation with the **Safe Abandonment** procedure below |

6. **Per-Field Release-Store Publication Order (do not overwrite `publication_guard`)**:
   Copy-assigning the entire `struct timpani_pipeline_header` would overwrite the `WRITER_PENDING` bit with `0` before validation is complete, allowing a reader to enter an incomplete table. The following **per-field `__atomic_store_n(..., __ATOMIC_RELEASE)`** order MUST therefore be followed.

   ```c
   /* active_count == 0 is confirmed and TIMPANI_PUB_WRITER_PENDING remains set */
   __atomic_store_n(&shm->magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->generation, odd_gen, __ATOMIC_RELEASE);

   /* Update tasks[] (names via atomic_name_store), n_tasks, and pipeline[] (ready_state=0, dispatch_counter=0) */
   write_tasks_and_pipeline_slots(shm, candidate);

   /* Record individual pipeline_header fields (do not access publication_guard) */
   __atomic_store_n(&shm->pipeline_header.abi_version,
                    TIMPANI_PIPELINE_ABI_VERSION, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.reserved, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.abi_size,
                    (uint32_t)sizeof(*shm), __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.instance_id,
                    shm_instance_id, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.origin_epoch_ns,
                    origin_epoch_ns, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.hyperperiod_us,
                    hyperperiod_us, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.magic,
                    TIMPANI_PIPELINE_MAGIC, __ATOMIC_RELEASE);

   /* Even generation -> outer magic -> clear WRITER_PENDING last */
   __atomic_store_n(&shm->generation, even_gen, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->magic, TIMPANI_TTSCHED_MAGIC, __ATOMIC_RELEASE);
   __atomic_and_fetch((uint32_t*)&shm->pipeline_header.publication_guard,
                      ~TIMPANI_PUB_WRITER_PENDING, __ATOMIC_RELEASE);
   ```

7. **Forced Invalidation Procedure During Shutdown/Replacement (Safe Abandonment)**:
   In daemon-shutdown, SHM-object-replacement, and startup cleanup paths for residual SHM, if `active_count > 0` remains after `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` (`50 ms`) has elapsed (for example, a writer process exits with `SIGKILL` between `pipeline_commit_enter` and `exit`), log the remaining `active_count` as a warning and force invalidation in the following order:
   1. While **retaining** `TIMPANI_PUB_WRITER_PENDING`, store `shm->magic = 0` and `shm->pipeline_header.magic = 0` (`__ATOMIC_RELEASE`).
   2. **Never perform** `ftruncate()` or an in-place `pipeline[]` overwrite on the old SHM object (preventing `SIGBUS` and state corruption for surviving readers).
   3. To wake every waiting TT/Pipeline consumer, call `FUTEX_WAKE(..., INT_MAX)` on **both** `tasks[i].counter` and `pipeline[i].dispatch_counter` in every slot (`for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; i++)`) without incrementing either counter, then reclaim only the name namespace with `shm_unlink(TIMPANI_TTSCHED_NAME)`. On replacement/restart, create a full-size SHM object with a new inode and new `instance_id` using `O_CREAT | O_EXCL`.

   Safe Abandonment is not an operation that atomically cancels an active writer or stops a running thread. Because a writer that has already entered the old mapping can resume after 50 ms, the writer rechecks invalidation through `pipeline_commit_alive()` (`shm->magic`, `pipeline_header.magic`, `generation`, and `instance_id`) immediately before every destination CAS and every wake; if invalidation is detected, it stops with `ESTALE` after `pipeline_commit_exit()`. Phase 0 provides no cancellation guarantee that eliminates even the short race between the final recheck and the subsequent CAS/wake.

   A late CAS/wake after invalidation+unlink **cannot propagate to the new SHM object**; a consumer retaining the old object also observes `magic=0` or an `instance_id` mismatch during stable-state revalidation after the wake and returns `ESTALE`. The Phase 0 guarantee is therefore not "every operation on the old mapping is unconditionally canceled," but rather "the old object is not reused and a late operation is not consumed as execution in the active generation." Safe Abandonment does not cancel a Job that is already executing after receiving a token from `ttsched_wait_data()` before invalidation. During normal operation, an active-count leak from a stalled writer without publisher shutdown/replacement is not an automatic Safe Abandonment trigger; it is a departure from assumptions/Phase 2 recovery scope.

Publication itself does not increment `ready_state` or `dispatch_counter`. A Source release stamp is made only in the actual L1/L2 release path, not during publication. This ordering removes the concurrent interval in which `pipeline_commit_enter()` or the read-only seqlock can observe an incomplete state during initial publication or shutdown/invalidation in Phase 0. A normal Phase 1 runtime update containing a Pipeline reuses the same guard and `TIMPANI_PUB_DRAIN_TIMEOUT_US` / `TIMPANI_PUB_MAX_DEFERS` for a boundary commit, while Phase 2 adds a residual-state initialization policy for publication failure, rollback, and restart.

### 4.3 RT TT/CBS Execution Adapters

- **Lightweight `Timer Master` internal guard definition (`tm_pipeline_commit_enter()` / `pipeline_commit_exit()`):**
  Because `Timer Master` is the sole lifecycle owner of the SHM object, it uses the lightweight guard `tm_pipeline_commit_enter(shm, &gen)` in the RT slot loop; the guard operates only on the `shm` pointer and does not depend on `shm_open`/`fstat`/`close` or TLS variables. All release paths are unified through the existing `pipeline_commit_exit(shm)` function.
  ```c
  static inline int tm_pipeline_commit_enter(
      const volatile struct timpani_ttsched_shm* shm, uint32_t* out_gen)
  {
      if (!shm)
          return -1;
      uint32_t guard = __atomic_load_n(
          &shm->pipeline_header.publication_guard, __ATOMIC_ACQUIRE);
      if ((guard & TIMPANI_PUB_WRITER_PENDING) != 0U)
          return -1;

      uint32_t expected_gen = 0U;
      if (read_stable_gen(shm, &expected_gen) != 0)
          return -1;
      if (__atomic_load_n(&shm->pipeline_header.magic, __ATOMIC_ACQUIRE) !=
          TIMPANI_PIPELINE_MAGIC)
          return -1;

      uint32_t before = __atomic_fetch_add(
          (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
          __ATOMIC_ACQ_REL);
      if ((before & TIMPANI_PUB_WRITER_PENDING) != 0U ||
          (before & TIMPANI_PUB_ACTIVE_MASK) == TIMPANI_PUB_ACTIVE_MASK) {
          pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
          return -1;
      }

      uint32_t confirmed_gen = 0U;
      if (read_stable_gen(shm, &confirmed_gen) != 0 ||
          confirmed_gen != expected_gen ||
          __atomic_load_n(&shm->pipeline_header.magic, __ATOMIC_ACQUIRE) !=
              TIMPANI_PIPELINE_MAGIC) {
          pipeline_commit_exit(shm); /* Prevent a Lost Wakeup */
          return -1;
      }

      *out_gen = expected_gen;
      return 0;
  }
  ```
- **L1 TT Source or ordinary TT:** **Immediately before** spawning `cpu_threads_` in `TimerMaster::start()`, call `clock_gettime(CLOCK_REALTIME)` exactly 1 time to compute the single `base_cycle_start` shared by all CPU threads and pass it to each `cpu_thread_loop(..., base_cycle_start)`, thereby preventing initial `k = 0` phase skew across CPUs. If `required_mask == 0`, update `update_current_slot()` and `next_tt_start` as for the existing slot firing. A Pipeline Source (`num_out_edges > 0`) uses not a global Hyperperiod number, but **a pipeline-cycle ordinal `k = src_ordinal[slot_idx]++` that starts at `0` and increments by `+1` for every firing of that Source slot**; release-store it as `pipeline[i].ready_state = make_source_release_state(k)`, then increment `tasks[i].counter`, perform the futex wake, and release through `pipeline_commit_exit(shm)` (so even multiple firings within one `H` in a mixed table where `period < H` do not duplicate `k`). It does not use the Pipeline `dispatch_counter`; an ordinary TT Task retains its existing wake without a Source stamp.
  ```c
  if (tm_pipeline_commit_enter(shm, &gen) == 0) {
      uint32_t k = src_ordinal[slot_idx]++;
      __atomic_store_n(&pipeline[i].ready_state,
                       make_source_release_state(k), __ATOMIC_RELEASE);
      __atomic_add_fetch(&tasks[i].counter, 1U, __ATOMIC_RELEASE);
      futex_wake(&tasks[i].counter, 1);
      pipeline_commit_exit(shm);
  }
  ```
  If stamp entry fails or the generation changes, do not increment `src_ordinal` or `counter` and do not wake.
- **L1 TT Successor (lightweight `Timer Master` guard and preceding `pending` check):**
  - When the L1 TT Successor's slot time arrives, first **update `bpf_loader_.update_next_tt_start(slot.cpu, ...)`** to protect the following L1 TT slot; then acquire `tm_pipeline_commit_enter(shm, &gen)` and check `pending = counter_ahead(ready_sequence(ready_state), dispatch_counter)`.
  - **Only when `pending == true`**, ① activate `current_slot_map[cpu]` through `bpf_loader_.update_current_slot(slot.cpu, slot.slot_idx)`, ② increment `pipeline[idx].dispatch_counter` by exactly `1` (`__ATOMIC_RELEASE`), and ③ call `FUTEX_WAKE(&pipeline[idx].dispatch_counter, 1)`, then release the guard through `pipeline_commit_exit(shm)`. (`scx_bpf_kick_cpu()` is a kernel BPF kfunc, so userspace `Timer Master` does not call it directly. When the Task becomes Runnable through `FUTEX_WAKE`, the existing `ops.enqueue()` in `timpani.bpf.c` inserts `SCX_DSQ_LOCAL_ON | acpu` and the kernel then calls `scx_bpf_kick_cpu(acpu, SCX_KICK_PREEMPT)` internally.)
  - When **`pending == false` or guard acquisition fails**, **omit all** of `update_current_slot()`, `tasks[i].counter`/`dispatch_counter` increments, and `FUTEX_WAKE`, and immediately release the guard. **Do not actively overwrite `current_slot_map[cpu]` with `SLOT_NONE`** (because the Task does not become Runnable, BPF `ops.enqueue()` and kernel kicks automatically occur 0 times; this also avoids 1 unnecessary RT `bpf_map_update_elem` and preserves the `deadline_us` lookup in `ops.stopping()` for another TT Task overrunning its previous slot). `next_tt_start` still protects the following L1 slot.
- **L2 CBS Source or ordinary CBS:** Do not add a separate L2 release daemon/IPC to `Timpani-N`; retain the existing event path. For an L2 CBS Pipeline Source (`required_mask == 0 && num_out_edges > 0 && execution_model == TIMPANI_EXEC_CBS`), an application-side event adapter/test generator with `O_RDWR` (`timpani-pub`) permission calls `ttsched_release_source(task_name, k)` immediately before the existing event wake (socket/pipe/eventfd/futex) used to wake the L2 Source, stamping activation ordinal `k` into `pipeline[i].ready_state`. The awakened L2 Source then calls `ttsched_current_job_token(&token)` → `ttsched_signal_ready(token)` exactly as an L1 Source does. In the Phase 0 mixed fan-in `CAM(L1) + LIDAR(L2) → FUSION`, both sources publish `k = 0, 1, 2, ...` sequentially from the common start phase immediately after publication.
- **L2 CBS Successor:** Finalize `ALL` through the packed `ready_state` CAS inside the `signal_ready` commit guard. After all outgoing-edge CAS operations have completed, increment `dispatch_counter` by 1 and perform the corresponding futex wake. Once the awakened thread becomes Runnable, BPF applies only the existing CBS CPU, budget, and Gap rules; OI-DD-03 adds neither a separate CBS DSQ nor a BPF release API.

If ready arrives late for a Phase 0 L1 slot, omit `update_current_slot()` and `FUTEX_WAKE` in that slot and retain the pending ready. Consume the ready sequence once in the next TT slot. A normal Phase 0 test assumes at most 1 pending ready under the modulo comparison (`wake_delay < Source_period`); 2 or more pending ready sequences are recorded as a departure from the backlog/slot-miss test assumption. Pending expiration, deadline-miss handling, `Activation Window`, and `SKIP` are determined in Phase 1/2.

| Path | Owner | Allowed | Prohibited |
|:---|:---|:---|:---|
| **L1 Source** | Timer Master | Establish the single `base_cycle_start`; stamp each Task's ordinal `k` into the Source `ready_state`, then perform the `tasks[i].counter` slot wake | Per-thread catch-up, wall-clock token calculation, Pipeline pending check |
| **L2 Source** | Application-side event adapter / test generator (`O_RDWR`) | Call `ttsched_release_source(task_name, k)` immediately before the existing event wake (for `TIMPANI_EXEC_CBS` only) | Calling for an L1 Source (`EINVAL`), a separate Timpani-N L2 daemon/IPC, wall-clock token calculation |
| **L1 Successor adapter** | Timer Master | Update `next_tt_start` → use `tm_pipeline_commit_enter` to check `pending`; if true, perform `update_current_slot()`·`dispatch_counter++`·`FUTEX_WAKE` → `pipeline_commit_exit` | When `pending == false`, `update_current_slot()`, wake, or actively overwrite with `SLOT_NONE`; call `scx_bpf_kick_cpu` from userspace; make an RT VFS call; or enforce `Window` |
| **L2 Successor adapter** | `signal_ready` | After the complete outgoing-edge CAS set forms `ALL`, perform `dispatch_counter++`·`FUTEX_WAKE` | Wake before CAS, wait for a TT slot, or use a separate release daemon |
| **CBS CPU execution** | `scx_timpani` | Apply the existing budget/Gap gate after Runnable | Evaluate the Data-Ready graph |
| **libttsched** | Application API | VFS-free atomic commit (`pipeline_commit_alive`), 16B atomic name comparison, read-only seqlock, dispatch futex | RT `shm_open`/`fstat`, writes on `O_RDONLY`, implicit adoption of generation mismatches, consuming a control wake as execution |
| **BPF** | Kernel scheduler | Reuse the existing TT/CBS DSQ (`scx_bpf_kick_cpu` inside `ops.enqueue()`) and the budget/slot rules unchanged | A separate Data-Driven DSQ |

### 4.4 Per-Layer Enforcement in `scx_timpani`

- A TT Successor does not become Runnable before the `dispatch_counter` futex wake. After Timer Master verifies `pending ready` and performs `update_current_slot()` and `FUTEX_WAKE`, the kernel dispatches it through the existing `ops.enqueue()` in `timpani.bpf.c` (`SCX_DSQ_LOCAL_ON | acpu` + `scx_bpf_kick_cpu(acpu, SCX_KICK_PREEMPT)`) and the `SCHED_TYPE_TT` path.
- A CBS Successor becomes Runnable from the `ALL` commit wake and is enqueued on the existing `SCHED_TYPE_CBS` path. `ALL` only adds a release condition; it does not change BPF's budget/Gap/TT-active decisions.
- Both TT and CBS use the existing Task-layer rules for `DMISS`, budget, and trace decisions. Data-Ready itself is not treated as a scheduler fault or BPF dispatch queue.

---

## 5. L-DD-P0-04 — `Timpani-O` Execution Candidate Validation and Deterministic Topological Placement (`schedinfo_service.{h,cpp}`, `global_scheduler.{h,cpp}`)

1. **Protect the committed model and resolve the Task class (`schedinfo_service.{h,cpp}`)**:
   - In `ConvertToClassifiedTasks()`, use the explicit value when `grpc_task.has_temporal_class()` is true; otherwise inherit `request->temporal_class()` to determine `ct.mechanism`. Verify that the resolved class is not changed by `predecessors` or the Source/Successor role.
   - In `AddSchedInfo()`, before updating `workload_tasks_[wid]`, back up an existing entry (`auto prev_it = workload_tasks_.find(wid);`). If `RegenerateAllSchedules()` fails, restore the backup when one exists (`workload_tasks_[wid] = std::move(prev_entry);`); otherwise roll back with `erase(wid)`, preserving the existing committed model intact.
2. **Normalize Task references and validate/sort the DAG deterministically (`ValidateAndSortDag`)**:
   - Standardize the internal identity as `TaskNameKey = std::pair<std::string, std::string>` `(workload_id, task_id)`. Interpret `TaskReference.task_id` as an exact 1:1 match with `TaskInfo.name` (field 1). Use only `std::map` and lexicographically ordered `std::set` (prohibit `std::unordered_map`).
   - If `TaskReference.workload_id` is an empty string, fill it with the current `SchedInfo.workload_id`. In Phase 0, reject the reference with `InfeasibleReason::InvalidDag` if the normalized result differs from the current workload.
   - For compatibility with the existing SHM lookup, the Task name (`task_id`) MUST be node-unique and `1..15` bytes of ASCII (`REQ-DD-001`, `ASM-DD-01`). Reject duplicate normalized Task keys, duplicate `task_id` values or length violations within the node, self-references, nonexistent references, **duplicate predecessor declarations within the same Task's `predecessors` array (for example, `[CAM, CAM]` — implicit deduplication is prohibited)**, and collisions of the `(fnv1a(workload_id), fnv1a(task_id))` hash pair with `InfeasibleReason::InvalidDag`.
   - Enforce a maximum of `16` immediate predecessors and `8` outgoing edges per vertex. If `out_topo_rank.size() != task_index.size()` after Kahn's Algorithm terminates using a lexicographically ordered ready queue, reject the cyclic dependency.
3. **Class-independent table construction and correction of L1→L1/CBS-only placement (`build_table`, `place_tt_slots`)**:
   - `PipelineTaskConfig` has no execution-layer field; it contains only `PipelineTaskKey(workload_id_hash, task_id_hash)` and the predecessor-key list. Deterministically sort the normalized predecessor keys of every Successor and assign distinct bit positions in ascending order. With `n` predecessors, the generated `required_mask` is exactly `(1U << n) - 1U`; with `n == 0`, it is `0`. Create TT/CBS entries separately according to the resolved class and do not publish the existing `ExecutionModel.DATA_FLOW = 2`.
   - **Guarantee CBS-only `hyperperiod_us > 0`:** When `all_periods.empty()` and `!cbs_tasks.empty()`, calculate `calculate_hyperperiod()` from the positive `period_us` values in `cbs_tasks` and use it as `global_hp` (reject with `InfeasibleReason::InvalidDag` if every CBS `period_us == 0`). Existing behavior remains unchanged if there is at least 1 TT Task.
   - **Guarantee L1 → L1 slot-offset ordering (`place_tt_slots()`):** Apply the sort key `(topo_rank, deadline_us, workload_id, task_id)` (this produces 0 regressions for non-DAG Tasks because they all have `topo_rank == 0`, preserving the existing DM ordering plus a deterministic tie-break). For each L1 Task placement, search `find_free_slot(min_start, task.wcet_us, frame_end)` from `min_start = std::max(frame_start, max_placed_l1_pred_end)`. After placement, verify `offset(succ) >= offset(pred) + duration(pred)` for every `L1(pred) → L1(succ)` edge and reject a violation in advance with `InfeasibleReason::TtSlotConflict`.
   - Build a `TtSlot` from the input timing for an L1 Task and a `CbsConfig` from the input timing for an L2 Task. Source/Successor does not determine this branch. Emit one outgoing edge for every assigned predecessor/destination pair and perform the same exact incoming-mask equality check as in §4.2 before returning the execution candidate.
4. **Phase 0 placement and deferral**:
   - Support mixtures of Tasks with explicit/inherited classes in the same workload, using the existing TT placement (including the L1→L1 ordering correction above) and CBS admission/worst-fit, respectively.
   - Phase 1 extends support to cross-workload `TaskReference` (including collision-resolution rules for `task_id` values across different workloads, `OI-DD-06`), TT Window/CBS Gap and budget-delay aggregation in mixed DAGs, `E2E` validation, and Pipeline updates. Do not include an `E2E` field in the Phase 0 OI-DD-01 protobuf.

---

## 6. Protobuf Interface Proposal (`OI-DD-01`, 008 Field Allocation Aligned)

> **Caution**: Preserve the field allocation established by 008:
> `min_inter_arrival_us = 11`, `temporal_class = 12`, and
> `predecessors = 13`. The proposal SHALL NOT be applied to the actual
> `.proto` before Project Leader approval under `CON-DD-05`.
> The build environment requires **`protoc >= 3.15`** for `proto3 optional`.

```protobuf
// timpani-o/proto/schedinfo.proto (package schedinfo.v1)
message TaskReference {
  // Phase 0: empty means the enclosing SchedInfo.workload_id.
  string workload_id = 1;
  // Matches TaskInfo.name (field 1) 1:1.
  string task_id = 2;
}

message TaskInfo {
  // ... existing fields 1..10 (field 1: string name) ...
  // Field 11 is owned by the 008 Event-Driven extension.
  uint32 min_inter_arrival_us = 11;
  // 008 reserved field 12 for this per-Task override.
  // Explicit value 0 selects L1; only absence inherits.
  optional TemporalClass temporal_class = 12;
  repeated TaskReference predecessors = 13;
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

The `TaskInfo` allocation is proposed as `11`/`12`/`13`. Field `11` remains the
008-owned `min_inter_arrival_us`, field `12` uses the 008 reservation for the
per-Task `temporal_class`, and field `13` holds `predecessors`. Keep
`HierarchicalScheduleTable.pipeline_tasks = 6`. This resolves the wire-number
collision but does not by itself approve moving the Task-level class override
into Phase 0 or applying the schema change.

Interpret `TaskReference.task_id` as an exact 1:1 match with
`TaskInfo.name` (field 1). Retain `SchedInfo.temporal_class` as the existing
workload default. Because O must detect the presence of
`TaskInfo.temporal_class`, use `proto3 optional` (`protoc >= 3.15`) rather
than a plain enum. The proposed presence rule supersedes the 008 P2 placeholder
`0 = inherit`: explicit `TEMPORAL_PERIODIC = 0` selects L1, while only an absent
field inherits. Adopting that semantic change is part of the required
`CON-DD-05` approval.

`PIPELINE_TRIGGER_ALL` is the sole valid trigger-enum value in Phase 0.
Retain `ExecutionModel.DATA_FLOW = 2`, already defined in
`node_control.proto`, in the enum to preserve wire compatibility, but keep
it unused in Phase 0. O does not publish it, and N rejects receipt with
`EINVAL`.

Do not duplicate dependencies in the TT/CBS entries themselves. Place O→N DAG metadata only in `PipelineTaskConfig`, and require N to connect each key pair to exactly one `TtSlot` or `CbsConfig`. For node-level SHM name lookup, retain the `REQ-DD-001` and `ASM-DD-01` constraint that the `1..15`-byte ASCII `task_id` be node-unique in Phase 0 as well.

---

## 7. L-DD-P0-05 — Test Integration Points

Unit tests SHALL verify that the DAG is processed without inferring an execution layer and that L1/L2 connections execute through the gate for each layer.

| Test | Verification |
|:---|:---|
| Class resolution and rollback | In `SchedInfoServiceImpl`, an explicit Task class takes precedence over the workload default, while an omitted Task class inherits the default (`predecessors` do not affect the class). If `RegenerateAllSchedules()` fails, the existing committed workload entry is restored and preserved. |
| Reference normalization/rejection | Verify the 1:1 mapping `TaskReference.task_id` ↔ `TaskInfo.name`; resolve an empty `workload_id` to the current workload; and reject, before activation, Phase 0 cross-workload references, duplicate keys, duplicate node-local `task_id` values or `1..15`B ASCII violations, duplicate `predecessors` within the same Task (`[CAM, CAM]`), hash-pair collisions, and nonexistent references. |
| O DAG/placement | Normalized keys, topological order, and TT/CBS placement are input-order independent, and the execution class equals the explicit/inherited value. `L1 → L1` uses `(topo_rank, deadline_us, workload_id, task_id)` + `min_start` to guarantee `offset(succ) >= offset(pred) + duration(pred)` (0 regressions for non-DAG cases), while a CBS-only chain creates `hyperperiod_us > 0` from the LCM of the CBS periods. |
| N wire registration | The `(workload_id_hash, task_id_hash)` pair is connected to exactly one TT/CBS entry and the class is unchanged; receipt of `ExecutionModel.DATA_FLOW = 2` is rejected with `EINVAL`. The `TimerMaster::set_schedule_table()` overload registers CBS/Pipeline entries at the same index in `tasks[]` and `pipeline[]`. |
| N-internal builder bijection (fault injection) | Immediately before publication, accept a normal N-internal builder output for a Successor with two predecessors represented by bits `0` and `1` and `required_mask=0b11`. In separate fault-injection unit tests, inject an out-of-range `n_tasks`, `dst == src`, two incoming edges both on bit `0`, mask `0b111` without a third edge, an edge bit outside the mask, and upper-16-bit contamination (`0x10003`); verify rejection and immutability of the committed execution candidate/active SHM. |
| SHM ABI and initialization | Retain the existing `void ttsched_init()` signature. For non-RT `fstat() < 784B`, `g_shm_init_errno = EINVAL` and a subsequent Pipeline API returns `EINVAL`; after failed reinitialization, `g_shm_mapping = {NULL, 0}` prevents a double `munmap`. On a 784B legacy/short mapping, only the TT API works (`ttsched_wait_next_period()` uses `g_shm_map_size` for `munmap`), and Pipeline APIs return `ENOTSUP` without accessing the extension. A magic/ABI-size mismatch returns `EINVAL`; unsupported versions with valid magic return `ENOTSUP`. Verify that the fd closes immediately after `mmap()` and that the `pthread_key` destructor has 0 `munmap` leaks when a thread terminates. |
| Exact Task-name identity | Given distinct slots named `task` and `task2`, `find_task_slot("task")` selects only `task`, and `find_task_slot("task3")` returns `-1` rather than binding the `task`/`task2` prefix. If a previously bound slot is replaced by another valid name, the `atomic_name_equals()` recheck returns `EINVAL`. A slot with no NUL in 16 bytes, an empty name, or nonzero bytes after the terminating NUL is malformed and cannot match a valid expected name. |
| VFS-free replacement/shutdown detection | Verify 0 calls to `shm_open`/`fstat`/`close` on the RT paths (`release_source`, `signal_ready`, `wait_data`, `current_job_token`, and `Timer Master`) and the 16-byte atomic name comparisons (`atomic_name_equals`/`atomic_name_store`, with 0 TSAN races). On a generation transition within the same SHM (`g → g+2`), every one of `signal_ready`/`wait_data`/`current_job_token`/`release_source` returns `ESTALE` immediately without implicit adoption. For Safe Abandonment (`WRITER_PENDING` retained + `shm->magic=0` + `pipeline_header.magic=0` + `FUTEX_WAKE(INT_MAX)` on `tasks[]`/`pipeline[]` + `shm_unlink` + `O_CREAT|O_EXCL` with a new `instance_id`) or an `instance_id` change, RT calls through the old mapping return `ESTALE` without accessing or executing the new `pipeline[]`; verify normal reconnection to the new object after non-RT `ttsched_init()` reinitialization. Pause a writer immediately before a CAS/wake for at least 50 ms, abandon the old object, and resume it: require 0 new-object modifications, 0 executions by old consumers, and an old-API `ESTALE`. In the pause/resume race immediately after the last revalidation, a late CAS/wake on the old object itself is allowed but MUST NOT affect the active generation. |
| Permissions and read-only seqlock | An `O_RDWR` failure for a Pipeline writer returns `EACCES`; an `O_RDONLY` (`PROT_READ`) Successor's `ttsched_wait_data()` and `ttsched_current_job_token()` succeed without a `publication_guard` write or `SIGSEGV`; and `ttsched_signal_ready()`/`ttsched_release_source()` on `O_RDONLY` return `EACCES`. After identity validation, `signal_ready()` from an `O_RDWR` Sink (`num_out_edges == 0`) returns no-op `0`. Verify in the deployment manifest that `timpani-pub` members match the trusted-writer list and untrusted workloads belong only to the RO group. Do not test isolation against arbitrary modification of another slot/header by an RW writer as a Phase 0 guarantee. |
| Common `job_token` acquisition | Only when an L1 Source (`Timer Master` with a single `base_cycle_start` and Source-specific `src_ordinal++`) or an L2 CBS Source (`ttsched_release_source()`, enforcing `TIMPANI_EXEC_CBS` and rejecting calls for an L1 target with `EINVAL`) has stamped `make_source_release_state(k)` does `ttsched_current_job_token()` return `k`. A release-invalid state returns `EAGAIN`, a non-Source slot or corrupt state returns `EINVAL`, and a generation change returns `ESTALE`. Within `wake_delay < Source_period`, shifting an app wake behind the Hyperperiod boundary (`offset=80ms` → `105ms`) or moving `CLOCK_REALTIME` forward/backward does not change the returned token or downstream `ready_sequence`. Across the 32-bit activation ordinal boundary `0xFFFFFFFF → 0`, the Source and Successor map to the same cycle under the wrap-comparison rule. |
| Fan-out preflight | On a Source with multiple edges, contaminate the `n_tasks` boundary and corrupt a later edge (`dst_idx`, `bit_idx`, upper bits/membership of `required_mask`, `trigger_mode`, `execution_model`, duplicate destination, or `num_out_edges` overflow), then call `signal_ready`. Require `-1`/`EINVAL` and 0 changes to the mask, sequence, dispatch counter, or futex wake across the complete fan-out before the first CAS. If a later destination's preflight token is made `ESTALE` or `EALREADY`, the earlier valid destination remains unchanged. For a valid fan-out, the first CBS wake occurs only after all CAS operations finish. With a separate failpoint, terminate the Source between destination CAS operations, immediately after CAS, or around the first wake: Phase 0 does not guarantee rollback/retry success; observe and record the partial state and fail the chain run. |
| Payload visibility/retention | After a Producer records a generation marker and pattern in a token-addressed payload and calls `signal_ready()`, the Consumer reads the same token/generation/pattern after `wait_data(token)`. On `signal_ready()` failure, that payload is not approved for a new dispatch and active-token storage is not overwritten. Repeatedly verify, with a minimum 2-slot PoC for Phase 0 `pending <= 1`, that the Producer does not overwrite an approved slot while the Consumer is processing it. Payload location, acknowledgement, and final reuse time are application contracts and are not owned by the TIMPANI API. |
| Concurrency and drain/Lost Wakeup | Verify that the 64-bit `ready_state` CAS is lock-free on the target, the final predecessor CAS atomically resets the mask and increments the sequence, and concurrent predecessor contention produces exactly 1 wake. An identical concurrent `signal_ready` that sets a preflighted bit or advances the sequence converges successfully (`0`) without a duplicate wake. If a normal `WRITER_PENDING=1` drain starts immediately after the writer completes the `ALL` CAS, `pipeline_commit_alive()` completes `dispatch_counter++` and `FUTEX_WAKE` to the end, and `pipeline_commit_exit()` terminates drain normally (0 Lost Wakeups and 0 partial fan-outs). Also verify on rollback failure in `pipeline_commit_enter()` and `tm_pipeline_commit_enter()` that `pipeline_commit_exit()` wakes the publisher waiting for drain with 0 Lost Wakeups, normal deferral works at `effective_pub_drain_timeout_us(H) = min(5 ms, H/4)`, and 50 ms shutdown Safe Abandonment works. |
| Cycle identity and FIFO consumption | With active token `k`, after A sets its bit, a duplicate `(A,k)` returns `EALREADY`, `A(k+1)`/`B(k+1)` return `ESTALE`, and mask/sequence/dispatch remain unchanged. If, during CAS contention after preflight, an identical concurrent call sets the bit or advances the sequence, the call converges (`0`); an old-token call arriving after the sequence advance is rejected with `ESTALE` during preflight. Attachment while the SHM is unpublished (`g_attached_before_publish=1`, with the `/proc` lookup preceding startup) or before the first dispatch (`cur=0`) receives token `0` through FIFO; a single-node late attach after `dispatch_counter=N > 0` has already been published (`init cursor=N`) waits only for the next dispatch and does not replay tokens `0..N-1` (downstream `ready_sequence` resynchronization across multiple hops is Phase 2 `REQ-DD-014`). Observe `cur - cursor > 1` as a departure from the backlog assumption. |
| Wake contract | Treat only an increment of `dispatch_counter` as an execution wake. A control wake accompanying publication/shutdown/generation change returns `ESTALE`, while a spurious wakeup with unchanged state and generation (`FUTEX_WAIT == 0`) safely waits again with 0 executions and without non-RT remapping. Modulo comparison at the `0x7fffffff` boundary has no duplication or loss. |
| L1 gate | Before `ALL` or when `pending ready == false`, omit all of `update_current_slot()`, `dispatch_counter++`, and `FUTEX_WAKE` (no active overwrite with `SLOT_NONE`, 0 Runnable transitions, BPF `ops.enqueue()` calls, and kernel kicks; `next_tt_start` is still updated). If ready is present before the slot, activate `update_current_slot()` in that slot, apply `dispatch_counter` `+1`, and invoke `FUTEX_WAKE`; duplicate wake for the same ready is 0; ready arriving after the slot remains pending until the next slot. |
| L2 gate | Immediately after the packed `ALL` CAS, increment `dispatch_counter` by 1 and perform 1 futex wake; actual CPU execution may be delayed by CBS budget/Gap. |
| CBS BPF boundary | When TT active or budget=0, the CBS Task is not dispatched immediately, but its release is retained and execution occurs through the existing throttled/replenishment paths. |
| Combinations | Mix Source/Successor roles and pass TT→TT, TT→CBS, CBS→TT, and CBS→CBS at least once each. |

The boundary matrix in SRS-009 §5.1 traces backward to the test rows above as follows.

| BM | LLD Test Integration Point | Minimum Observed Fields |
|:---|:---|:---|
| **BM-DD-P0-01** | `Common job_token acquisition`, `Wake contract` | source release state, token, downstream ready sequence, `clock_gettime` call count |
| **BM-DD-P0-02** | `Cycle identity and FIFO consumption`, `SHM ABI and initialization` | `dispatch_counter`, `g_attached_before_publish`, init cursor, first returned token, replay count |
| **BM-DD-P0-03** | `Fan-out preflight`, `Concurrency and drain/Lost Wakeup` | failpoint, destination-specific mask/sequence, wake count, partial-state/disposition |
| **BM-DD-P0-04** | `payload visibility/retention` | payload token/generation/pattern, producer reuse event, consumer completion |
| **BM-DD-P0-05** | `Permissions and read-only seqlock` | UID/GID/ACL, allowlist membership, API `errno` |
| **BM-DD-P0-06** | `VFS-free replacement/shutdown detection`, `Concurrency and drain/Lost Wakeup`, `Wake contract` | normal drain `WRITER_PENDING` wake completion, `g→g+2` `ESTALE`, pause/resume point, old/new instance, late CAS/wake, old API result |
| **BM-DD-P0-07** | `Fan-out preflight`, `Concurrency and drain/Lost Wakeup` | fan-out=8, concurrent writers, latency distribution, wake order/count, timeout/`EAGAIN` |

Record hardware verification as not performed when target execution is not performed.
