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
| `timpani-o/proto/*.proto` | Task-level 실행 클래스 정보와 클래스 독립 공통 Pipeline dependency metadata | **`OI-DD-01` 승인** |
| `sample-apps/src/libttsched.h/c` | 공통 ready/dispatch 순번, `wait_data`/`signal_ready`, TT/CBS adapter 연결 | **`OI-DD-02` 승인** |
| `timpani-o/src/global_scheduler.*` | DAG 검증 후 각 Task의 기존 TT/CBS 배치를 생성하고 graph meta를 hash에 연결 | 기존 API 보존 |
| `timpani-n/src/main.cpp` | TT/CBS entry와 공통 graph meta를 함께 로컬 확인·등록 | 계층별 가능성 재계산 없음 |
| `timpani-n/src/timer_master.*` | L1 Task는 allowed slot에서 pending ready를 dispatch | L2 release는 하지 않음 |
| `timpani-n/src/bpf/timpani.bpf.c` | 기존 `SCHED_TYPE_TT`/`CBS` dispatch gate 재사용 | 별도 DSQ 없음 |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` 공유 메모리 레이아웃 (`libttsched.h`)

[DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` 및 [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`에 따라 기존 헤더와 `tasks[TIMPANI_MAX_TASKS]`(오프셋 `0..783`)를 바이트 단위로 그대로 보존하고, 그 뒤쪽에 `pipeline[TIMPANI_MAX_TASKS]` 영역을 배치한다.

```c
#define TIMPANI_TTSCHED_MAGIC 0x54494D50U /* "TIMP" */
#define TIMPANI_MAX_TASKS     32
#define TIMPANI_MAX_OUT_EDGES 8

/* 트리거 모드 상수 */
#define TIMPANI_TRIG_ALL 0U /* 모든 직속 선행 태스크 완료 시 wakeup (Phase 0 필수) */
#define TIMPANI_TRIG_ANY 1U /* 하나 이상 직속 선행 완료 시 wakeup (확장 대비 상수) */

/* 선행 완료 비트 영역 (직속 선행 최대 16개) */
#define TRIG_READY_MASK  0x0000FFFFU

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

/* 후방 확장 영역: tasks[i]와 동일한 인덱스 i를 공유하는 Pipeline 슬롯 */
#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    uint32_t data_counter;      /* ALL 충족으로 증가하는 논리적 ready sequence */
    uint32_t dispatch_counter;  /* TT slot 또는 CBS release가 허용한 dispatch sequence */
    uint32_t ready_mask;        /* 원자적 선행 완료 비트마스크 */
    uint32_t required_mask;     /* 필요 선행 비트 (0이면 Source/일반 Task) */
    uint16_t num_out_edges;
    uint16_t trigger_mode;      /* TIMPANI_TRIG_ALL(0) */
    uint16_t execution_model;   /* TT or CBS; source/successor 역할과 독립 */
    uint16_t _reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

struct timpani_ttsched_shm {
    uint32_t magic;      /* 안정 게시 완료 시 TIMPANI_TTSCHED_MAGIC, 게시 중/종료 시 0 */
    uint32_t n_tasks;    /* 점유 슬롯 상한 인덱스 + 1 */
    uint32_t generation; /* 홀수 = 게시 중, 짝수(>0) = 안정 게시 상태 */
    uint32_t _pad;
    struct timpani_task_slot     tasks[TIMPANI_MAX_TASKS];    /* 기존 영역 보존 */
    struct timpani_pipeline_slot pipeline[TIMPANI_MAX_TASKS]; /* Phase 0 후방 확장 */
};
```

---

## 3. L-DD-P0-02 — `libttsched` 동기화 및 안정 게시 확인 (`libttsched.c`)

### 3.1 SHM 매핑 및 공개 API 계약

- `ttsched_init()`은 `shm_open("/timpani_ttsched", O_RDWR, 0666)` 및 `PROT_READ | PROT_WRITE`로 전체 `sizeof(struct timpani_ttsched_shm)` 매핑을 시도한다. 읽기 전용 일반 TT 컨테이너에서 `EACCES`가 발생하면 기존 `O_RDONLY` / `PROT_READ`로 fallback하며 `g_shm_writable = 0`으로 기록한다.
- 공개 API 시그니처 *(사전 승인 대상, `OI-DD-02`)*:
  ```c
  int ttsched_wait_data(void);    /* 정상 Data-Ready wakeup 시 0, 미준비/불안정/철회 시 -1 */
  int ttsched_signal_ready(void); /* 신호 처리 완료 시 0, 미준비/불안정/권한 없음/미발견 시 -1 */
  ```

### 3.2 안정 세대 확인, `ttsched_signal_ready()` 및 `ttsched_wait_data()` 상세 로직

```c
static __thread int g_shm_writable = 0;
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
    /* 슬롯 신원(이름) 재확인 */
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

    /* 간선 스냅샷 복사 후 동일 안정 세대 유지 여부 재확인 */
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
                next_val = 0U; /* wakeup 확정과 동시에 다음 주기를 위해 0으로 소비 */
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
            /* TT 대상: TimerMaster가 허용 slot에서 dispatch_counter를 연다. */
        }
    }
    return 0;
}
```

- **`ttsched_wait_data()` 소비자 규약**:
  1. 안정 게시와 자기 Task 신원을 확인하고 `dispatch_counter`를 동기화한다.
  2. `dispatch_counter`가 `g_last_dispatch_counter`와 달라질 때만 `0`을 반환한다. 이때 ready 자체가 아니라 계층별 scheduler gate가 열렸음이 보장된다.
  3. `FUTEX_WAIT(&dispatch_counter, cur)`를 사용한다. 단순 wake 반환·EAGAIN·EINTR로는 성공하지 않고 실제 순번 변화를 다시 확인한다.
  4. L2 CBS Task는 `ALL` CAS에서 `dispatch_counter`를 연다. L1 TT Task는 TimerMaster가 허용 slot에서 `data_counter > dispatch_counter`를 확인한 뒤 연다.

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

1. SHM 게시자는 모든 TT/CBS Task의 Task hash로 공통 `pipeline[]` 인덱스를 만든다.
2. 각 pipeline slot에 `execution_model`, `required_mask`, `out_edges`, `data_counter`, `dispatch_counter`를 초기화한다.
3. 공통 DAG metadata의 predecessor hash를 순회해 `required_mask`와 `out_edges`를 채운다.
4. TT Task는 기존 `TtSlot`/TimerMaster에, CBS Task는 기존 `CbsConfig`/CbsState에 등록한다.
5. 게시 자체는 ready/dispatch 순번을 증가시키지 않고 안정 세대에 release한다.

### 4.3 RT TT/CBS 실행 어댑터

- **L1 TT Source 또는 일반 TT:** `required_mask == 0`이면 기존 slot 발화와 동일하게 dispatch한다.
- **L1 TT Successor:** slot 시각에 `data_counter > dispatch_counter`일 때만 `dispatch_counter`를 해당 ready로 1회 전진시키고 futex wake한다. ready가 없으면 실행하지 않는다.
- **L2 CBS Source 또는 일반 CBS:** 기존 Event release 경로를 유지한다.
- **L2 CBS Successor:** `ALL` CAS가 ready를 만든 뒤 `dispatch_counter`를 즉시 전진시키고 futex wake한다. BPF는 CBS CPU·예산·Gap 규칙을 적용한다.

| 경로 | 허용 | 금지 |
|:---|:---|:---|
| **비RT 준비** | 공통 graph + TT/CBS entry 등록 | 후보 부분 반영 |
| **TT adapter** | slot에서 pending ready의 dispatch | ready 없는 L1 Successor wake |
| **CBS adapter** | `ALL` ready를 CBS release | TT/CBS timing 우회 |
| **libttsched** | 원자 연산·futex | 제어 wake 실행 소비 |
| **BPF** | 기존 TT/CBS gate, 예산·slot 규칙 | 별도 Data-Driven DSQ |

### 4.4 `scx_timpani` 계층별 집행

- TT Task는 기존 `SCHED_TYPE_TT` 경로를 사용하되, dependent Task는 TT adapter가 dispatch wake를 열기 전에는 runnable이 되지 않는다.
- CBS Task는 기존 `SCHED_TYPE_CBS` 경로를 사용하고, `ALL` ready가 CBS release의 추가 조건이다.
- TT/CBS 공통으로 `DMISS`, budget, trace 판단은 기존 Task 계층 규칙을 사용한다. Data-Ready 자체를 scheduler fault로 오해하지 않는다.

---

## 5. L-DD-P0-04 — `Timpani-O` 후보 검증 및 결정적 위상 배치 (`global_scheduler.{h,cpp}`)

1. **확정 모델 보호·Task class 보존**: 후보를 먼저 구성해 검증한다. O는 `predecessors`로 DAG 역할만 계산하고 Task의 explicit/inherited `TemporalClass`를 그대로 유지한다. 거절 시 확정 모델을 유지한다.
2. **결정적 DAG 검증 및 위상 정렬 (`ValidateAndSortDag`)**:
   - `std::map<std::string, size_t> task_index`, `std::map<std::string, uint32_t> in_degree`, `std::map<std::string, std::vector<std::string>> adj`만 사용한다(`std::unordered_map` 금지).
   - 검사 항목: ① 태스크 이름 길이(`1..15`바이트 ASCII) 및 `fnv1a_hash()` 충돌 여부, ② 자기 참조(`pred == t.task_id`) 또는 미존재 참조(`task_index.find(pred) == end`), ③ 직속 선행 개수 `> 16` 또는 정점별 후행 간선 수 `> 8`, ④ 사전순 `std::set<std::string> ready_queue` 기반 Kahn's Algorithm 종료 후 `out_topo_rank.size() != task_index.size()`(순환 의존성). 위반 시 `InfeasibleReason::InvalidDag`를 반환한다.
3. **클래스 독립 테이블 생성 (`build_table`)**:
   - 모든 Task hash에 공통 `PipelineTaskConfig`를 만든다.
   - L1 Task는 입력 timing으로 `TtSlot`을, L2 Task는 입력 timing으로 `CbsConfig`를 만든다. Source/Successor는 이 분기를 결정하지 않는다.
   - O는 DAG 위상 순서를 TT slot 충돌 검사 또는 CBS admission 검사의 tie-breaker로만 사용한다.
4. **Phase 0 배치**:
   - TT Task는 기존 TT placement, CBS Task는 기존 CBS admission/worst-fit을 사용한다.
   - Phase 1은 혼합 DAG에서 TT Window와 CBS Gap/예산 지연을 경로 단위로 합산한다.

---

## 6. Protobuf 인터페이스 최소 확장안 (`OI-DD-01`, 사전 승인 대상)

> **주의**: 실제 `.proto` 파일 반영은 Project Leader의 명시적 승인 후에만 수행한다(`CON-DD-05`).

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

## 7. L-DD-P0-05 — 시험 접점

단위 시험은 실행 계층 추론 없이 DAG를 처리하고, L1/L2 연결을 계층별 gate로 실행하는지 검증한다.

| 시험 | 확인 |
|:---|:---|
| O DAG/배치 | `predecessors`와 실행 클래스가 독립이며, 위상 순서·TT/CBS 배치가 입력 순서 독립적 |
| N 등록 | 공통 pipeline metadata가 올바른 TT/CBS entry에 연결되고 클래스가 바뀌지 않음 |
| SHM | `ALL` ready 1회, partial/control wake 미실행, ready≠dispatch 구분 |
| L1 Successor | ready가 slot 전이면 slot에서만 dispatch, ready가 없으면 slot dispatch 없음 |
| L2 Successor | `ALL` ready가 기존 CBS release로 연결되고 budget/Gap 준수 |
| 조합 | TT→TT, TT→CBS, CBS→TT, CBS→CBS 각 최소 1회 통과 |

타깃 미실행 시 하드웨어 검증은 미수행으로 기록한다.
