<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-009: Data-Driven Workload — Phase 0 상세 설계

**작성일:** 2026-10-27  
**최종 수정:** 2026-11-25 (Phase 0 PR 리뷰 반영)
**상태:** Draft — Phase 0 설계안, 검토·승인 대기.
008 필드 `11/12/13`은 정합되었으나 `OI-DD-01`/`02`/`03`은 Project Leader
승인과 검증 전이며 구현 완료가 아니다.
**상위 기준:** [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, [SRS-009](SRS-009-data-driven-workload.md), [HLD-009](HLD-009-data-driven-workload.md)  
**설계 결정:** [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01`..`05`, [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`, `D-P0-06`

---

## 1. 설계 경계, 사전 승인 대상 및 기존 접점

본 문서는 Phase 0의 공통 DAG 자료구조, ready/dispatch 분리, TT/CBS 실행 어댑터, 사전 확인과 시험 접점만 상세화한다. Phase 1의 Window/혼합 `E2E` 및 Phase 2의 `SKIP`/복구 상태 기계는 포함하지 않는다.

| 기존 접점 | Phase 0 상세 설계 의도 | 승인·호환 제약 |
|:---|:---|:---|
| `timpani-o/proto/*.proto` | `optional` Task class override(`protoc >= 3.15`), 구조화 `TaskReference`(`task_id` ↔ `TaskInfo.name`), workload/task hash 쌍 공통 DAG metadata(`ExecutionModel.DATA_FLOW = 2` 미사용) | **`OI-DD-01` 승인 대기(008 배정 `11/12/13` 정합 완료)** |
| `sample-apps/src/libttsched.h/c` | versioned Pipeline extension(`40B` 헤더), packed `ready_state`, dispatch-only futex, `void ttsched_init()` 유지·`g_shm_init_errno`·TLS 일괄 리셋·게시 전/후 attach 분리(`g_attached_before_publish`), `release_source`/`current_job_token`/`wait_data`/`signal_ready`, `pipeline_commit_alive`, `atomic_name_equals`, 읽기 전용 seqlock 및 무-VFS RT 검증 | **`OI-DD-02` 조건부 승인안 대기** |
| `timpani-o/src/schedinfo_service.{h,cpp}` | `TaskInfo.temporal_class` explicit-over-inherit 해석, `RegenerateAllSchedules()` 실패 시 기존 워크로드 모델 안전 롤백 | 기존 API 보존 |
| `timpani-o/src/global_scheduler.*` | DAG 검증, CBS-only `hyperperiod_us > 0` 보장, `place_tt_slots()`의 `(topo_rank, deadline_us, ...)` + `min_start` L1→L1 순서 보장 및 graph meta 분리 생성 | 기존 API 보존 |
| `timpani-n/src/main.cpp` | TT/CBS entry와 공통 graph meta 로컬 확인·등록, `ExecutionModel.DATA_FLOW` 거절(`EINVAL`), `TimerMaster::set_schedule_table()` 오버로드로 CBS/Pipeline 메타 전달 | 계층별 가능성 재계산 없음 |
| `timpani-n/src/timer_master.*` | `set_schedule_table()` 오버로드(`CON-DD-06` 보존), `start()` 단일 `base_cycle_start`·Source별 파이프라인 주기 ordinal, L1 Successor 허용 slot에서 `next_tt_start` 갱신 후 `tm_pipeline_commit_enter`로 `pending` 확인 시에만 `update_current_slot()`·`dispatch_counter++`·`FUTEX_WAKE` 수행(`pending == false` 시 생략·`SLOT_NONE` 미덮어쓰기) | L2 release는 하지 않음 |
| `timpani-n/src/bpf/timpani.bpf.c` | 기존 `SCHED_TYPE_TT`/`CBS` dispatch gate 변경 없이 재사용 | 별도 DSQ 없음 |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` 공유 메모리 레이아웃 (`libttsched.h`)

[DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` 및 [DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`에 따라 기존 헤더와 `tasks[TIMPANI_MAX_TASKS]`(오프셋 `0..783`)를 바이트 단위로 그대로 보존한다. 그 뒤에 버전이 있는 Pipeline extension header와 `pipeline[TIMPANI_MAX_TASKS]` 영역을 추가한다.

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
#define TIMPANI_PUB_DRAIN_TIMEOUT_US  5000U /* 정상 게시/갱신 drain 상한: effective_pub_drain_timeout_us(H) = min(5 ms, H/4) */
#define TIMPANI_PUB_MAX_DEFERS        2U    /* Phase 1+ 연속 drain 타임아웃 이연 상한 */
#define TIMPANI_PUB_SHUTDOWN_DRAIN_MS 50U   /* 종료/교체 강제 무효화(Safe Abandonment) 상한 */

#define TIMPANI_TRIG_ALL 0U /* Phase 0 유일한 유효 trigger */
#define TIMPANI_TRIG_ANY 1U /* Phase 1 이후 확장 예약값; Phase 0 게시 금지 */

#define TIMPANI_READY_MASK         0x0000FFFFULL
#define TIMPANI_SOURCE_RELEASE_VALID 0x00010000ULL
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
    /* Timpani-N이 TtSlot/CbsConfig 결합 검증 및 진단용으로 기록·보관한다.
     * Phase 0 libttsched는 노드 내 유일 1..15B ASCII tasks[i].name으로 슬롯을 조회한다. */
    uint64_t workload_id_hash;
    uint64_t task_id_hash;
    uint64_t ready_state;
    /* Successor: [63:32] ready sequence, [31:16] reserved, [15:0] ready mask.
     * Source: [63:32] released activation ordinal, bit 16 = release-valid,
     * [15:0] = 0. */
    uint32_t required_mask;    /* 필요한 선행 비트 (0이면 Source/일반 Task) */
    uint32_t dispatch_counter; /* TT slot 또는 CBS release가 허용한 실행 수, 유일한 futex word */
    uint16_t num_out_edges;
    uint16_t trigger_mode;     /* Phase 0: TIMPANI_TRIG_ALL */
    uint16_t execution_model;  /* TT/CBS; Source/Successor 역할과 독립 */
    uint16_t reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

/* 후방 확장 header (40B) — tasks[]가 끝나는 byte 784에 배치 */
struct timpani_pipeline_header {
    uint32_t magic;              /* TIMPANI_PIPELINE_MAGIC (무효화/종료 시 0) */
    uint16_t abi_version;        /* TIMPANI_PIPELINE_ABI_VERSION */
    uint16_t reserved;
    uint32_t abi_size;           /* 전체 struct timpani_ttsched_shm 크기 (3128B) */
    uint32_t publication_guard;  /* bit31=writer pending, 하위 31bit=활성 write commit 수 */
    uint64_t instance_id;        /* SHM 객체 고유 식별자 (0 불가; 교체·재생성 시 신규 발급) */
    uint64_t origin_epoch_ns;    /* 게시자 기록 진단·Phase 1 epoch 메타데이터 (RT fast-path 미로드, job_token 원천 아님) */
    uint64_t hyperperiod_us;     /* 게시자 검증(> 0) 진단·Phase 1 고정 스케줄 hyperperiod (us; RT fast-path 미로드) */
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

새 client는 비RT 초기화 경로인 `void ttsched_init(void)`에서만 `fstat()`로 SHM 크기를 확인한다. 기존 반환형(`void`)을 유지하되 thread-local `g_shm_init_errno`를 기록하므로, 파일 크기가 기존 784바이트 prefix보다 짧으면 `g_shm_init_errno = EINVAL`로 실패를 기록하고 후속 Pipeline API가 이를 그대로 `EINVAL`로 반환하며 TT 경로도 진입하지 않는다. prefix는 유효하지만 extension 전체(`sizeof(struct timpani_ttsched_shm)`)가 없으면 `g_shm_pipeline_capable = 0`으로 설정해 기존 `ttsched_wait_next_period()`만 계속 사용할 수 있게 한다. 이후 RT critical path(`release_source`, `signal_ready`, `wait_data`, `current_job_token`)에서는 `shm_open`/`fstat`/`close` 같은 VFS/fd 시스템 콜을 일절 호출하지 않고, 순수 사용자 공간 원자 로드(`g_shm_pipeline_capable`, `shm->magic`, 짝수 `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`, `publication_guard`)와 16바이트 원자 이름 비교(`atomic_name_equals`)만으로 ABI·세대·신원·교체/무효화를 검증한다. `origin_epoch_ns`와 `hyperperiod_us`는 게시자(`Timpani-N`)가 게시 전 `hyperperiod_us > 0`을 검증해 40B 헤더에 기록하는 진단·Phase 1 메타데이터이며, RT fast-path에서는 불필요한 추가 로드를 하지 않는다.

live SHM object는 수명 동안 고정 크기다. `Timpani-N`은 full-layout mapping을 게시하기 전에 전체 ABI 크기(`3,128`B)로 생성하고, client가 mapping할 수 있는 동안 크기를 그 아래로 `ftruncate()`해서는 안 된다(no-shrink 불변량). 교체·종료 시에는 `WRITER_PENDING` 설정, active commit drain(최대 `TIMPANI_PUB_SHUTDOWN_DRAIN_MS = 50 ms`), `shm->magic = 0`·`pipeline_header.magic = 0` 무효화, 모든 슬롯(`0..TIMPANI_MAX_TASKS-1`)의 `tasks[i].counter` 및 `pipeline[i].dispatch_counter`에 대한 `FUTEX_WAKE(..., INT_MAX)` 뒤 구 object를 `ftruncate()`하거나 in-place 덮어쓰지 않고 `shm_unlink()`하며, 신규 객체는 `O_CREAT | O_EXCL`과 새 `instance_id`로 생성한다. 구 mapping을 보유한 client는 다음 RT 호출에서 `magic == 0`·`WRITER_PENDING` 또는 `instance_id` 불일치를 원자 로드로 즉시 탐지해 `ESTALE`을 반환하고, 비RT 제어 경로에서 `ttsched_init()`을 재호출해 새 객체로 재연결한다.

`publication_guard`는 extension header 안에서 쓰기 commit(`ttsched_release_source()`, `ttsched_signal_ready()` 및 `Timer Master` L1 어댑터)과 게시 세대 전환 사이의 TOCTOU를 막는다. 게시자는 `TIMPANI_PUB_WRITER_PENDING` 비트를 원자적으로 켜 새 쓰기 commit을 차단하고, 기존 활성 commit이 `0`이 될 때까지 제한된 시간(`effective_pub_drain_timeout_us(H) = min(5 ms, H/4)`, 종료/교체 시 `50 ms`)만 대기한 뒤 `magic = 0`·홀수 `generation`으로 SHM을 갱신한다. 이미 guard에 진입한 in-flight writer는 각 CAS/wake 직전 `WRITER_PENDING`을 제외한 `pipeline_commit_alive()`를 확인하므로 정상 drain 중에도 commit+wake를 온전히 완결한 뒤 `pipeline_commit_exit()`로 drain을 정상 종료한다. 반면 SHM에 쓰지 않는 순수 읽기 소비자 API(`ttsched_wait_data()`, `ttsched_current_job_token()`)는 `publication_guard`에 쓰지 않는 **읽기 전용 seqlock** 경로를 사용하여 `O_RDONLY`(`PROT_READ`) 매핑에서도 `SIGSEGV`나 소비자 크래시에 의한 guard 누수 없이 안전하게 동작한다.

---

## 3. L-DD-P0-02 — `libttsched` 동기화 및 안정 게시 확인 (`libttsched.c`)

### 3.1 SHM 매핑, 배포 권한 모델 및 공개 API 계약

- 기존 공개 API는 유지한다: `void ttsched_init(void)` 시그니처와 기존 TT prefix용 `O_RDONLY` fallback을 보존하고 `ttsched_wait_next_period()`의 시그니처와 의미를 바꾸지 않는다(단, `ttsched_wait_next_period()` 내부의 재매핑/종료 `munmap` 크기는 고정 `sizeof(struct timpani_ttsched_shm)` 대신 `g_shm_map_size`를 사용하고 `g_shm_mapping = {NULL, 0}`을 함께 갱신해 legacy 784B 매핑과 스레드 destructor의 double-`munmap`을 방지한다).
- `void ttsched_init(void)` 시작 시 기존 `g_shm_mapping.addr != NULL`이면 `munmap(g_shm_mapping.addr, g_shm_mapping.size)`를 수행한 뒤 **전체 TLS 캐시를 일괄 리셋**한다(`g_shm`, `g_shm_mapping`, `g_shm_map_size`, `g_shm_pipeline_capable`, `g_shm_writable`, `g_shm_instance_id`, `g_shm_generation`, `g_task_idx`, `g_last_dispatch_counter`, `g_dispatch_counter_synced`, `g_attached_before_publish`, `g_shm_init_errno`). 이후 비RT 경로에서 `shm_open()` → `fstat()`로 실제 객체 크기를 1회 확인한다. `st_size < 784`이면 `close(fd); g_shm_init_errno = EINVAL; errno = EINVAL; return;`으로 실패를 기록한다(`g_shm_mapping.addr == NULL` 보장). `st_size >= 784`이면 `mmap()`으로 매핑하고 **`mmap()` 직후 SHM 파일 디스크립터를 즉시 `close()`** 한다. 매핑 크기가 전체 `sizeof(struct timpani_ttsched_shm)` 이상이면 `g_shm_pipeline_capable = 1`, 구 784B 객체나 짧은 매핑이면 `g_shm_pipeline_capable = 0`으로 설정한다.
- Pipeline-capable `ttsched_init()`은 호출 시점의 SHM 상태에 따라 최초 attach cursor를 구분한다:
  - `pipeline_read_dispatch_snapshot()` 성공(이미 안정 게시됨 + 자기 슬롯 존재) 시 `g_shm_generation = gen`, `g_shm_instance_id = inst`, `g_task_idx = idx`, `g_last_dispatch_counter = cur`, `g_dispatch_counter_synced = 1`, `g_attached_before_publish = 0`으로 스냅샷한다(`cur == 0`이면 token `0` 보존, `cur = N > 0` late attach면 `0..N-1` replay 차단).
  - SHM이 아직 미게시(`magic != TIMP` 또는 홀수/0 `generation`)이거나 자기 슬롯이 아직 없는 상태(실제 `Timpani-N`의 `/proc` 선행 탐색 → SHM 게시 기동 순서)면 `g_shm_generation = 0U`, `g_last_dispatch_counter = 0U`, `g_dispatch_counter_synced = 0`, **`g_attached_before_publish = 1`**로 설정해 최초 유효 세대 진입 시 cursor `0`에서 시작함으로써 token `0` 누락과 다중 홉 `ESTALE` 교착을 방지한다.
- **스레드 종료 시 리소스 해제**: thread-local 매핑 상태(`g_shm`, `g_shm_map_size`)는 프로세스당 1회 `pthread_once`로 등록한 `pthread_key_create()` destructor에 연결해, 워커 스레드 종료 시 자동으로 `munmap(g_shm, g_shm_map_size)`를 수행함으로써 스레드 생성·종료 반복에 따른 VMA/fd 누수를 원천 차단한다.
- RT 경로(`ttsched_release_source()`, `ttsched_signal_ready()`, `ttsched_wait_data()`, `ttsched_current_job_token()`)에서는 `shm_open`/`fstat`/`close`를 호출하지 않는다. 진입 가드에서 `!g_shm`이면 `errno = (g_shm_init_errno != 0) ? g_shm_init_errno : ENOTSUP`, `!g_shm_pipeline_capable`이면 `errno = ENOTSUP`을 즉시 반환해 `< 784B` 초기화 오류(`EINVAL`)와 legacy 784B 매핑(`ENOTSUP`)을 정확히 구분한다. 이어서 순수 원자 로드와 16바이트 원자 이름 비교(`atomic_name_equals`)로 `publication_guard`의 `WRITER_PENDING`, `shm->magic`, 짝수 `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`를 검증한다.
- 실패한 Pipeline 호출은 RT 경로에서 silently remap/reopen하거나 바인딩된 세대(`g_shm_generation != 0U`)와 다른 새 세대를 암묵 채택하지 않는다. `ESTALE`을 받은 caller는 비RT 제어 경로에서 `ttsched_init()`을 다시 호출하며, 재초기화는 기존 매핑을 `munmap()`하고 전체 TLS를 리셋한 뒤 현재 `/timpani_ttsched` 객체를 다시 `shm_open()`/`fstat()`/`mmap()`/`close()`하여 새 세대·신원과 현재 `dispatch_counter`를 attach cursor로 다시 snapshot한다. 과거 token은 replay하지 않는다. 재초기화 전 반복 호출은 안전하게 `ESTALE`을 반환한다.
- **읽기 전용(`O_RDONLY`) vs 쓰기(`O_RDWR`) 권한 및 배포 모델**:
  - Data-Driven 완료 신호를 기록하는 writer(Source, L2 Source 이벤트 어댑터/시험 생성기 및 중계 Successor)는 `O_RDWR` + `PROT_READ|PROT_WRITE`로 매핑하며, 쓰기 권한이 없으면 `g_shm_writable = 0`으로 `O_RDONLY` fallback한다.
  - 읽기 전용 소비자(일반 TT 및 Sink/read-only Successor)는 `O_RDONLY` + `PROT_READ` 매핑으로 `ttsched_wait_data()`와 `ttsched_current_job_token()`을 정상 호출할 수 있으며(읽기 전용 seqlock 사용, `publication_guard` 미기록), `ttsched_signal_ready()` 또는 `ttsched_release_source()` 호출 시에만 `EACCES`를 반환한다.
  - `Timpani-N`만 SHM 생성·`ftruncate()`·`shm_unlink()` 수명 주기를 소유한다. 생산 배포용 `/dev/shm/timpani_ttsched` 권한 모델은 **소유자 UID `timpani-n`(`rw-`) + POSIX ACL/전용 그룹 분리(`g:timpani-pub:rw-`는 신뢰 writer 컨테이너, `g:timpani-sub:r--`는 일반 TT·read-only Successor/Sink 컨테이너, other `---`)** 를 표준으로 한다(ACL 미지원 경량 환경에서는 `0660` + `timpani-pub` 그룹과 컨테이너 마운트/읽기 전용 바인드로 동등한 격리를 적용한다). 기존 PoC의 `0666`은 로컬 회귀 시험 전용으로 한정하며 생산 ABI 계약으로 간주하지 않는다.
  - **Phase 0 trust boundary**: `O_RDWR`/`PROT_WRITE`는 전체 SHM mapping이므로 `timpani-pub` 그룹 구성원은 OS 수준에서 header와 다른 Task slot도 변경할 수 있다. 이 그룹은 상호 신뢰된 cooperative writer만 포함해야 하며, `libttsched`의 자기 slot/세대 검사는 우발적 오용과 corruption 탐지용이지 악의적 writer 격리 또는 memory integrity guarantee가 아니다. 비신뢰 workload는 반드시 `timpani-sub`의 read-only 경로를 사용한다. slot별 capability, per-slot SHM, kernel mediator 기반 강한 격리는 Phase 0 범위가 아니다.
- 공개 API 시그니처 *(조건부 승인안 대기, `OI-DD-02`)*:
  ```c
  void ttsched_init(void);                            /* 기존 시그니처 유지 (실패 원인은 g_shm_init_errno 보존) */
  int ttsched_wait_next_period(void);                 /* 기존 TT API, 변경 없음 */
  int ttsched_release_source(const char *task_name,
                             uint32_t job_token);     /* L2 CBS Source 이벤트 어댑터/생성기 전용 stamp (O_RDWR 필요) */
  int ttsched_current_job_token(uint32_t *job_token); /* L1/L2 Source 공통 활성화 token 취득 (O_RDONLY 가능) */
  int ttsched_wait_data(uint32_t *job_token);         /* 실제 dispatch wake만 0, FIFO 순차 소비 (O_RDONLY 가능) */
  int ttsched_signal_ready(uint32_t job_token);       /* 같은 job의 완료 commit (O_RDWR 필요) */
  ```
- 신규 API는 실패 시 `-1`과 `errno`를 반환한다: `EACCES`(write 권한
  없음), `ENOTSUP`(legacy/짧은 mapping 또는 extension magic은 유효하지만
  미지원 ABI version), `EINVAL`(NULL/잘못된 인자, `< 784B` 초기화 실패 후
  호출, extension magic/ABI size, 호출자 이름·슬롯 신원 불일치,
  Source-role/release-state/L1 대상 `release_source` 오호출 또는
  outgoing-edge metadata 오류), `ESTALE`(종료/교체로 인한
  `magic=0`/`WRITER_PENDING`/`instance_id` 변경, publish/control wake,
  바인딩 세대 불일치 또는 Successor의 활성 ready sequence와 다른 job
  token), `EALREADY`(활성 job token의 같은 선행 비트가 이미 commit됨),
  `EAGAIN`(commit contention 또는 Source release가 아직 stamp되지 않음),
  `ENOENT`(호출자 자신의 Task 슬롯 미존재 또는
  `ttsched_release_source()` 대상 Task 슬롯 미존재)다.
- **Sink 태스크의 `ttsched_signal_ready()` 호출 규약**: `num_out_edges == 0`인 Sink 태스크가 완료 직후 관행적으로 `ttsched_signal_ready(job_token)`를 호출할 경우, 쓰기 권한·SHM ABI/세대·`n_tasks`·자기 슬롯 신원(`atomic_name_equals(tasks[g_task_idx].name, g_my_name)`)을 정상 검증한 뒤 outgoing-edge 변경 없이 `0`(안전한 no-op)을 반환한다. 단, `O_RDONLY` 매핑 Sink는 `signal_ready()`를 호출하지 않으며 호출 시 `EACCES`를 반환한다.
- `ttsched_signal_ready()`는 첫 `ready_state` 변경 전에 제한된 stack plan에 전체 outgoing-edge 목록을 snapshot·검증한다. 검증 대상은 `n_tasks`, `num_out_edges`, `dst_slot_idx`, self/중복 destination, `bit_idx`, `required_mask` 하위 16비트 유효성 및 소속, `trigger_mode`, `execution_model`, 각 destination의 token/중복 bit 상태다. 어느 edge에서라도 실패하면 predecessor mask, dispatch counter, futex wake가 하나도 바뀌지 않은 채 `-1`을 반환하며, CBS dispatch counter와 wake는 모든 edge CAS가 끝나거나 동시 동일 호출에 의해 완료된 뒤에만 수행한다.
- **공통 `job_token` 도메인 및 호출 시점 규약**: Phase 0에서 `job_token`은 현재 `ready_sequence`에 대응하는 32비트 논리 activation ordinal이며 timestamp나 Task-local 호출 횟수가 아니다. L1 Source는 `Timer Master`가 `start()` 시점에 모든 CPU 스레드가 공유하는 단일 `base_cycle_start`를 확정한 뒤 Source별 파이프라인 주기 단위 activation ordinal `k`(`0, 1, 2, ...`, 슬롯 발화마다 `+1`)를 stamp하고, L2 CBS Source는 `O_RDWR`(`timpani-pub`) 권한의 이벤트 어댑터/시험 생성기가 `ttsched_release_source(task_name, k)`(`required_mask == 0 && num_out_edges > 0 && execution_model == TIMPANI_EXEC_CBS` 검증)로 `k`를 stamp한 뒤 기존 이벤트 경로로 Task를 깨운다. 같은 고정 주기 `H` Pipeline의 L1/L2 Source는 **활성화 직후** `ttsched_current_job_token(&token)`을 1회 호출해 이 release snapshot을 읽고 처리 완료 후 `ttsched_signal_ready(token)`에 전달한다(`wake_delay < Source_period` 전제). `origin_epoch_ns`, `hyperperiod_us`, `CLOCK_REALTIME` 또는 앱-local counter로 token을 다시 계산해서는 안 되며, release 표식이 없는 상태에서 `current_job_token()`을 호출하면 상태 변경 없이 `EAGAIN`을 반환한다. Successor는 `ttsched_wait_data(&token)`이 FIFO 순차로 반환한 `token`을 그대로 다음 단계 `ttsched_signal_ready(token)`에 전달한다. Harmonic 주기-token 매핑은 Phase 1에서 정의한다.
- **애플리케이션 소유 payload 계약**:
  - 009/libttsched는 payload를 저장·복사·파싱하지 않으며 payload 위치, 자료 구조, producer/consumer ownership acknowledgement API를 소유하지 않는다.
  - Producer는 `job_token`에 대응하는 payload slot에 최종 내용을 모두 기록하고 필요한 application memory barrier를 완료한 뒤에만 `ttsched_signal_ready(job_token)`을 호출한다.
  - Source/Successor의 ready CAS와 `dispatch_counter`의 release/acquire 순서가 이후 `wait_data(token)` 또는 TT/CBS dispatch 소비에 대한 visibility edge를 형성한다. Consumer는 token 취득 전 payload를 읽어서는 안 된다.
  - Producer는 해당 token을 받아 처리 중인 모든 직속 Successor가 완료됨을 애플리케이션 소유 ack/ownership 메커니즘으로 확인하기 전 동일 payload slot을 overwrite하지 않는다. Phase 0 PoC는 pending `<= 1` 전제를 반영해 최소 2개의 token-addressed slot을 사용한다.
  - `signal_ready()` 거절(`EALREADY`, `ESTALE`, `EINVAL` 등)은 새 payload를 승인하지 않는다. Producer는 거절된 payload가 활성 token의 승인된 payload를 덮어쓰지 않도록 애플리케이션 버퍼를 격리한다.
  - 009는 payload 내용·generation 일치·overwrite를 직접 검증하지 않는다. 이 애플리케이션 계약 위반은 Phase 0 시험 전제 이탈이자 payload correctness 미보장 사례다.

### 3.2 안정 세대 확인, `ttsched_release_source()`, `ttsched_current_job_token()`, `ttsched_signal_ready()` 및 `ttsched_wait_data()` 상세 로직

```c
/* 스레드 종료 시 thread-local SHM 매핑을 자동 해제하는 destructor */
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

/* 참고: void ttsched_init(void)는 시작 시 기존 g_shm_mapping.addr != NULL이면
 * munmap(g_shm_mapping.addr, g_shm_mapping.size) 후 전체 TLS(g_shm,
 * g_shm_mapping, g_shm_map_size, g_shm_pipeline_capable, g_shm_writable,
 * g_shm_instance_id, g_shm_generation, g_task_idx, g_last_dispatch_counter,
 * g_dispatch_counter_synced, g_attached_before_publish, g_shm_init_errno)를
 * 일괄 리셋한다. 비RT 경로에서 shm_open() -> fstat()로 크기를 1회 검증해
 * st_size < TIMPANI_TTSCHED_PREFIX_SIZE이면 close(fd); g_shm_init_errno =
 * EINVAL; errno = EINVAL; return;(g_shm_mapping.addr == NULL 보장 ->
 * double-munmap 방지). 성공 시 mmap() 직후 fd를 즉시 close()하고
 * pthread_once/pthread_setspecific로 스레드 종료 해제를 등록한다.
 * g_shm_map_size >= sizeof(struct timpani_ttsched_shm)이면
 * g_shm_pipeline_capable = 1로 설정하고:
 *  - pipeline_read_dispatch_snapshot() 성공(이미 안정 게시 + 슬롯 존재):
 *    g_shm_generation = gen; g_shm_instance_id = inst; g_task_idx = idx;
 *    g_last_dispatch_counter = cur; g_dispatch_counter_synced = 1;
 *    g_attached_before_publish = 0;
 *  - SHM 미게시(magic != TIMP, 홀수/0 generation) 또는 슬롯 부재(/proc 선행
 *    기동 순서): g_shm_generation = 0U; g_last_dispatch_counter = 0U;
 *    g_dispatch_counter_synced = 0; g_attached_before_publish = 1;
 * 기존 ttsched_wait_next_period()의 munmap 크기도 g_shm_map_size를 사용하고
 * g_shm_mapping = { NULL, 0 }을 함께 갱신한다. */

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

/* RT 경로 무-VFS 순수 원자 검증: ABI·세대·WRITER_PENDING·instance_id 확인 */
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

/* 이미 publication_guard를 보유한 in-flight writer 전용 생사 확인.
 * 정상 drain(WRITER_PENDING=1) 중에는 commit+wake를 완결하고,
 * Safe Abandonment(magic=0 / pipeline_header.magic=0) 또는
 * 세대·instance_id 변경 시에만 중단한다. */
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

/* RT 경로용 16바이트 NUL padding 정확 비교. */
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

/* 쓰기 경로(ttsched_signal_ready) 전용 commit guard 진입 (O_RDWR 필수) */
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
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = ESTALE;
        return -1;
    }
    if ((guard_before & TIMPANI_PUB_ACTIVE_MASK) ==
            TIMPANI_PUB_ACTIVE_MASK) {
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = EAGAIN;
        return -1;
    }

    uint32_t confirmed_gen;
    uint64_t confirmed_inst;
    if (read_stable_pipeline_gen(shm, &confirmed_gen, &confirmed_inst) != 0) {
        int saved_errno = errno;
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = saved_errno;
        return -1;
    }
    if (confirmed_gen != expected_gen || confirmed_inst != expected_inst) {
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
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

/* L2 CBS Pipeline Source 이벤트 어댑터/시험 생성기 전용 stamp (O_RDWR 필요).
 * 기존 이벤트 wake 직전에 Source 슬롯 ready_state에 activation ordinal을 기록한다. */
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
    /* L1 TT Source는 Timer Master가 소유하므로 TIMPANI_EXEC_CBS만 허용 */
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

/* L1/L2 Source 공통 job_token 취득 — release snapshot 읽기 (O_RDONLY 가능, 무-VFS) */
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

/* ttsched_wait_data() 전용 읽기 전용 seqlock 스냅샷 (publication_guard 미기록) */
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

/* Successor 대기 API — O_RDONLY(PROT_READ) 지원, FIFO 순차 소비 */
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

        /* 이미 바인딩된 세대와 다르거나 대기 중 세대가 바뀌면 즉시 ESTALE */
        if ((has_waited && gen != wait_gen) ||
            (g_shm_generation != 0U && gen != g_shm_generation)) {
            errno = ESTALE;
            return -1;
        }

        if (g_shm_instance_id == 0ULL)
            g_shm_instance_id = inst;

        /* 최초 유효 세대 진입: SHM 게시 전 부착(g_attached_before_publish == 1,
         * /proc 선행 기동 순서)이면 cursor 0에서 시작해 token 0을 보존하고,
         * 이미 안정 게시 후 부착이면 현재 cur을 스냅샷해 과거 replay를 차단한다. */
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
            /* FIFO 순차 소비: current - 1U 점프 시 하류 ready_sequence와의
             * 영구 ESTALE 교착을 방지한다. (cur - token > 1은 Phase 0
             * pending<=1 전제 이탈 backlog 경고 대상) */
            g_last_dispatch_counter = token + 1U;
            *job_token = token;
            return 0;
        }

        wait_gen = gen;
        has_waited = 1;
        /* 게시·종료·Safe Abandonment 제어 wake는 magic=0/홀수 gen/WRITER_PENDING/
         * gen 변경을 동반하므로 루프 상단 스냅샷 검증에서 즉시 ESTALE로 반환된다.
         * 반면 세대·유효성 변화가 전혀 없는 spurious wakeup(futex(2))은 실행 0회로
         * 안전하게 재대기한다. */
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
    /* 최초 바인딩(g_shm_generation == 0U)만 허용; 바인딩된 세대와 다르면 즉시 ESTALE */
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
    /* 슬롯 신원(이름) 원자 재확인 (n_tasks 범위 검증 이후 안전 접근) */
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
        /* Sink 태스크(num_out_edges == 0): 신원·SHM 검증 통과 후 안전한 no-op */
        result = 0;
        goto out;
    }

    struct signal_edge_plan plans[TIMPANI_MAX_OUT_EDGES];

    /* 첫 변경 전에 전체 fan-out을 검증하고 preflight한다. */
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

    /* Preflight는 여기서 종료한다. 첫 CAS 이후에는 Phase 0 crash transaction을
     * 보장하지 않는다. 이미 guard를 보유한 in-flight writer는 WRITER_PENDING을
     * 제외한 pipeline_commit_alive()로 정상 drain 중 commit+wake를 완결한다. */
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
                break; /* guard 아래 reset 불가이므로 이미 commit됨. */

            uint32_t current_mask = ready_mask(old_state);
            if ((current_mask & plans[i].bit) != 0U)
                break; /* 동시 동일 호출이 이 bit를 설정했다. */

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

    /* 모든 edge CAS가 완료된 뒤에야 wake를 관측 가능하게 한다. */
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
    /* TT target: Timer Master가 허용 slot에서 dispatch_counter를 연다. */
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

- **`ttsched_wait_data()` 소비자 규약 요약**:
  1. `publication_guard`에 쓰지 않는 **읽기 전용 seqlock**(`pipeline_read_dispatch_snapshot()`)으로 스냅샷 전후 `read_stable_pipeline_gen()`(짝수 `generation > 0`, `magic`, ABI, `instance_id`, `WRITER_PENDING == 0`) 일치와 16바이트 원자 이름 비교(`atomic_name_equals()`, `n_tasks` 선행 검증 포함)를 확인한다. 따라서 `O_RDONLY`(`PROT_READ`) 매핑에서도 `SIGSEGV`나 C11 데이터 경합 없이 동작하며 소비자 크래시(`SIGKILL`)가 `publication_guard`를 영구 점유하지 않는다.
  2. `ttsched_init()`은 SHM이 아직 미게시이거나 슬롯이 없는 정상 기동 경로(`/proc` 탐색 선행)에서 `g_attached_before_publish = 1`을 기록해 최초 유효 세대 진입 시 cursor `0`으로 token `0`을 보존하고, 이미 안정 게시된 SHM의 `cur = N > 0` 시점 attach에서는 `g_last_dispatch_counter = N`을 스냅샷해 `0..N-1`을 replay하지 않는다. `counter_ahead(cur, g_last_dispatch_counter)`가 참일 때 `current - 1U`로 점프하지 않고 **FIFO 순차 소비(`token = g_last_dispatch_counter; g_last_dispatch_counter = token + 1U; *job_token = token`)** 를 수행해 하류 `ready_sequence`와의 영구 `ESTALE` 교착을 방지한다. `cur - token > 1U`이면 Phase 0 정상 수용 조건(`pending <= 1`) 이탈로 관측·경고한다. 재시작/복구 시 하류 재동기화 및 payload 복원은 Phase 2(`REQ-DD-014`) 범위다.
  3. 미진행 상태에서는 `PROT_READ`에서도 커널이 기본 지원하는 `FUTEX_WAIT(&dispatch_counter, cur)`로 대기한다. 복귀 후(`0`/`EAGAIN`/`EINTR`) 루프 상단에서 스냅샷과 세대를 재평가하므로, 게시·종료·Safe Abandonment·세대 변경을 동반한 제어 wake는 즉시 `ESTALE`로 반환되고, 세대·유효성 변화가 전혀 없는 spurious wakeup은 실행 `0`회로 안전하게 재대기한다.
  4. L2 CBS Task는 `ttsched_signal_ready()`의 `pipeline_commit_enter()` guard 안에서 packed `ready_state` CAS와 `dispatch_counter` 증가·futex wake를 수행한다(정상 `WRITER_PENDING=1` drain 중에는 `pipeline_commit_alive()`로 commit+wake 완결). L1 TT Task는 `Timer Master`가 내부 경량 guard(`tm_pipeline_commit_enter()`)로 `ready_sequence > dispatch_counter`를 확인하고 허용 slot에서 `update_current_slot()`·`dispatch_counter` 1 증가·`FUTEX_WAKE`를 수행한다.

`signal_ready`의 성공은 전체 outgoing-edge 목록이 preflight를 통과하고 전달 token이 모든 destination과 일치했으며, 각 선행 비트가 이 호출에서 commit됐거나 동일 동시 호출에 의해 먼저 완료됐음을 뜻한다. CAS 재시도 중 같은 bit가 이미 설정되었거나 destination sequence가 앞선 경우 해당 edge는 수렴 완료로 처리한다. `pipeline_commit_enter()`의 모든 실패 롤백 분기는 `pipeline_commit_exit()`를 호출해 drain 대기 중인 게시자의 Lost Wakeup을 방지한다. publication guard가 정적 metadata·generation을 유지하고 edge별 CAS가 선행 경쟁을 보호한다. 모든 정적/preflight 실패는 첫 CAS 이전에 mutation-free이며, 모든 edge CAS가 끝나기 전에는 CBS `dispatch_counter`나 futex wake가 바뀌지 않는다.

첫 destination CAS가 시작된 뒤 Source process가 종료되면 일부 destination bit만 남을 수 있다. Phase 0은 이를 원자적 fan-out transaction으로 취급하지 않으며 자동 rollback, idempotent retry, quarantine 성공을 보장하지 않는다. 해당 chain run은 실패·시험 전제 이탈로 기록하고, commit marker·generation quarantine·crash recovery는 Phase 2 `REQ-DD-014`에서 설계한다. `pipeline_commit_enter()` 자체는 대기하지 않으며, 게시 대기는 게시자 쪽에서만 제한된 시간 수행한다.

---

## 4. L-DD-P0-03 — `Timpani-N` 사전 확인, 안정 게시 및 타이머 규율

### 4.1 비RT 노드 로컬 적합성 확인 (`main.cpp`)

테이블 적용 전에 비RT 경로에서 **(a) O→N 와이어 입력 검증**과 **(b) SHM 게시 직전 N 내부 빌더 불변량 자가 검증(§4.2 step 3)**을 수행한다. 실패 시 활성 상태를 바꾸지 않고 거절한다.
- **(a) O→N 와이어 입력(`HierarchicalScheduleTable`·`PipelineTaskConfig`) 검증:**
  - `LayerConfig`의 `model == ExecutionModel.DATA_FLOW (2)` 수신 시 Phase 0 미지원으로 즉시 `EINVAL` 거절
  - `hyperperiod_us > 0` 확인 및 `pipeline_tasks`의 `(workload_id_hash, task_id_hash)` 중복·선행/후행 상한·자기 참조·중복 선행·`trigger_mode == PIPELINE_TRIGGER_ALL` 확인
  - 각 Task의 공통 metadata 실행 계층이 TT entry 또는 CBS entry 중 정확히 하나와 일치
  - TT entry의 CPU/slot/timing, CBS entry의 CPU/budget/period/deadline 표현 범위
  - Source/Successor 역할과 실행 계층은 서로 독립이며 클래스가 강제로 바뀌지 않았는지 확인
- **(b) N 내부 빌더 불변량 자가 검증 (§4.2 step 3):** `predecessor_keys`로부터 결정적으로 빌드한 `required_mask`·`out_edges`에 대해 `required_mask` 상위 16비트 0, `dst != src`, incoming-bit OR-union == `required_mask`, `incoming_edges == popcount(required_mask)` bijection을 게시 전 확인한다(단위 시험에서는 빌더 출력에 fault injection으로 검증).
- *(N은 DAG 또는 TT/CBS 스케줄 가능성을 재계산하지 않는다.)*

### 4.2 비RT 공통 SHM 그래프 및 계층 등록

1. SHM 게시자는 TT/CBS entry의 `(workload_id_hash, task_id_hash)` 쌍을 정규 key로 사용해 공통 `pipeline[]` 인덱스를 만들고, 해당 해시 쌍을 `pipeline[i].{workload_id_hash, task_id_hash}`에 기록한다. 이 해시 쌍은 **`Timpani-N` 내부 결합 검증 및 진단 도구 소비용**이며, Phase 0 `libttsched`는 `prctl(PR_GET_NAME)`과 `tasks[i].name`(노드 내 유일 `1..15`바이트 ASCII)으로 슬롯을 조회한다. `write_tasks_and_pipeline_slots()`는 `tasks[i].name` 기록 시 C11 데이터 경합 방지를 위해 `atomic_name_store()`(16바이트 `__ATOMIC_RELAXED` 저장 + 후속 `even_gen` `__ATOMIC_RELEASE`)를 사용한다.
2. 각 pipeline slot의 `workload_id_hash`, `task_id_hash`, `execution_model`, `trigger_mode`, `required_mask`, `out_edges`를 초기화하고 `ready_state=0`, `dispatch_counter=0`을 설정한다. Source의 `ready_state=0`은 release-valid 표식이 없는 상태이며, 최초 실제 release authority가 `make_source_release_state(k)`를 기록할 때 token `k`가 유효해진다. Successor의 `ready_state=0`은 active `ready_sequence == 0`을 뜻한다. 게시자는 게시 전 `hyperperiod_us > 0`을 검증한 뒤 `pipeline_header`에 신규 `instance_id`, `origin_epoch_ns`, `hyperperiod_us`를 진단·Phase 1 메타데이터로 기록하되 이 값들은 `job_token` 계산이나 RT fast-path 검증에 사용하지 않는다.
3. 게시 직전 **N 내부 빌더 불변량 자가 검증**으로 destination-indexed incoming-bit map과 슬롯 메타데이터(`n_tasks`, `dst == src` 자기 참조, `trigger_mode`, `execution_model`, `required_mask` 상위 16비트 오염 여부)를 함께 생성·검증한다.
   - 공통 DAG metadata의 predecessor key를 순회해 Successor의 직속 선행마다 서로 다른 bit position `0..15`를 배정한다. 이 배정으로 `required_mask`를 만들고 각 선행에 대응하는 `out_edge`를 하나 생성한다.
   - 고정 배열 `incoming_masks[TIMPANI_MAX_TASKS]`와 `incoming_edges[TIMPANI_MAX_TASKS]`를 0으로 초기화한다. 전체 슬롯 순회에서 `trigger_mode == TIMPANI_TRIG_ALL`, `execution_model ∈ {TIMPANI_EXEC_TT, TIMPANI_EXEC_CBS}`, `(required_mask & ~TIMPANI_READY_MASK) == 0U`를 확인하고, 전체 outgoing-edge 순회에서 `dst_slot_idx < n_tasks`, `dst_slot_idx != src`(자기 참조 금지), `bit_idx < 16`, destination `required_mask` 소속, Source/destination 간 중복 간선 부재를 확인한다. `incoming_masks[dst]`에 이미 같은 bit가 있으면 중복 incoming-bit assignment로 거절하고, 없으면 bit를 설정하고 `incoming_edges[dst]`를 증가시킨다.
   - 모든 활성 Source 순회 후 각 활성 Pipeline slot에 대해 `incoming_masks[i] == required_mask[i]`와 `incoming_edges[i] == popcount(required_mask[i])`를 요구한다. 따라서 `ALL`의 모든 required bit에는 정확히 하나의 incoming edge가 존재하며 edge 없는 required bit는 없다. bit alias, 여분 required bit, required-mask 밖 edge bit, 상위 16비트 오염은 게시 전에 거절한다. `required_mask == 0`인 Task는 `incoming_edges == 0`이어야 한다.

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

   이 전체 테이블 검사는 N 게시 사전조건이다. RT `signal_ready()`의 Source-local plan만으로는 global bijection을 증명할 수 없으므로 게시 generation 수명 동안 검증된 불변 metadata를 사용한다.
4. TT Task는 기존 `TtSlot`/Timer Master에, CBS Task는 기존 `CbsConfig`/CbsState에 등록한다. **`TimerMaster` SHM 게시 인터페이스(`CON-DD-06` 준수)**: 기존 `void TimerMaster::set_schedule_table(const std::vector<SlotEntry>&, uint64_t, uint64_t)` 시그니처는 삭제/변경하지 않고(내부적으로 빈 `pipeline_entries`를 넘겨 위임), CBS 태스크와 Pipeline 메타를 함께 SHM `tasks[]`/`pipeline[]`에 동일 인덱스로 등록하기 위한 오버로드 `void TimerMaster::set_schedule_table(const std::vector<SlotEntry>& slots, const std::vector<PipelineEntry>& pipeline_entries, uint64_t hyperperiod_us, uint64_t epoch_ns)`를 추가한다. `main.cpp`는 `table.pipeline_tasks_size() > 0`일 때 이 오버로드를 호출하고 비-Pipeline 테이블은 기존 호출을 유지한다.
5. **Drain 상수 및 `publication_guard` 직렬화**:
   실제 게시 직전 `pipeline_header.publication_guard`에 `TIMPANI_PUB_WRITER_PENDING` 비트를 원자적으로 설정해 새 쓰기 commit 진입을 차단하고, 이미 시작된 활성 commit이 `0`이 될 때까지 제한된 시간 동안 `FUTEX_WAIT`로 대기한다. 정상 게시/갱신 drain 상한은 `effective_pub_drain_timeout_us(hyperperiod_us)` 헬퍼로 `min(5 ms, hyperperiod_us / 4)`를 적용한다. 마지막 writer는 `pipeline_commit_exit()`(실패 롤백 분기 포함)에서 `WRITER_PENDING`과 `active == 0`을 감지해 guard futex wake를 수행한다.

   ```c
   static inline uint32_t effective_pub_drain_timeout_us(uint64_t hyperperiod_us)
   {
       uint64_t quarter_hp = hyperperiod_us / 4U;
       if (quarter_hp > 0U && quarter_hp < TIMPANI_PUB_DRAIN_TIMEOUT_US)
           return (uint32_t)quarter_hp;
       return TIMPANI_PUB_DRAIN_TIMEOUT_US;
   }
   ```

   | 상수명 | 값 | 적용 상황 | 타임아웃 초과 시 동작 |
   |:--|:--:|:--|:--|
   | `TIMPANI_PUB_DRAIN_TIMEOUT_US` | **`5,000 µs` (`5 ms`)**<br/>*(실제 적용: `effective_pub_drain_timeout_us(H) = min(5 ms, H/4)`)* | 정상 게시 및 런타임 테이블 갱신 | `WRITER_PENDING` 즉시 해제 후 적용 거절/이연 (**in-place 덮어쓰기 금지**) |
   | `TIMPANI_PUB_MAX_DEFERS` | **`2`회** *(Phase 1+)* | 연속 drain 타임아웃 이연 상한 | 상한 초과 시 갱신 최종 실패(`TableApplied{success=false}`) 보고 |
   | `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` | **`50 ms`** | 데몬 종료, SHM 객체 교체, 기동 시 잔존 SHM 정리 | 아래 **Safe Abandonment** 절차로 강제 무효화 |

6. **필드별 Release-Store 게시 순서 (`publication_guard` 덮어쓰기 금지)**:
   `struct timpani_pipeline_header`를 통째로 복사 대입하면 `WRITER_PENDING` 비트가 유효화 완료 전에 `0`으로 덮여 reader가 미완성 테이블에 진입할 수 있으므로, 반드시 아래 **필드별 `__atomic_store_n(..., __ATOMIC_RELEASE)`** 순서를 준수한다.

   ```c
   /* active_count == 0 확인 완료 및 TIMPANI_PUB_WRITER_PENDING 유지 상태 */
   __atomic_store_n(&shm->magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->generation, odd_gen, __ATOMIC_RELEASE);

   /* tasks[](이름은 atomic_name_store), n_tasks, pipeline[] 갱신 (ready_state=0, dispatch_counter=0) */
   write_tasks_and_pipeline_slots(shm, candidate);

   /* pipeline_header 개별 필드 기록 (publication_guard 미접근) */
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

   /* 짝수 generation -> 상위 magic -> 마지막에 WRITER_PENDING 해제 */
   __atomic_store_n(&shm->generation, even_gen, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->magic, TIMPANI_TTSCHED_MAGIC, __ATOMIC_RELEASE);
   __atomic_and_fetch((uint32_t*)&shm->pipeline_header.publication_guard,
                      ~TIMPANI_PUB_WRITER_PENDING, __ATOMIC_RELEASE);
   ```

7. **종료/교체 시 강제 무효화 절차 (Safe Abandonment)**:
   데몬 종료·SHM 객체 교체·기동 시 잔존 SHM 정리 경로에서 `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` (`50 ms`)가 경과해도 `active_count > 0`(예: writer 프로세스가 `pipeline_commit_enter`와 `exit` 사이에서 `SIGKILL` 종료)이면 잔여 `active_count`를 경고 로그로 남긴 뒤 다음 순서로 강제 무효화한다:
   1. `TIMPANI_PUB_WRITER_PENDING`을 **유지**한 채 `shm->magic = 0`과 `shm->pipeline_header.magic = 0`(`__ATOMIC_RELEASE`)을 기록한다.
   2. 구 SHM 객체에 `ftruncate()`나 `pipeline[]` in-place 덮어쓰기를 **절대 수행하지 않는다**(생존 reader의 `SIGBUS` 및 상태 손상 차단).
   3. 대기 중인 TT·Pipeline 소비자를 모두 깨우기 위해 모든 슬롯(`for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; i++)`)에 대해 `tasks[i].counter`와 `pipeline[i].dispatch_counter` **양쪽 모두** 카운터 미증가 상태로 `FUTEX_WAKE(..., INT_MAX)`를 호출한 뒤, `shm_unlink(TIMPANI_TTSCHED_NAME)`으로 이름 공간만 회수한다. 교체·재기동 시에는 `O_CREAT | O_EXCL`로 새 inode와 새 `instance_id`를 가진 전체 크기 SHM 객체를 생성한다.

   Safe Abandonment는 active writer를 원자적으로 취소하거나 실행 중인 스레드를 중단시키는 연산이 아니다. 구 mapping에 이미 진입한 writer가 50 ms 초과 후 resume될 수 있으므로, writer는 각 destination CAS와 각 wake 직전에 `pipeline_commit_alive()`(`shm->magic`, `pipeline_header.magic`, `generation`, `instance_id` 확인)로 무효화를 재확인하고 탐지되면 `pipeline_commit_exit()` 후 `ESTALE`로 중단한다. 마지막 재확인과 이후 CAS/wake 사이의 짧은 race까지 제거하는 cancellation guarantee는 Phase 0에 없다.

   invalidation+unlink 이후의 late CAS/wake는 **새 SHM object로 전달될 수 없으며**, 구 object를 보유한 consumer도 wake 후 stable-state 재검증에서 `magic=0`/`instance_id` 불일치를 보고 `ESTALE`를 반환한다. 따라서 Phase 0 보장은 "구 mapping의 연산이 무조건 취소됨"이 아니라 "구 object가 재사용되지 않고 late operation이 active generation 실행으로 소비되지 않음"이다. 무효화 전에 `ttsched_wait_data()`가 token을 반환받아 이미 실행 중인 Job은 Safe Abandonment가 취소하지 않는다. 정상 운영 중 publisher 종료/교체 없이 writer가 멈춘 active-count 누수는 자동 Safe Abandonment trigger가 아니며 전제 이탈/Phase 2 recovery 대상이다.

게시 자체는 `ready_state`나 `dispatch_counter`를 증가시키지 않는다. Source release stamp는 게시가 아니라 실제 L1/L2 release 경로에서만 수행한다. 이 순서는 Phase 0의 최초 게시·종료/무효화에서도 `pipeline_commit_enter()`와 읽기 전용 seqlock이 미완성 상태를 관측하는 동시 구간을 제거한다. Phase 1의 Pipeline 포함 정상 런타임 갱신은 동일 guard와 `TIMPANI_PUB_DRAIN_TIMEOUT_US` / `TIMPANI_PUB_MAX_DEFERS`를 경계별 커밋에 재사용하며, Phase 2는 publish 실패·rollback·재시작 시 잔여 상태 초기화 정책을 추가한다.

### 4.3 RT TT/CBS 실행 어댑터

- **`Timer Master` 내부 경량 guard 정의 (`tm_pipeline_commit_enter()` / `pipeline_commit_exit()`):**
  `Timer Master`는 SHM 객체의 유일한 생명주기 소유자이므로 RT 슬롯 루프에서 `shm_open`/`fstat`/`close`나 TLS 변수에 의존하지 않고, `shm` 포인터만으로 동작하는 경량 guard `tm_pipeline_commit_enter(shm, &gen)`을 사용하며 모든 해제 경로는 기존 `pipeline_commit_exit(shm)` 단일 함수로 통일한다.
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
          pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
          return -1;
      }

      uint32_t confirmed_gen = 0U;
      if (read_stable_gen(shm, &confirmed_gen) != 0 ||
          confirmed_gen != expected_gen ||
          __atomic_load_n(&shm->pipeline_header.magic, __ATOMIC_ACQUIRE) !=
              TIMPANI_PIPELINE_MAGIC) {
          pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
          return -1;
      }

      *out_gen = expected_gen;
      return 0;
  }
  ```
- **L1 TT Source 또는 일반 TT:** `TimerMaster::start()`에서 `cpu_threads_`를 스폰하기 **직전에 1회만** `clock_gettime(CLOCK_REALTIME)`으로 모든 CPU 스레드가 공유하는 단일 `base_cycle_start`를 계산해 각 `cpu_thread_loop(..., base_cycle_start)`에 전달함으로써 다중 CPU 간 초기 `k = 0` 위상 어긋남을 원천 차단한다. `required_mask == 0`이면 기존 slot 발화와 동일하게 `update_current_slot()`·`next_tt_start`를 갱신한다. Pipeline Source(`num_out_edges > 0`)는 전역 Hyperperiod 번호가 아니라 **해당 Source 슬롯 발화마다 `0`부터 `+1` 증가하는 파이프라인 주기 단위 ordinal `k = src_ordinal[slot_idx]++`**을 `pipeline[i].ready_state = make_source_release_state(k)`로 release-store한 뒤 `tasks[i].counter` 증가·futex wake를 수행하고 `pipeline_commit_exit(shm)`로 해제한다(혼합 테이블에서 `period < H`로 한 `H` 내 복수 발화할 때도 중복 `k` 없음). Pipeline `dispatch_counter`는 사용하지 않으며, 일반 TT Task는 Source stamp 없이 기존 wake를 유지한다.
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
  stamp 진입이 실패하거나 세대가 바뀌면 `src_ordinal`·`counter` 증가와 wake를 모두 수행하지 않는다.
- **L1 TT Successor (`Timer Master` 경량 guard 및 선행 `pending` 검사):**
  - L1 TT Successor의 슬롯 시각이 도래하면 먼저 후속 L1 TT 슬롯 보호를 위해 **`bpf_loader_.update_next_tt_start(slot.cpu, ...)`를 갱신**한 뒤 `tm_pipeline_commit_enter(shm, &gen)`을 획득해 `pending = counter_ahead(ready_sequence(ready_state), dispatch_counter)`를 확인한다.
  - **`pending == true`인 경우에만** ① `bpf_loader_.update_current_slot(slot.cpu, slot.slot_idx)`로 `current_slot_map[cpu]` 활성화, ② `pipeline[idx].dispatch_counter`를 정확히 `1` 증가(`__ATOMIC_RELEASE`), ③ `FUTEX_WAKE(&pipeline[idx].dispatch_counter, 1)`을 수행한 뒤 `pipeline_commit_exit(shm)`로 guard를 해제한다. (`scx_bpf_kick_cpu()`는 커널 BPF kfunc이므로 userspace `Timer Master`가 직접 호출하지 않으며, `FUTEX_WAKE`로 태스크가 Runnable이 될 때 기존 `timpani.bpf.c`의 `ops.enqueue()`가 `SCX_DSQ_LOCAL_ON | acpu` 삽입 후 커널 내부에서 `scx_bpf_kick_cpu(acpu, SCX_KICK_PREEMPT)`를 호출한다.)
  - **`pending == false`이거나 guard 획득에 실패한 경우**에는 `update_current_slot()`, `tasks[i].counter`/`dispatch_counter` 증가, `FUTEX_WAKE`를 **모두 생략**하고 즉시 guard를 해제한다. **`current_slot_map[cpu]`에 `SLOT_NONE`을 능동 덮어쓰기하지 않는다**(태스크가 Runnable로 전환되지 않으므로 BPF `ops.enqueue()`와 커널 kick이 자동으로 `0`건이며, 불필요한 RT `bpf_map_update_elem` 1회를 절감하고 직전 슬롯에서 overrun 중인 타 TT 태스크의 `ops.stopping()` `deadline_us` 조회를 보존한다). `next_tt_start`는 여전히 후속 L1 슬롯을 보호한다.
- **L2 CBS Source 또는 일반 CBS:** `Timpani-N`에 별도 L2 release 데몬/IPC를 신설하지 않고 기존 이벤트 경로를 유지한다. L2 CBS Pipeline Source(`required_mask == 0 && num_out_edges > 0 && execution_model == TIMPANI_EXEC_CBS`)는 `O_RDWR`(`timpani-pub`) 권한으로 L2 Source를 깨우는 앱 측 이벤트 어댑터/시험 생성기가 기존 이벤트 wake(socket/pipe/eventfd/futex) 직전에 `ttsched_release_source(task_name, k)`를 호출해 activation ordinal `k`를 `pipeline[i].ready_state`에 stamp한다. 이후 깨어난 L2 Source는 L1 Source와 동일하게 `ttsched_current_job_token(&token)` → `ttsched_signal_ready(token)`을 호출한다. Phase 0 혼합 fan-in(`CAM(L1) + LIDAR(L2) → FUSION`)에서는 게시 직후 공통 시작 위상에서 양쪽 모두 `k = 0, 1, 2, ...`을 순차 발행한다.
- **L2 CBS Successor:** `signal_ready`의 commit guard 안에서 packed `ready_state` CAS로 해당 `ALL`을 확정한다. 모든 outgoing-edge CAS가 완료된 뒤 `dispatch_counter`를 1 증가시키고 해당 futex wake를 수행한다. BPF는 wake된 thread가 Runnable이 된 뒤 기존 CBS CPU·budget·Gap 규칙만 적용하며, OI-DD-03이 별도 CBS DSQ나 BPF release API를 추가하지 않는다.

Phase 0 L1 slot에 ready가 늦게 도착하면 해당 slot에서는 `update_current_slot()` 및 `FUTEX_WAKE`를 생략하고 pending ready를 유지한다. 다음 TT slot에서 ready sequence를 1회 소비한다. 정상 Phase 0 시험은 modulo 비교 결과 pending ready가 최대 1개(`wake_delay < Source_period`)인 것을 전제로 하며, 2개 이상이면 backlog/slot-miss 시험 전제 이탈로 기록한다. pending 만료, deadline miss 처리, `Activation Window`, `SKIP`는 Phase 1/2에서 결정한다.

| 경로 | 소유 주체 | 허용 | 금지 |
|:---|:---|:---|:---|
| **L1 Source** | Timer Master | 단일 `base_cycle_start` 확정, source `ready_state`에 task별 ordinal `k` stamp 후 `tasks[i].counter` slot wake | 스레드별 개별 catch-up, wall-clock token 계산, Pipeline pending 확인 |
| **L2 Source** | 앱 측 이벤트 어댑터 / 시험 생성기 (`O_RDWR`) | 기존 이벤트 wake 직전 `ttsched_release_source(task_name, k)` 호출(`TIMPANI_EXEC_CBS` 전용) | L1 Source 대상 호출(`EINVAL`), 별도 Timpani-N L2 데몬/IPC, wall-clock token 계산 |
| **L1 Successor adapter** | Timer Master | `next_tt_start` 갱신 → `tm_pipeline_commit_enter`로 `pending` 확인 시 `update_current_slot()`·`dispatch_counter++`·`FUTEX_WAKE` → `pipeline_commit_exit` | `pending == false` 시 `update_current_slot()`·wake·`SLOT_NONE` 능동 덮어쓰기, userspace `scx_bpf_kick_cpu` 호출, RT VFS 호출, `Window` 적용 |
| **L2 Successor adapter** | `signal_ready` | 전체 outgoing-edge CAS 집합에서 `ALL` 형성 후 `dispatch_counter++`·`FUTEX_WAKE` | CAS 전 wake, TT slot 대기, 별도 release daemon |
| **CBS CPU execution** | `scx_timpani` | Runnable 후 기존 budget/Gap gate | Data-Ready 그래프 계산 |
| **libttsched** | Application API | 무-VFS 원자 commit(`pipeline_commit_alive`)·16B 원자 이름 비교·읽기 전용 seqlock·dispatch futex | RT `shm_open`/`fstat`, `O_RDONLY` 쓰기, 세대 불일치 암묵 채택, control wake 실행 소비 |
| **BPF** | Kernel scheduler | 기존 TT/CBS DSQ(`ops.enqueue()` 내 `scx_bpf_kick_cpu`), budget/slot 규칙 변경 없이 재사용 | 별도 Data-Driven DSQ |

### 4.4 `scx_timpani` 계층별 집행

- TT Successor는 `dispatch_counter` futex wake 전에는 Runnable이 되지 않는다. Timer Master가 `pending ready`를 확인해 `update_current_slot()`과 `FUTEX_WAKE`를 수행하면 커널이 기존 `timpani.bpf.c` `ops.enqueue()`(`SCX_DSQ_LOCAL_ON | acpu` + `scx_bpf_kick_cpu(acpu, SCX_KICK_PREEMPT)`) 및 `SCHED_TYPE_TT` 경로로 집행한다.
- CBS Successor는 `ALL` commit wake로 Runnable이 되고 기존 `SCHED_TYPE_CBS` 경로에 enqueue된다. `ALL`은 release 조건만 추가하며 BPF의 budget/Gap/TT active 판정을 바꾸지 않는다.
- TT/CBS 공통으로 `DMISS`, budget, trace 판단은 기존 Task 계층 규칙을 사용한다. Data-Ready 자체를 scheduler fault 또는 BPF dispatch queue로 취급하지 않는다.

---

## 5. L-DD-P0-04 — `Timpani-O` 후보 검증 및 결정적 위상 배치 (`schedinfo_service.{h,cpp}`, `global_scheduler.{h,cpp}`)

1. **확정 모델 보호·Task class 해석 (`schedinfo_service.{h,cpp}`)**:
   - `ConvertToClassifiedTasks()`에서 `grpc_task.has_temporal_class()`이면 그 명시값을 사용하고 미지정이면 `request->temporal_class()`를 상속해 `ct.mechanism`을 결정한다. resolved class가 `predecessors`나 Source/Successor 역할로 바뀌지 않는지 검증한다.
   - `AddSchedInfo()`에서 `workload_tasks_[wid]` 갱신 전에 기존 엔트리가 존재하면 백업(`auto prev_it = workload_tasks_.find(wid);`)하고, `RegenerateAllSchedules()` 실패 시 백업이 있으면 복원(`workload_tasks_[wid] = std::move(prev_entry);`), 없으면 `erase(wid)`로 롤백해 기존 확정 모델을 손상 없이 유지한다.
2. **Task 참조 정규화 및 결정적 DAG 검증 (`ValidateAndSortDag`)**:
   - 내부 식별자는 `TaskNameKey = std::pair<std::string, std::string>` `(workload_id, task_id)`로 통일한다. `TaskReference.task_id`는 `TaskInfo.name`(field 1)과 1:1 정확 일치로 해석한다. `std::map`과 사전순 `std::set`만 사용한다(`std::unordered_map` 금지).
   - `TaskReference.workload_id`가 빈 문자열이면 현재 `SchedInfo.workload_id`로 채운다. Phase 0에서는 정규화 결과가 현재 workload와 다르면 `InfeasibleReason::InvalidDag`로 거절한다.
   - Task name(`task_id`)은 기존 SHM lookup 호환을 위해 노드 단위로 유일해야 하며 길이는 `1..15`바이트 ASCII다(`REQ-DD-001`, `ASM-DD-01`). 정규화 Task key 중복, 노드 내 `task_id` 중복/길이 위반, 자기 참조, 미존재 참조, **동일 Task의 `predecessors` 배열 내 중복 선행 선언(예: `[CAM, CAM]` — 암묵적 중복 제거 금지)**, `(fnv1a(workload_id), fnv1a(task_id))` hash 쌍 충돌을 모두 `InfeasibleReason::InvalidDag`로 거절한다.
   - 직속 선행 최대 `16`개와 정점별 후행 간선 최대 `8`개를 확인한다. 사전순 ready queue 기반 Kahn's Algorithm 종료 후 `out_topo_rank.size() != task_index.size()`이면 순환 의존성으로 거절한다.
3. **클래스 독립 테이블 생성 및 L1→L1·CBS-only 배치 보정 (`build_table`, `place_tt_slots`)**:
   - `PipelineTaskConfig`는 실행 계층 필드를 갖지 않고 `PipelineTaskKey(workload_id_hash, task_id_hash)`와 선행 key 목록만 갖는다. 각 Successor의 정규화 선행 key를 결정적으로 정렬하고 오름차순으로 서로 다른 bit position을 배정한다. 선행 수가 `n`이면 생성 `required_mask`는 정확히 `(1U << n) - 1U`이고 `n == 0`이면 `0`이다. TT/CBS entry는 resolved class에 따라 별도로 만들며 기존 `ExecutionModel.DATA_FLOW = 2`는 발행하지 않는다.
   - **CBS-only `hyperperiod_us > 0` 보장:** `all_periods.empty()`이고 `!cbs_tasks.empty()`이면, `cbs_tasks`의 양수 `period_us`들로 `calculate_hyperperiod()`를 계산해 `global_hp`로 사용한다(모든 CBS `period_us == 0`이면 `InfeasibleReason::InvalidDag` 거절). TT 태스크가 1개라도 있으면 기존 동작 불변.
   - **L1 → L1 슬롯 오프셋 순서 보장 (`place_tt_slots()`):** 정렬 키를 `(topo_rank, deadline_us, workload_id, task_id)`로 적용한다(비-DAG 태스크는 모두 `topo_rank == 0`이므로 기존 DM 정렬 + 결정적 tie-break로 회귀 `0`). 각 L1 태스크 배치 시 `min_start = std::max(frame_start, max_placed_l1_pred_end)`부터 `find_free_slot(min_start, task.wcet_us, frame_end)`를 탐색하고, 배치 완료 후 모든 `L1(pred) → L1(succ)` 간선에 대해 `offset(succ) >= offset(pred) + duration(pred)`를 검증해 위반 시 `InfeasibleReason::TtSlotConflict`로 사전 거절한다.
   - L1 Task는 입력 timing으로 `TtSlot`을, L2 Task는 입력 timing으로 `CbsConfig`를 만든다. Source/Successor는 이 분기를 결정하지 않는다. 배정한 predecessor/destination 쌍마다 outgoing edge를 하나 생성하고, 후보를 반환하기 전 §4.2와 같은 incoming-mask exact equality 검사를 실행한다.
4. **Phase 0 배치와 이관**:
   - 같은 workload 안에서 명시/상속 class가 혼합된 Task를 지원하고 기존 TT placement(위 L1→L1 순서 보정 포함) 및 CBS admission/worst-fit을 각각 사용한다.
   - Phase 1은 cross-workload `TaskReference`(서로 다른 workload 간 `task_id` 충돌 해소 규칙 포함, `OI-DD-06`)와 혼합 DAG의 TT Window/CBS Gap·예산 지연, `E2E` 검증, Pipeline 갱신으로 확장한다. `E2E` 필드는 Phase 0 OI-DD-01 protobuf에 포함하지 않는다.

---

## 6. Protobuf 인터페이스 확장안 (`OI-DD-01`, 008 필드 배정 정합 완료)

> **주의**: 008이 정한 `min_inter_arrival_us = 11`,
> `temporal_class = 12`, `predecessors = 13` 배정을 보존한다. 이 안은
> `CON-DD-05`에 따른 Project Leader 승인 전 실제 `.proto`에 반영하지
> 않는다. `proto3 optional` 사용을 위해 빌드 환경은
> **`protoc >= 3.15`** 를 요구한다.

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
  // 008 Event-Driven 확장이 사용하는 field 11.
  uint32 min_inter_arrival_us = 11;
  // 008이 Task 단위 override용으로 예약한 field 12.
  // 명시적 0은 L1 선택; 필드 부재 때만 상속.
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

`TaskInfo` 필드 배정은 `11`/`12`/`13`으로 제안한다. field 11은 008의
`min_inter_arrival_us`, field 12는 008이 `temporal_class`용으로 예약한 위치,
field 13은 `predecessors`로 사용한다.
`HierarchicalScheduleTable.pipeline_tasks = 6` 배정은 유지한다. 이 정합은
wire 번호 충돌을 해소하며 Task 단위 class override의 Phase 0 도입 승인이나 실제
schema 반영 승인을 뜻하지 않는다.

`TaskReference.task_id`는 `TaskInfo.name`(field 1)과 1:1 정확 일치로
해석한다. `SchedInfo.temporal_class`는 기존 workload 기본값으로 유지한다.
O는 `TaskInfo.temporal_class`의 presence를 확인해야 하므로 단순 enum
대신 `proto3 optional`(`protoc >= 3.15`)을 사용한다. 제안하는 presence 규칙은
008 P2 placeholder `0 = inherit`를 대체한다. 즉 명시적
`TEMPORAL_PERIODIC = 0`은 L1을 선택하고 필드가 없을 때만 상속하며, 이 의미
변경은 `CON-DD-05` 승인 대상이다.

Phase 0에서 trigger enum의 유효값은 `PIPELINE_TRIGGER_ALL`뿐이다.
`node_control.proto`에 기존 정의된 `ExecutionModel.DATA_FLOW = 2`는 wire
호환을 위해 enum에 유지하되 Phase 0에서는 미사용한다(O 미발행, N 수신 시
`EINVAL` 거절).

TT/CBS entry 자체에는 dependency를 중복하지 않는다. O→N DAG metadata는 `PipelineTaskConfig`에만 두고, N은 key 쌍을 `TtSlot` 또는 `CbsConfig` 중 정확히 하나와 연결한다. node-level SHM 이름 조회는 `REQ-DD-001` 및 `ASM-DD-01`의 `1..15`바이트 ASCII `task_id` 노드 유일성 제약을 Phase 0에도 유지한다.

---

## 7. L-DD-P0-05 — 시험 접점

단위 시험은 실행 계층 추론 없이 DAG를 처리하고, L1/L2 연결을 계층별 gate로 실행하는지 검증한다.

| 시험 | 확인 |
|:---|:---|
| class 해석 및 롤백 | `SchedInfoServiceImpl`에서 Task 명시 class는 workload 기본값보다 우선하고, 미지정 Task는 기본값을 상속한다(`predecessors` 유무는 class에 영향 없음). `RegenerateAllSchedules()` 실패 시 기존 확정 워크로드 엔트리가 원복·보존된다 |
| 참조 정규화/거절 | `TaskReference.task_id` ↔ `TaskInfo.name` 1:1 매핑, 빈 `workload_id`는 현재 workload로 해석되고, Phase 0 cross-workload 참조·중복 key·노드 내 `task_id` 중복/`1..15`B ASCII 위반·동일 Task `predecessors` 중복(`[CAM, CAM]`)·hash-pair 충돌·미존재 참조는 활성화 전에 거절된다 |
| O DAG/배치 | 정규화 key, 위상 순서, TT/CBS 배치가 입력 순서와 무관하며 실행 class는 명시/상속값과 일치한다. `L1 → L1`은 `(topo_rank, deadline_us, workload_id, task_id)` + `min_start`로 `offset(succ) >= offset(pred) + duration(pred)`를 보장(비-DAG 회귀 `0`)하고, CBS-only 체인은 CBS 주기 LCM으로 `hyperperiod_us > 0`을 생성한다 |
| N 와이어 등록 | `(workload_id_hash, task_id_hash)` 쌍이 정확히 하나의 TT/CBS entry와 연결되고 클래스가 바뀌지 않으며, `ExecutionModel.DATA_FLOW = 2` 수신 시 `EINVAL`로 거절한다. `TimerMaster::set_schedule_table()` 오버로드로 CBS/Pipeline 엔트리가 `tasks[]`/`pipeline[]`에 동일 인덱스로 등록된다 |
| N 내부 빌더 bijection (fault-injection) | 게시 직전 N 내부 빌더 출력에 대해 선행 2개 Successor의 bit `0`, `1` 및 `required_mask=0b11` 정상 후보는 승인한다. 별도 fault-injection 단위 시험으로 `n_tasks` 범위, `dst == src`, 두 incoming edge가 모두 bit `0`, 세 번째 edge 없이 mask `0b111`, mask 밖 edge bit, 상위 16비트 오염(`0x10003`)을 주입해 거절 및 committed candidate/active SHM 불변을 확인한다 |
| SHM ABI 및 초기화 | 기존 `void ttsched_init()` 시그니처를 유지하며 비RT `fstat() < 784B` 시 `g_shm_init_errno = EINVAL`로 후속 Pipeline API가 `EINVAL`을 반환하고 재초기화 실패 시 `g_shm_mapping = {NULL, 0}`으로 double-`munmap`이 없다. 784B legacy/짧은 매핑에서는 TT API만 동작(`ttsched_wait_next_period()` `g_shm_map_size` `munmap`)하고 Pipeline API는 extension 접근 없이 `ENOTSUP`; magic/ABI-size 불일치 `EINVAL`, magic이 유효한 미지원 version은 `ENOTSUP`; `mmap()` 직후 fd 즉시 close 및 스레드 종료 시 `pthread_key` destructor로 `munmap` 누수 `0`건 확인 |
| Task 이름 정확 일치 | `task`와 `task2`라는 서로 다른 슬롯이 있을 때 `find_task_slot("task")`는 `task`만 선택하고, `find_task_slot("task3")`는 `task`/`task2` prefix에 묶이지 않고 `-1`을 반환한다. 기존에 묶인 슬롯 이름이 다른 유효 이름으로 바뀌면 `atomic_name_equals()` 재검증은 `EINVAL`을 반환한다. 16바이트 안에 NUL이 없거나 빈 이름이거나 종료 NUL 뒤에 0이 아닌 바이트가 있는 슬롯은 malformed로 간주해 유효 expected name과 일치하지 않는다 |
| 무-VFS 교체/종료 감지 | RT 경로(`release_source`, `signal_ready`, `wait_data`, `current_job_token`, `Timer Master`)에서 `shm_open`/`fstat`/`close` 호출 `0`회 및 16바이트 원자 이름 비교(`atomic_name_equals`/`atomic_name_store`, TSAN 경합 `0`건) 확인. 동일 SHM 내 세대 전환(`g → g+2`) 시 암묵 채택 없이 `signal_ready`/`wait_data`/`current_job_token`/`release_source` 모두 즉시 `ESTALE`을 반환한다. Safe Abandonment(`WRITER_PENDING` 유지 + `shm->magic=0` + `pipeline_header.magic=0` + `tasks[]`/`pipeline[]` `FUTEX_WAKE(INT_MAX)` + `shm_unlink` + `O_CREAT|O_EXCL` 신규 `instance_id`) 또는 `instance_id` 변경 시 기존 매핑의 RT 호출은 새 `pipeline[]`에 접근하거나 실행하지 않고 `ESTALE`을 반환하며, 비RT `ttsched_init()` 재초기화 후 새 객체에 정상 연결됨을 확인한다. writer를 CAS/wake 직전에 50 ms 이상 pause하고 old object가 abandon된 뒤 resume시키면 신 object 변경 `0`, old consumer 실행 `0`, old API `ESTALE`이어야 한다. 마지막 재검증 직후 pause/resume race에서 old object late CAS/wake 자체는 허용하되 active generation에 영향을 주지 않는다 |
| 권한 및 읽기 전용 seqlock | Pipeline writer의 `O_RDWR` 실패는 `EACCES`, `O_RDONLY`(`PROT_READ`) Successor의 `ttsched_wait_data()` 및 `ttsched_current_job_token()`은 `publication_guard` 쓰기·`SIGSEGV` 없이 성공, `O_RDONLY`의 `ttsched_signal_ready()`·`ttsched_release_source()`는 `EACCES`. `O_RDWR` Sink(`num_out_edges == 0`)의 `signal_ready()`는 신원 검증 후 no-op `0` 반환. 배포 manifest에서 `timpani-pub` member가 신뢰 writer 목록과 일치하고 비신뢰 workload가 RO group에만 속하는지 검증한다. RW writer의 다른 slot/header 임의 변경 격리는 Phase 0 보장으로 시험하지 않는다 |
| 공통 `job_token` 취득 | L1 Source(`Timer Master` 단일 `base_cycle_start`, Source별 `src_ordinal++`) 또는 L2 CBS Source(`ttsched_release_source()`, `TIMPANI_EXEC_CBS` 강제·L1 대상 호출 시 `EINVAL` 거절)가 `make_source_release_state(k)`를 stamp한 경우에만 `ttsched_current_job_token()`이 `k`를 반환한다. release-invalid 상태는 `EAGAIN`, Source가 아닌 slot이나 손상 state는 `EINVAL`, 세대 변경 중에는 `ESTALE`이다. `wake_delay < Source_period` 범위에서 앱 wake를 Hyperperiod 경계 뒤로 미루거나(`offset=80ms` → `105ms`) `CLOCK_REALTIME`을 forward/backward로 변경해도 반환 token과 downstream `ready_sequence`는 변하지 않는다. 32비트 activation ordinal의 `0xFFFFFFFF → 0` 경계는 wrap 비교 규칙과 동일하게 Source/Successor 모두 같은 cycle에 매핑되어야 한다 |
| fan-out 사전검증 | 다중 edge Source에서 `n_tasks` 경계 오염 및 뒤쪽 edge 손상(`dst_idx`, `bit_idx`, `required_mask` 상위비트/소속, `trigger_mode`, `execution_model`, 중복 destination, `num_out_edges` overflow)시켜 `signal_ready`를 호출한다. `-1`/`EINVAL`, 첫 CAS 전 전체 fan-out의 mask·sequence·dispatch counter·futex wake 변경 `0`건이어야 한다. 뒤쪽 destination의 preflight token을 `ESTALE` 또는 `EALREADY`로 만든 경우에도 앞쪽 유효 destination은 불변이다. 유효 fan-out에서는 모든 CAS가 끝난 뒤 첫 CBS wake가 발생한다. 별도 failpoint로 destination CAS 사이·CAS 직후·첫 wake 전후 Source를 종료하면 Phase 0가 rollback/retry 성공을 보장하지 않으며 partial state를 관측·기록하고 chain run을 실패 처리한다 |
| payload visibility/retention | Producer가 token-addressed payload에 generation marker와 pattern을 기록한 뒤 `signal_ready()`를 호출하면 `wait_data(token)` 이후 Consumer가 동일 token/generation/pattern을 읽는다. `signal_ready()` 실패 시 해당 payload는 새 dispatch로 승인되지 않으며 활성 token storage는 덮어쓰이지 않는다. Phase 0 pending `<= 1`에서 최소 2-slot PoC를 사용해 Consumer 처리 중 Producer가 승인된 slot을 overwrite하지 않음을 반복 검증한다. payload 위치·ack·최종 reuse 시각은 애플리케이션 계약이며 TIMPANI API가 소유하지 않는다 |
| 동시성 및 drain/Lost Wakeup | 64비트 `ready_state` CAS가 target에서 lock-free이고, 마지막 선행 CAS는 mask reset+sequence 증가를 한 번에 수행하며 concurrent predecessor 경합에 wake 1회다. 동일 동시 `signal_ready`가 preflight된 bit를 설정하거나 sequence를 증가시키면 중복 wake 없이 수렴 완료(`0`)로 본다. writer가 `ALL` CAS 완료 직후 정상 `WRITER_PENDING=1` drain이 시작될 때 `pipeline_commit_alive()`로 `dispatch_counter++`와 `FUTEX_WAKE`를 끝까지 완결하고 `pipeline_commit_exit()`로 drain을 정상 종료한다(Lost Wakeup·부분 fan-out `0`건). `pipeline_commit_enter()` 및 `tm_pipeline_commit_enter()` 실패 롤백 시에도 `pipeline_commit_exit()`가 drain 대기 게시자를 깨워 Lost Wakeup이 `0`건이며, `effective_pub_drain_timeout_us(H) = min(5 ms, H/4)` 정상 이연 및 `50 ms` 종료 Safe Abandonment가 동작함을 검증한다 |
| 주기 신원 및 FIFO 소비 | 활성 token `k`에서 A가 bit를 설정한 뒤 중복 `(A,k)`는 `EALREADY`, `A(k+1)`/`B(k+1)`은 `ESTALE`이며 mask/sequence/dispatch는 불변이다. preflight 통과 후 CAS 경합 중 동일 동시 호출로 bit가 설정되거나 sequence가 전진한 경우는 수렴 완료(`0`)이며, sequence 증가 이후 늦게 도착한 이전 token 호출은 preflight에서 `ESTALE`로 거절한다. SHM 미게시 시점 부착(`g_attached_before_publish=1`, `/proc` 선행 기동 순서) 또는 최초 dispatch 전 부착(`cur=0`)은 token `0`을 FIFO로 받고, 이미 게시된 `dispatch_counter=N > 0` 이후 단일 노드 late attach(`init cursor=N`)는 token `0..N-1`을 replay하지 않고 다음 dispatch만 기다린다(다중 홉 하류 `ready_sequence` 재동기화는 Phase 2 `REQ-DD-014`). `cur - cursor > 1`이면 backlog 전제 이탈로 관측한다 |
| wake 계약 | `dispatch_counter` 증가만 실행 wake로 취급한다. 게시/종료/세대 변경을 동반한 control wake는 `ESTALE`을 반환하고, 상태·세대 불변의 spurious wakeup(`FUTEX_WAIT == 0`)은 실행 `0`회로 비RT 재매핑 없이 안전하게 재대기한다. `0x7fffffff` 경계에서 modulo 비교가 중복/누락 없이 동작 |
| L1 gate | `ALL` 전 또는 `pending ready == false`일 때 `update_current_slot()`·`dispatch_counter++`·`FUTEX_WAKE` 모두 생략(`SLOT_NONE` 능동 덮어쓰기 없음, Runnable 전환·BPF `ops.enqueue()`·커널 kick `0`건, `next_tt_start`는 갱신); ready가 slot 전에 있으면 해당 slot에서 `update_current_slot()` 활성화 및 `dispatch_counter` `+1`·`FUTEX_WAKE`; 같은 ready의 중복 wake `0`; slot 후 ready는 다음 slot까지 pending |
| L2 gate | packed `ALL` CAS 뒤 즉시 `dispatch_counter+1`과 futex wake `1`; 실제 CPU 실행은 CBS budget/Gap에 따라 지연 가능 |
| CBS BPF 경계 | TT active 또는 budget=0일 때 CBS Task가 즉시 dispatch되지 않지만 release 자체는 유지되고 기존 throttled/replenishment 경로로 집행 |
| 조합 | Source/Successor 역할을 섞어 TT→TT, TT→CBS, CBS→TT, CBS→CBS 각 최소 1회 통과 |

SRS-009 §5.1의 boundary matrix는 위 시험 행을 다음과 같이 역추적한다.

| BM | LLD 시험 접점 | 최소 관측 필드 |
|:---|:---|:---|
| **BM-DD-P0-01** | `공통 job_token 취득`, `wake 계약` | source release state, token, downstream ready sequence, `clock_gettime` call count |
| **BM-DD-P0-02** | `주기 신원 및 FIFO 소비`, `SHM ABI 및 초기화` | `dispatch_counter`, `g_attached_before_publish`, init cursor, first returned token, replay count |
| **BM-DD-P0-03** | `fan-out 사전검증`, `동시성 및 drain/Lost Wakeup` | failpoint, destination별 mask/sequence, wake count, partial-state/disposition |
| **BM-DD-P0-04** | `payload visibility/retention` | payload token/generation/pattern, producer reuse event, consumer completion |
| **BM-DD-P0-05** | `권한 및 읽기 전용 seqlock` | UID/GID/ACL, allowlist membership, API `errno` |
| **BM-DD-P0-06** | `무-VFS 교체/종료 감지`, `동시성 및 drain/Lost Wakeup`, `wake 계약` | normal drain `WRITER_PENDING` wake completion, `g→g+2` `ESTALE`, pause/resume point, old/new instance, late CAS/wake, old API result |
| **BM-DD-P0-07** | `fan-out 사전검증`, `동시성 및 drain/Lost Wakeup` | fan-out=8, concurrent writers, latency distribution, wake order/count, timeout/`EAGAIN` |

타깃 미실행 시 하드웨어 검증은 미수행으로 기록한다.
