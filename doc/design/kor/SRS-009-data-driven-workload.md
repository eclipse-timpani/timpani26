<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-009: Data-Driven Workload — Phase 0 소프트웨어 요구사항 명세

**작성일:** 2026-10-27  
**최종 수정:** 2026-11-01  
**상태:** Draft — Phase 0 전용 (구현 완료 또는 기능안전 운용 승인 아님)  
**상위 기준:** [SRS-000](SRS-000-timpani-system.md) §5.1 `REQ-DD-001`, [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, §9  
**연계 문서:** [HLD-009](HLD-009-data-driven-workload.md), [DDR-009](DDR-009-data-driven-workload.md), [LLD-009](LLD-009-data-driven-workload.md), [REQ-011](REQ-011-runtime-table-update.md), [DDR-011](DDR-011-runtime-table-update.md)  
**용어 기준:** [REQ-009](REQ-009-data-driven-workload.md) §2 및 [DDR-012](DDR-012-fault-management.md) §2

---

## 1. 목적과 적용 규칙

본 문서는 [REQ-009](REQ-009-data-driven-workload.md)에 정의된 Data-Driven Workload의 **Phase 0(기본 체인 동작 확인)** 달성에 필요한 소프트웨어 요구사항만 명세한다. Phase 1(`Activation Window` 통제, 오프라인 E2E 검증, Pipeline 포함 테이블의 정상 런타임 갱신) 및 Phase 2(입력 누락 `SKIP`, `DATA_TIMEOUT` 보고, 비정상 상태 안전 초기화) 요구사항은 본 문서로 자동 승계하지 않는다.

요구사항 ID는 전체 단계 요구사항(`REQ-DD-###`)과 구분되도록 `REQ-DD-P0-F-###`(기능) 및 `REQ-DD-P0-N-###`(비기능) 접두어를 사용한다. 표의 모든 활성 항목은 Phase 0 범위에서 **SHALL**(필수)이다. 복합 단계 상위 요구사항(`REQ-DD-001`, `002`, `015`, `013`)에 대한 매핑은 **[REQ-009](REQ-009-data-driven-workload.md) §1.5.3의 Phase 0 적용 부분에만 한정**되며, 해당 상위 ID 전체의 완료를 뜻하지 않는다.

---

## 2. Phase 0 시험 경계

| 구분 | Phase 0 조건 |
|:---|:---|
| **실행 대상** | 단일 노드에서 원래 `TemporalClass`가 L1 TT 또는 L2 CBS인 Task를 동일 DAG에 연결. DAG 역할로 실행 계층을 변경하지 않음 |
| **체인 구성** | 동일 주기 기본 체인과 L1/L2 혼합 Task 조합을 사용. 각 Task는 기존 계층의 TT timing 또는 CBS timing을 갖는다 |
| **시간·상태 기준** | 고정 `E/H/table`. `ALL`은 공통 ready를 만든다. L1은 허용 TT slot에서, L2는 CBS release/gate에서 실행한다. ready 상태는 원자적으로 1회 소비한다 |
| **런타임 갱신과의 경계** | 시험 중 **Pipeline 포함 테이블의 런타임 갱신(`UPD-REPLACE / ADD / REMOVE / CLEAR`)은 수행하지 않음**(Phase 1 이관). 단, `/timpani_ttsched` 안정 게시 확인·제어 wake 비실행([DDR-011](DDR-011-runtime-table-update.md) `D-P0-05`)과 Pipeline 미포함 일반 TT 테이블의 [REQ-011](REQ-011-runtime-table-update.md) Phase 0 정상 갱신 회귀 방지는 필수 |
| **입력 및 역할 분담** | `Timpani-O` 입력(또는 시험 입력 경로)으로 DAG를 제출하고 `Timpani-O`만 검증·생성하여 기존 `O → N` 경로로 전송. `Timpani-N`이나 SHM 직접 주입만으로 End-to-End 성공 판정 금지 |
| **인터페이스·승인 제약** | 기존 공개 C++ 메서드 및 `timpani_task_slot` 초기 레이아웃 보존. `.proto` 확장(`OI-DD-01`), SHM 후방 확장 및 `libttsched` 신규 공개 API(`OI-DD-02`)는 코드 반영 전 Project Leader 승인 필수 |

Harmonic 다중 주기, 계층별 정밀 시간 창, 정량 `E2E Feasibility`, `SKIP`, `DATA_TIMEOUT`은 Phase 0 범위에서 제외한다. Phase 0의 CPU/slot 배치는 기존 TT 또는 CBS scheduler 정책을 사용한다.

---

## 3. 기능 요구사항 (Phase 0 Functional Requirements)

### 3.1 입력·생성·사전 거절 (`Timpani-O` & `Timpani-N`)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-001** | `Timpani-O`만이 `TaskReference(workload_id, task_id)` 형식의 선행 의존성을 해석·검증해야 한다. 빈 `workload_id`는 현재 workload로 정규화하고 다른 workload 참조는 Phase 0에서 거절한다. 순환, 자기 참조, 미존재 참조, Task 쌍 중복, `(workload_id_hash, task_id_hash)` 충돌, 선행 최대 `16`개 및 후행 간선 최대 `8`개 초과 입력은 사유와 함께 거절하고 확정 모델과 활성 테이블을 변경하지 않아야 한다. | REQ-DD-001 | AC-DD-P0-01, AC-DD-P0-04 |
| **REQ-DD-P0-F-002** | O는 각 Task의 `TaskInfo.temporal_class` 명시값을 사용하고, 미지정 Task는 `SchedInfo.temporal_class`를 상속해야 한다. resolved class를 유지한 채 `TtSlot` 또는 `CbsConfig`를 만들고, DAG 역할과 별도 공통 메타를 `(workload_id_hash, task_id_hash)` 쌍으로 전달한다. | REQ-DD-002, 009 | AC-DD-P0-01, AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-F-003** | N은 공통 DAG Task key가 정확히 하나의 TT 또는 CBS entry와 대응되는지 확인한다. key는 `(workload_id_hash, task_id_hash)` 쌍이며, 부적합 후보는 거절한다. TT/CBS 승인식을 재계산하지 않는다. | REQ-DD-001, 002, CON-DD-03 | AC-DD-P0-04 |

### 3.2 런타임 `Data-Ready` 동기화 및 스케줄링 집행 (`Timpani-N` & `libttsched`)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-004** | L1/L2 Source는 기존 Timer Master/Event release 경로를 유지한다. Successor는 `ALL` ready와 계층별 dispatch gate를 모두 만족해야 하고, 게시·종료·슬롯 재사용 wake는 ready나 dispatch로 소비하지 않는다. | REQ-DD-002, 004 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-005** | `ALL` 충족 시 packed `ready_state` 단일 64비트 CAS로 predecessor mask를 0으로 소비하고 ready sequence를 1 증가해야 한다. CAS와 L2/Timer Master `dispatch_counter` 갱신·futex wake는 게시 commit guard 안에서 수행하며 64비트 CAS는 lock-free여야 한다. 앱은 완료 job당 `signal_ready()`를 최대 1회 호출하고 성공 후 재호출하지 않는다. | REQ-DD-004, 005, 007 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-006** | `signal_ready`/`wait_data`는 실제 SHM 크기, extension `magic/version/abi_size`, 안정 `generation`, Task 신원, commit guard를 확인한다. `dispatch_counter`만 futex wait/wake word이며, ABI·generation·writer-pending 실패는 `errno`와 함께 상태 변경 없이 실패해야 한다. | REQ-DD-004, 015 | AC-DD-P0-03, AC-DD-P0-05 |
| **REQ-DD-P0-F-007** | L1 Successor는 자신의 TT slot 시각에만 pending ready가 있을 때 `dispatch_counter`를 1 증가시키고 wake한다. slot에 ready가 없거나 ready가 slot 후에 생기면 해당 slot에서 wake가 `0`건이다. L2 Successor는 `ALL` commit에서 `dispatch_counter`를 1 증가시키고 wake한 뒤 BPF CBS CPU/budget/Gap gate만 실제 실행을 결정한다. | REQ-DD-008, 009 | AC-DD-P0-01, AC-DD-P0-05 |

### 3.3 시험 관측 및 전제 이탈 처리

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-008** | 시험 로그는 테이블 식별자, 태스크별 시작·완료 `CLOCK_MONOTONIC` 타임스탬프, `ttsched_signal_ready()` 소요 시간, 사전 거절 사유를 연결할 수 있어야 한다. 연결 단절·시계 이상 등 Phase 0 시험 전제가 깨지면 성공으로 판정하지 않고 시험을 중단해 원인을 기록해야 하며, 자동 복구나 타임아웃 `SKIP`이 동작한다고 가정해서는 안 된다. | REQ-DD-013 | AC-DD-P0-01..06 |

---

## 4. 비기능 요구사항 (Phase 0 Non-Functional Requirements)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-N-001** | 변경한 RT critical path(`Timer Master` 루프, `libttsched` 신호/대기 경로, `scx_timpani` 콜백)에 동적 메모리 할당, 블로킹 IPC/gRPC, 파일/네트워크 I/O, 무한 busy-spin 또는 블로킹 lock을 추가해서는 안 된다. `publication_guard` reader는 비대기 fetch-add/fetch-sub만 사용하고 경합 시 `EAGAIN` 또는 `ESTALE`을 반환한다. 게시자는 active commit을 무한 대기하지 않고 기한 초과 시 게시를 이연한다. `ready_state` 64비트 원자 연산은 대상 아키텍처에서 lock-free여야 한다. 워크로드 컨테이너는 `CAP_BPF` 특권 없이 동작해야 한다. | REQ-DD-007, CON-DD-04 | AC-DD-P0-06 |
| **REQ-DD-P0-N-002** | `signal_ready` 오버헤드는 `5 µs` 이하여야 하며, L1/L2 그래프 혼합이 각 계층의 기존 타이밍 기준을 악화시키지 않아야 한다. | REQ-DD-007, 008, 013 | AC-DD-P0-01, AC-DD-P0-05 |
| **REQ-DD-P0-N-003** | DAG 순서·공통 의존성 메타·계층별 TT/CBS 배치가 입력 순서와 무관해야 한다. 실행 계층은 `predecessors` 유무로 결정되지 않는다. | REQ-DD-001, 002, 009, CON-DD-04 | AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-N-004** | 인터페이스 변경은 승인 후 반영하며 TT/CBS/SHM/간선 상한을 넘는 입력은 사전 거절한다. | REQ-DD-001, 015, CON-DD-05, CON-DD-06 | AC-DD-P0-04, AC-DD-P0-05 |

---

## 5. Phase 0 검증 시나리오

고정 `Epoch`/`Hyperperiod`(`100ms`)와 사전 지정 Isolated CPU에서 `Timpani-O` 생성 결과를 사용한다. host 단위 시험과 Linux 6.12+ `sched_ext` 목표 환경의 End-to-End 실행 증거를 구분하여 기록한다.

| 시험 ID | 입력·조작 | 관측 및 성공 판정 | REQ 수용 기준 |
|:---|:---|:---|:---|
| **TC-DD-P0-001** | 고정 DAG에서 Task class를 명시한 경우와 workload class에서 상속한 경우를 섞어 L1→L1, L1→L2, L2→L1, L2→L2 최소 조합을 반복한다. L1에는 `ALL`을 slot 전에, slot 직후, ready 없이 배치하고 CBS budget/TT active 상태도 조작한다 | L1은 slot 전에는 `dispatch_counter` 증가 `0`, slot 전에 준비된 ready는 해당 slot에서 `+1`; slot 직후 생성된 ready는 현재 slot에서 wake `0` 후 다음 slot에서 `+1`; pending은 정상 시험에서 1개다. L2는 `ALL` 직후 release `1`이나 실제 실행은 CBS budget/Gap에 따라 지연 가능. 순서 위반 `0`, class 회귀 `0`, `signal_ready <= 5 µs` | AC-DD-P0-01 |
| **TC-DD-P0-002** | 일부 선행만 완료하고 L1/L2 Successor를 각각 포함하며, L1의 slot에 ready가 없고 control wake만 인가한다 | 어느 계층 Successor도 dispatch/wakeup/실행 `0`회. control wake는 `ESTALE`이고 카운터 증가가 없다 | AC-DD-P0-02 |
| **TC-DD-P0-003** | 순환(`A → B → A`), 자기 참조, 미존재 참조, 다른 workload를 가리키는 Phase 0 참조, Task 쌍 중복/해시 충돌, 용량·간선 상한 초과 입력을 `Timpani-O` 및 `Timpani-N` 사전 확인에 제출 | 활성화 전 거절 및 사유 확인, `Timpani-O` 확정 모델과 `Timpani-N` 활성 테이블·SHM 상태 불변 | AC-DD-P0-04 |
| **TC-DD-P0-004** | 구 784바이트 SHM, extension magic/version/size 불일치, `magic=0`, 홀수 `generation`, writer-pending, O_RDONLY `signal_ready`, 그리고 안정 확인 직후 게시가 packed CAS 전후에 시작되는 interleaving에서 API를 호출한다. dispatch 증가 없는 게시/종료 control wake도 인가한다 | 구 SHM에서는 TT API만 동작하고 Pipeline API는 `ENOTSUP`; ABI 오류는 `EINVAL`; write 불가는 `EACCES`; 불안정 게시/제어 wake는 변경 없이 `ESTALE`. packed CAS·dispatch·wake는 게시자와 완전히 직렬화되고 구/신 세대 혼합 기록 및 control-wake 실행 `0`건 | AC-DD-P0-03 |
| **TC-DD-P0-005** | L1/L2 혼합 DAG와 기존 TT/CBS 회귀·011 일반 TT 갱신 시험 실행. TT Successor에 slot 전에 ready를 만들고, CBS Successor에 budget=0과 active TT을 각각 인가한다 | DAG 추가 후 기존 Source는 기존 counter/Event 경로를 유지한다. TT는 slot gate, CBS는 release 후 budget/Gap gate를 지키며 기존 class·scheduling 회귀 `0`건 | AC-DD-P0-05 |
| **TC-DD-P0-006** | 제출 순서를 섞고 동일 DAG 반복 생성 | 위상 순서·공통 의존성 메타·계층별 배치 100% 일치 | AC-DD-P0-06 |

---

## 6. 미결·승인 항목의 Phase 0 처리

| REQ 항목 | Phase 0 결정 / 계약 | 상세 근거 |
|:---|:---|:---|
| **OI-DD-01** | `TaskInfo.temporal_class`는 optional explicit-over-inherit 규칙을 따른다. 같은 workload의 `TaskReference`로 DAG를 전달하고, O→N에서는 `(workload_id_hash, task_id_hash)` 쌍으로 공통 metadata를 식별한다. Phase 0은 `ALL`만 지원하고 `E2E`는 OI-DD-05로 이관한다 **(인터페이스 승인)** | DDR-009 `D-DD-P0-03` |
| **OI-DD-02** | 784바이트 prefix 뒤 extension header와 `pipeline[]`를 추가한다. ABI/permission/`errno` 계약, 64비트 lock-free `ready_state` CAS, `dispatch_counter` 전용 futex, O_RDWR/O_RDONLY fallback, 구 ABI 호환 및 control-wake 시험을 조건으로 설계를 승인한다. 미충족 시험이 있으면 구현 병합을 보류한다 **(조건부 SHM/API 승인)** | DDR-009 `D-DD-P0-01`, `D-DD-P0-04` |
| **OI-DD-03** | Phase 0 Source는 기존 TT/CBS 경로를 유지한다. L1 Successor는 자신의 TT slot에서 pending ready가 있을 때만 `dispatch_counter`를 1 증가시키며, L2 Successor는 `ALL` commit에서 1 증가시키고 wake한다. BPF는 기존 CBS budget/Gap을 집행한다. slot-miss pending, pending 최대 1개, `Activation Window`/`SKIP` 이관을 조건으로 설계 승인하되, no-ready·mixed-class·late-ready 시험 미통과 시 구현 병합을 보류한다 **(조건부 Phase 0 실행 연결 승인)** | DDR-009 `D-DD-P0-03`, `D-DD-P0-05` |

---

## 7. 추적성 및 후속 단계 이관

§3·§4의 모든 활성 요구사항은 상위 [REQ-009](REQ-009-data-driven-workload.md) ID와 `AC-DD-P0-01` ~ `06`을 인용하며, 역방향으로 `AC-DD-P0-01` ~ `06`은 §5의 `TC-DD-P0-001` ~ `006`에 대응한다. [HLD-009](HLD-009-data-driven-workload.md)는 컴포넌트 책임과 흐름, [DDR-009](DDR-009-data-driven-workload.md)는 핵심 설계 결정, [LLD-009](LLD-009-data-driven-workload.md)는 내부 자료·동기화·검증 접점을 명세하며 본 SRS의 활성 ID를 인용한다.

| 후속 단계 | 이관 범위 (본 문서 비포함) |
|:---|:---|
| **Phase 1** | Window, Harmonic 주기, L1/L2 혼합 DAG의 `E2E Feasibility`, 계층별 CPU/slot/예산 간섭 정밀화, Pipeline 갱신 |
| **Phase 2** | `SKIP`, 최초 원인 `DATA_TIMEOUT`, Sink Fallback, writer-pending/활성 commit을 포함한 클래스 공통 복구 상태 초기화 |

이관 범위의 상태 플래그·타이머 훅·복구 상태 기계를 Phase 0에 선행 구현하지 않는다.
