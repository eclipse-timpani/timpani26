<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-009: Data-Driven Workload — Phase 0 상세 설계

**작성일:** 2026-10-27  
**최종 수정:** 2026-11-01  
**상태:** Draft — 내부 처리 및 인터페이스 제안 계약 (구현 완료 아님)  
**상위 기준:** [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, [SRS-009](SRS-009-data-driven-workload.md), [HLD-009](HLD-009-data-driven-workload.md)  
**설계 결정:** [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01`..`04`, [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, `D-P0-06`

---

## 1. 설계 경계, 사전 승인 대상 및 기존 접점

본 문서는 Phase 0의 공통 DAG 자료구조, ready/dispatch 분리, TT/CBS 실행 어댑터, 사전 확인과 시험 접점만 상세화한다. Phase 1의 Window/혼합 `E2E` 및 Phase 2의 `SKIP`/복구 상태 기계는 포함하지 않는다.

| 기존 접점 | Phase 0 상세 설계 의도 | 승인·호환 제약 |
|:---|:---|:---|
| `timpani-o/proto/*.proto` | optional Task class override, 구조화 `TaskReference`, workload/task hash 쌍 공통 DAG metadata | **`OI-DD-01` 승인** |
| `sample-apps/src/libttsched.h/c` | versioned Pipeline extension, packed `ready_state`, dispatch-only futex, `wait_data`/`signal_ready` 및 ABI/`errno` 계약 | **`OI-DD-02` 승인** |
| `timpani-o/src/global_scheduler.*` | Task class를 explicit-over-inherit로 해석하고 DAG 검증 후 TT/CBS 배치와 graph meta를 분리 생성 | 기존 API 보존 |
| `timpani-n/src/main.cpp` | TT/CBS entry와 공통 graph meta를 함께 로컬 확인·등록 | 계층별 가능성 재계산 없음 |
| `timpani-n/src/timer_master.*` | L1 Task는 allowed slot에서 pending ready를 dispatch | L2 release는 하지 않음 |
| `timpani-n/src/bpf/timpani.bpf.c` | 기존 `SCHED_TYPE_TT`/`CBS` dispatch gate 재사용 | 별도 DSQ 없음 |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` 공유 메모리 레이아웃 (`libttsched.h`)

[DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` 및 [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`에 따라 기존 헤더와 `tasks[TIMPANI_MAX_TASKS]`(오프셋 `0..783`)를 바이트 단위로 그대로 보존한다. 그 뒤에 버전이 있는 Pipeline extension header와 `pipeline[TIMPANI_MAX_TASKS]` 영역을 추가한다.

```c
#define TIMPANI_TTSCHED_MAGIC       0x54494D50U /* "TIMP" */
#define TIMPANI_PIPELINE_MAGIC      0x50495045U /* "PIPE" */
#define TIMPANI_PIPELINE_ABI_VERSION 1U
#define TIMPANI_MAX_TASKS           32
#define TIMPANI_MAX_OUT_EDGES       8

#define TIMPANI_PUB_WRITER_PENDING 0x80000000U
#define TIMPANI_PUB_ACTIVE_MASK    0x7FFFFFFFU

#define TIMPANI_TRIG_ALL 0U /* Phase 0 유일한 유효 trigger */
#define TIMPANI_TRIG_ANY 1U /* Phase 1 이후 확장 예약값; Phase 0 게시 금지 */

#define TIMPANI_READY_MASK         0x0000FFFFULL
#define TIMPANI_READY_SEQUENCE_SHIFT 32
#define TIMPANI_READY_SEQUENCE_MASK  0xFFFFFFFFULL

/* 기존 슬롯 구조체 — 크기(24B) 및 필드 오프셋 변경 없음 */
struct timpani_task_slot {
    char     name[16]; /* task comm 이름 (PR_SET_NAME, <=15자 + NUL) */
    uint32_t counter;  /* Timer Master 주기 슬롯 발화 카운터 */
    uint32_t _pad;
};

struct timpani_out_edge {
    uint16_t dst_slot_idx; /* 후행 태스크의 SHM 인덱스 (0..TIMPANI_MAX_TASKS-1) */
    uint16_t bit_idx;      /* 후행 ready_mask에서 켤 비트 위치 (0..15) */
};

#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    uint64_t workload_id_hash;
    uint64_t task_id_hash;
    uint64_t ready_state;
    /* ready_state: [63:32] ready sequence, [31:16] reserved, [15:0] ready mask */
    uint32_t required_mask;    /* 필요한 선행 비트 (0이면 Source/일반 Task) */
    uint32_t dispatch_counter; /* TT slot 또는 CBS release가 허용한 실행 수, 유일한 futex word */
    uint16_t num_out_edges;
    uint16_t trigger_mode;     /* Phase 0: TIMPANI_TRIG_ALL */
    uint16_t execution_model;  /* TT/CBS; Source/Successor 역할과 독립 */
    uint16_t reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

/* 후방 확장 header — tasks[]가 끝나는 byte 784에 배치 */
struct timpani_pipeline_header {
    uint32_t magic;              /* TIMPANI_PIPELINE_MAGIC */
    uint16_t abi_version;        /* TIMPANI_PIPELINE_ABI_VERSION */
    uint16_t reserved;
    uint32_t abi_size;           /* 전체 struct timpani_ttsched_shm 크기 */
    uint32_t publication_guard;  /* bit31=writer pending, 하위 31bit=활성 commit 수 */
};

struct timpani_ttsched_shm {
    uint32_t magic;      /* 안정 게시 완료 시 TIMPANI_TTSCHED_MAGIC, 게시 중/종료 시 0 */
    uint32_t n_tasks;    /* 점유 슬롯 상한 인덱스 + 1 */
    uint32_t generation; /* 홀수 = 게시 중, 짝수(>0) = 안정 게시 상태 */
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

새 client는 `fstat()`로 SHM 크기를 먼저 확인한다. 파일 크기가 기존 784바이트 prefix보다 짧으면 TT 경로도 `EINVAL`로 거절하고, prefix는 유효하지만 extension 전체가 없으면 기존 `ttsched_wait_next_period()`만 계속 사용할 수 있다. Pipeline API는 extension `magic`, `abi_version`, `abi_size`, 실제 파일 크기를 모두 확인한 뒤에만 `pipeline[]`에 접근하며, 실패 시 `ENOTSUP` 또는 `EINVAL`을 반환한다.

`publication_guard`는 extension header 안에서 세대 확인과 상태 갱신 사이의 TOCTOU를 막는다. 게시자는 writer-pending 비트를 원자적으로 켜 새 commit을 차단하고, 기존 활성 commit이 `0`이 될 때까지 제한된 시간만 대기한 뒤 `generation`을 홀수로 만들고 SHM을 갱신한다. reader는 writer-pending이 없을 때 활성 commit 수를 증가시키고 동일 안정 세대를 다시 확인한다. 따라서 이전 세대 commit이 끝난 뒤 게시하거나, 게시자가 먼저 시작되면 reader가 상태 변경 없이 실패하는 두 순서만 허용한다.

---

## 3. L-DD-P0-02 — `libttsched` 동기화 및 안정 게시 확인 (`libttsched.c`)

### 3.1 SHM 매핑 및 공개 API 계약

- 기존 공개 API는 유지한다: `ttsched_init()`은 기존 TT prefix를 위한 `O_RDONLY` fallback을 계속 지원하고 `ttsched_wait_next_period()`의 signature와 의미를 바꾸지 않는다.
- `ttsched_init()`은 먼저 `shm_open()` 후 `fstat()`로 실제 객체 크기를 확인한다. 784바이트 prefix 이상이면 기존 `magic/n_tasks/generation/tasks[]` 경로를 사용할 수 있고, 전체 `sizeof(struct timpani_ttsched_shm)` 이상이며 extension `magic/version/size`가 유효할 때만 Pipeline 영역을 사용한다.
- write가 필요한 Data-Driven Source는 `O_RDWR` + `PROT_READ|PROT_WRITE`로 열리고, 권한이 없으면 `g_shm_writable = 0`으로 기존 읽기 전용 TT 경로에 fallback한다. read-only Successor는 `O_RDONLY`로 `ttsched_wait_data()`를 사용할 수 있지만 `ttsched_signal_ready()`는 `EACCES`를 반환한다.
- `Timpani-N`은 SHM 객체의 소유자다. 일반 TT/Successor는 최소 O_RDONLY, Data-Driven 완료 신호를 보내는 Task는 O_RDWR 권한을 받아야 한다. Phase 0 구현은 permission·group/UID/GID 배포 정책을 명시하고, 승인되지 않은 전체-world-write를 생산 배포의 정당한 ABI 계약으로 간주하지 않는다. 현재 PoC의 `0666`은 regression 시험 범위로만 유지하거나 동등한 배포 ACL로 대체한다.
- 공개 API 시그니처 *(사전 승인 대상, `OI-DD-02`)*:
  ```c
  int ttsched_wait_next_period(void); /* 기존 TT API, 변경 없음 */
  int ttsched_wait_data(void);        /* 실제 dispatch wake만 0 */
  int ttsched_signal_ready(void);     /* 출력 기록 후 완료 commit */
  ```
- 신규 API는 실패 시 `-1`과 `errno`를 반환한다: `EACCES`(write 권한 없음), `ENOTSUP`(구 버전/없는 extension/미지원 ABI version), `EINVAL`(extension magic/size 불일치), `ESTALE`(publish/control wake 또는 세대 전환), `EAGAIN`(commit contention으로 재시도 필요), `ENOENT`(Task/후행 참조 없음)다.
- Phase 0 앱 계약은 각 실행 job 완료마다 `ttsched_signal_ready()`를 최대 1회 호출하는 것이다. 출력 데이터 기록 후 호출하며, `0` 반환 후 재호출하지 않는다. job별 중복 억제를 위한 API 파라미터는 Phase 1에서 필요성이 확인될 때 확장한다.

### 3.2 안정 세대 확인, `ttsched_signal_ready()` 및 `ttsched_wait_data()` 상세 로직

```c
static __thread int g_shm_writable = 0;
static __thread int g_pipeline_supported = 0;
static __thread char g_my_name[16] = {};
static __thread int g_name_cached = 0;
static __thread uint32_t g_last_dispatch_counter = 0;
static __thread int g_dispatch_counter_synced = 0;

/* acquire 순서로 안정 게시 상태(유효 magic + 0이 아닌 짝수 generation) 확인 */
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
    /* 슬롯 신원(이름) 재확인 */
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
        /* TT target: Timer Master opens dispatch_counter in an allowed slot. */
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

- **`ttsched_wait_data()` 소비자 규약**:
  1. commit guard를 획득한 동안 extension ABI, 안정 게시, 자기 Task 신원을 확인하고 `dispatch_counter`의 현재값을 동기화한 뒤 guard를 해제한다. 실제 `FUTEX_WAIT`로 잠드는 동안에는 guard를 잡지 않는다.
  2. `dispatch_counter`가 `g_last_dispatch_counter`보다 ahead일 때만 `0`을 반환한다. 비교는 `counter_ahead(current, previous)` modulo 규칙을 사용한다. 이때 ready 자체가 아니라 계층별 scheduler gate가 열렸음이 보장된다.
  3. `FUTEX_WAIT(&dispatch_counter, current)`를 사용한다. 단순 wake 반환·EAGAIN·EINTR는 성공이 아니며, guard를 다시 획득해 실제 카운터 변화와 같은 세대를 확인한다. 게시/종료 control wake는 카운터를 증가시키지 않았으므로 `ESTALE`로 반환한다.
  4. L2 CBS Task는 packed `ready_state` CAS와 `dispatch_counter` 증가·futex wake를 같은 commit guard 안에서 수행한다. L1 TT Task는 Timer Master가 guard를 획득한 뒤 `ready_sequence > dispatch_counter`인지 modulo 비교하고 허용 slot에서 `dispatch_counter`를 1 증가시킨다.

`signal_ready`의 성공은 커밋 구간이 게시자와 완전히 직렬화되었고, `ALL`이면 `ready_state`의 mask reset + sequence 증가가 하나의 64비트 CAS로 완료되었음을 뜻한다. `pipeline_commit_enter()`는 대기하지 않으며, 게시 대기는 게시자 쪽에서만 제한된 시간 수행한다.

---

## 4. L-DD-P0-03 — `Timpani-N` 사전 확인, 안정 게시 및 타이머 규율

### 4.1 비RT 노드 로컬 적합성 확인 (`main.cpp`)

테이블 적용 전에 비RT 경로에서 다음을 확인한다. 실패 시 활성 상태를 바꾸지 않고 거절한다.
- 공통 DAG metadata의 이름/해시, 선행 참조, 간선·용량 상한
- 각 Task의 공통 metadata 실행 계층이 TT entry 또는 CBS entry 중 정확히 하나와 일치
- TT entry의 CPU/slot/timing, CBS entry의 CPU/budget/period/deadline 표현 범위
- Source/Successor 역할과 실행 계층은 서로 독립이며 클래스가 강제로 바뀌지 않았는지 확인
- *(N은 DAG 또는 TT/CBS 스케줄 가능성을 재계산하지 않는다.)*

### 4.2 비RT 공통 SHM 그래프 및 계층 등록

1. SHM 게시자는 TT/CBS entry의 `(workload_id_hash, task_id_hash)` 쌍을 정규 key로 사용해 공통 `pipeline[]` 인덱스를 만든다. Phase 0의 Task name 유일성 제약은 SHM name lookup 호환을 위해 유지한다.
2. 각 pipeline slot의 `workload_id_hash`, `task_id_hash`, `execution_model`, `required_mask`, `out_edges`를 초기화하고 `ready_state=0`, `dispatch_counter=0`을 설정한다.
3. 공통 DAG metadata의 predecessor key를 순회해 `required_mask`와 `out_edges`를 채운다.
4. TT Task는 기존 `TtSlot`/TimerMaster에, CBS Task는 기존 `CbsConfig`/CbsState에 등록한다.
5. 실제 게시 직전 `pipeline_header.publication_guard`에 writer-pending 비트를 설정해 새 Pipeline commit 진입을 차단한다. 이미 시작된 활성 commit은 게시의 상태 기록보다 먼저 끝나야 하며, publisher는 active count가 `0`이 될 때까지 제한된 시간 `FUTEX_WAIT` 또는 bounded yield로 대기한다. 마지막 reader는 guard를 해제할 때 writer-pending을 감지하면 guard futex wake를 수행한다.
6. active count가 `0`이 된 뒤 `magic=0`, 홀수 `generation`, `tasks[]`/`pipeline_header`/`pipeline[]` 갱신, `pipeline_header.magic/version/abi_size` 유효화, 짝수 `generation`, `magic=유효`, writer-pending 해제 순서로 게시한다.
7. 게시 자체는 ready/dispatch 순번을 증가시키지 않는다. 기한 전에 active count가 `0`이 되지 않으면 해당 경계의 게시를 거절하거나 다음 경계로 이연한다. 게시/종료 control wake는 `dispatch_counter`를 변경하지 않는다.

이 순서는 Phase 0의 최초 게시·종료/무효화에서도 `pipeline_commit_enter()`가 상태 변경을 시작할 수 있는 동시 구간을 제거한다. Phase 1의 Pipeline 포함 정상 런타임 갱신은 동일 guard를 경계별 커밋에 재사용하며, Phase 2는 publish 실패·rollback·종료/재시작 시 남은 writer-pending/활성 commit 상태 회수 및 초기화 정책을 추가한다.

### 4.3 RT TT/CBS 실행 어댑터

- **L1 TT Source 또는 일반 TT:** `required_mask == 0`이면 기존 slot 발화와 동일하게 `tasks[i].counter`를 증가시키고 wake한다. Pipeline `dispatch_counter`를 사용하지 않는다.
- **L1 TT Successor:** Timer Master는 slot 시각에 먼저 `update_current_slot()`과 `next_tt_start`를 갱신한 뒤 `pipeline_commit_enter()`를 획득한다. `counter_ahead(ready_sequence(ready_state), dispatch_counter)`가 참일 때만 `dispatch_counter`를 정확히 1 증가시키고 futex wake한 다음 guard를 해제한다. pending ready가 없거나 guard 획득에 실패하면 `tasks[i].counter`와 `dispatch_counter` 모두 증가시키지 않는다.
- **L2 CBS Source 또는 일반 CBS:** 기존 Event release 경로를 유지한다.
- **L2 CBS Successor:** `signal_ready`의 commit guard 안에서 packed `ready_state` CAS로 `ALL`을 확정한 뒤, `dispatch_counter`를 1 증가시키고 futex wake한다. BPF는 wake된 thread가 Runnable이 된 뒤 기존 CBS CPU·budget·Gap 규칙만 적용하며, OI-DD-03이 별도 CBS DSQ나 BPF release API를 추가하지 않는다.

Phase 0 L1 slot에 ready가 늦게 도착하면 해당 slot에서는 wake하지 않고 pending ready를 유지한다. 다음 TT slot에서 ready sequence를 1회 소비한다. 정상 Phase 0 시험은 modulo 비교 결과 pending ready가 최대 1개인 것을 전제로 하며, 2개 이상이면 backlog/slot-miss 시험 전제 이탈로 기록한다. pending 만료, deadline miss 처리, `Activation Window`, `SKIP`는 Phase 1/2에서 결정한다.

| 경로 | 소유 주체 | 허용 | 금지 |
|:---|:---|:---|:---|
| **L1 Source** | Timer Master | `tasks[i].counter` slot wake | Pipeline pending 확인 |
| **L1 Successor adapter** | Timer Master | 해당 TT slot에서 `dispatch_counter++` | slot 전/ready 없음 wake, `Window` 적용 |
| **L2 Successor adapter** | `signal_ready` | `ALL` commit에서 `dispatch_counter++` | TT slot 대기, 별도 release daemon |
| **CBS CPU execution** | `scx_timpani` | Runnable 후 기존 budget/Gap gate | Data-Ready 그래프 계산 |
| **libttsched** | Application API | 원자 commit·dispatch futex | control wake 실행 소비 |
| **BPF** | Kernel scheduler | 기존 TT/CBS DSQ, budget/slot 규칙 | 별도 Data-Driven DSQ |

### 4.4 `scx_timpani` 계층별 집행

- TT Successor는 `dispatch_counter` futex wake 전에는 Runnable이 되지 않는다. Timer Master가 slot을 열어 wake한 뒤 BPF는 기존 `SCHED_TYPE_TT` 경로로 집행한다.
- CBS Successor는 `ALL` commit wake로 Runnable이 되고 기존 `SCHED_TYPE_CBS` 경로에 enqueue된다. `ALL`은 release 조건만 추가하며 BPF의 budget/Gap/TT active 판정을 바꾸지 않는다.
- TT/CBS 공통으로 `DMISS`, budget, trace 판단은 기존 Task 계층 규칙을 사용한다. Data-Ready 자체를 scheduler fault 또는 BPF dispatch queue로 취급하지 않는다.

---

## 5. L-DD-P0-04 — `Timpani-O` 후보 검증 및 결정적 위상 배치 (`global_scheduler.{h,cpp}`)

1. **확정 모델 보호·Task class 해석**: 후보를 먼저 구성해 검증한다. 각 Task는 `TaskInfo.temporal_class`가 있으면 그 명시값을 사용하고 없으면 `SchedInfo.temporal_class`를 상속한다. resolved class가 `predecessors`나 Source/Successor 역할로 바뀌지 않는지 검증한다. 거절 시 확정 모델을 유지한다.
2. **Task 참조 정규화 및 결정적 DAG 검증 (`ValidateAndSortDag`)**:
   - 내부 식별자는 `TaskNameKey = std::pair<std::string, std::string>` `(workload_id, task_id)`로 통일한다. `std::map`과 사전순 `std::set`만 사용한다(`std::unordered_map` 금지).
   - `TaskReference.workload_id`가 빈 문자열이면 현재 `SchedInfo.workload_id`로 채운다. Phase 0에서는 정규화 결과가 현재 workload와 다르면 `InfeasibleReason::InvalidDag`로 거절한다.
   - Task name은 기존 SHM lookup 호환을 위해 노드 단위로 유일해야 하며 길이는 `1..15`바이트 ASCII다. 정규화 Task key 중복, 자기 참조, 미존재 참조, `(fnv1a(workload_id), fnv1a(task_id))` hash 쌍 충돌을 거절한다.
   - 직속 선행 최대 `16`개와 정점별 후행 간선 최대 `8`개를 확인한다. 사전순 ready queue 기반 Kahn's Algorithm 종료 후 `out_topo_rank.size() != task_index.size()`이면 순환 의존성으로 거절한다.
3. **클래스 독립 테이블 생성 (`build_table`)**:
   - `PipelineTaskConfig`는 실행 계층 필드를 갖지 않고 `PipelineTaskKey(workload_id_hash, task_id_hash)`와 선행 key 목록만 갖는다. TT/CBS entry는 resolved class에 따라 별도로 만든다.
   - L1 Task는 입력 timing으로 `TtSlot`을, L2 Task는 입력 timing으로 `CbsConfig`를 만든다. Source/Successor는 이 분기를 결정하지 않는다.
   - O는 DAG 위상 순서를 TT slot 충돌 검사 또는 CBS admission 검사의 tie-breaker로만 사용한다.
4. **Phase 0 배치와 이관**:
   - 같은 workload 안에서 명시/상속 class가 혼합된 Task를 지원하고 기존 TT placement 및 CBS admission/worst-fit을 각각 사용한다.
   - Phase 1은 cross-workload `TaskReference`와 혼합 DAG의 TT Window/CBS Gap·예산 지연, `E2E` 검증, Pipeline 갱신으로 확장한다. `E2E` 필드는 Phase 0 OI-DD-01 protobuf에 포함하지 않는다.

---

## 6. Protobuf 인터페이스 최소 확장안 (`OI-DD-01`, 사전 승인 대상)

> **주의**: 실제 `.proto` 파일 반영은 Project Leader의 명시적 승인 후에만 수행한다(`CON-DD-05`).

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

권고 field 번호 `11`, `12`, `6`은 현재 schema의 빈 번호를 사용한 것이며 실제 반영 전에 인터페이스 승인으로 확정한다. `SchedInfo.temporal_class`는 기존 workload 기본값으로 유지한다. O는 `TaskInfo.temporal_class`의 presence를 확인해야 하므로 단순 enum 대신 `optional`을 사용한다. Phase 0에서 trigger enum의 유효값은 `PIPELINE_TRIGGER_ALL`뿐이다.

TT/CBS entry 자체에는 dependency를 중복하지 않는다. O→N DAG metadata는 `PipelineTaskConfig`에만 두고, N은 key 쌍을 `TtSlot` 또는 `CbsConfig` 중 정확히 하나와 연결한다. node-level SHM 이름 조회는 기존 `1..15`바이트 Task name의 유일성 제약을 Phase 0에도 유지한다.

---

## 7. L-DD-P0-05 — 시험 접점

단위 시험은 실행 계층 추론 없이 DAG를 처리하고, L1/L2 연결을 계층별 gate로 실행하는지 검증한다.

| 시험 | 확인 |
|:---|:---|
| class 해석 | Task 명시 class는 workload 기본값보다 우선하고, 미지정 Task는 기본값을 상속한다. `predecessors` 유무는 class에 영향을 주지 않는다 |
| 참조 정규화/거절 | 빈 `workload_id`는 현재 workload로 해석되고, Phase 0 cross-workload 참조·중복 key·hash-pair 충돌·미존재 참조는 활성화 전에 거절된다 |
| O DAG/배치 | 정규화 key, 위상 순서, TT/CBS 배치가 입력 순서와 무관하며 실행 class는 명시/상속값과 일치한다 |
| N 등록 | `(workload_id_hash, task_id_hash)` 쌍이 정확히 하나의 TT/CBS entry와 연결되고 클래스가 바뀌지 않는다 |
| SHM ABI | 784바이트 legacy object에서 TT API만 동작하고 Pipeline API는 `ENOTSUP`; extension magic/version/size 불일치 `EINVAL`, `fstat()` 범위 초과 접근 없음 |
| 권한 | Pipeline writer의 O_RDWR 실패는 `EACCES`, read-only Successor의 `wait_data`는 성공, `signal_ready`는 `EACCES` |
| 동시성 | 64비트 `ready_state` CAS가 target에서 lock-free이고, 마지막 선행 CAS는 mask reset+sequence 증가를 한 번에 수행하며 concurrent predecessor 경합에 wake 1회 |
| wake 계약 | `dispatch_counter` 증가만 실행 wake로 취급하고, publication/shutdown control wake는 `ESTALE`; `0x7fffffff` 경계에서 modulo 비교가 중복/누락 없이 동작 |
| L1 gate | `ALL` 전 또는 slot 없이 `dispatch_counter` 증가 `0`; ready가 slot 전에 있으면 해당 slot에서 `+1`; 같은 ready의 중복 wake `0`; slot 후 ready는 다음 slot까지 pending |
| L2 gate | packed `ALL` CAS 뒤 즉시 `dispatch_counter+1`과 futex wake `1`; 실제 CPU 실행은 CBS budget/Gap에 따라 지연 가능 |
| CBS BPF 경계 | TT active 또는 budget=0일 때 CBS Task가 즉시 dispatch되지 않지만 release 자체는 유지되고 기존 throttled/replenishment 경로로 집행 |
| 조합 | Source/Successor 역할을 섞어 TT→TT, TT→CBS, CBS→TT, CBS→CBS 각 최소 1회 통과 |

타깃 미실행 시 하드웨어 검증은 미수행으로 기록한다.
