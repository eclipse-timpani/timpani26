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
| **REQ-DD-P0-F-001** | `Timpani-O`만이 Pipeline 선행 의존성(`predecessors`)을 파싱·검증해야 한다. 순환 의존성(Cycle), 자기 참조, 미존재 태스크 참조, 비정상/중복 태스크 이름이나 해시 충돌, 지원 상한(선행 최대 `16`개, 후행 간선 최대 `8`개) 초과 입력은 스케줄 생성 단계에서 사유와 함께 거절하고, 기존 확정 모델과 노드의 활성 테이블을 변경해서는 안 된다. | REQ-DD-001 | AC-DD-P0-01, AC-DD-P0-04 |
| **REQ-DD-P0-F-002** | O는 `predecessors`로 Source/Successor 역할을 계산하되 Task의 `TemporalClass`를 유지한다. L1은 기존 `TtSlot`, L2는 기존 `CbsConfig`로 발행하고 공통 DAG 메타를 별도로 전달한다. | REQ-DD-002, 009 | AC-DD-P0-01, AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-F-003** | N은 공통 DAG 참조와 Task의 계층별 TT/CBS 표현·CPU·용량을 확인하고 부적합 후보는 거절한다. TT/CBS 승인식을 재계산하지 않는다. | REQ-DD-001, 002, CON-DD-03 | AC-DD-P0-04 |

### 3.2 런타임 `Data-Ready` 동기화 및 스케줄링 집행 (`Timpani-N` & `libttsched`)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-004** | Source는 실행 계층의 기존 활성화 방식을 따른다: L1은 Timer Master, L2는 기존 Event release. Successor는 `ALL` ready와 계층별 dispatch gate를 모두 만족해야 한다. 제어 wake는 ready로 소비하지 않는다. | REQ-DD-002, 004 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-005** | `ALL` 충족 시 단일 `CAS`로 ready 상태를 소비하고 공통 Data-Ready 순번을 1회 증가한다. L1/L2 실행 어댑터는 이 순번을 각 계층의 dispatch gate에 연결한다. | REQ-DD-004, 005, 007 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-006** | `signal_ready`/`wait_data`는 안정 `generation`, Task 신원, 실제 Data-Ready/dispatch 순번 변화를 확인한다. | REQ-DD-004, 015 | AC-DD-P0-03, AC-DD-P0-05 |
| **REQ-DD-P0-F-007** | L1 Successor는 허용 TT slot/Window에서, L2 Successor는 CBS CPU·예산·Gap에서 실행한다. 어느 경우도 계층별 scheduler 규칙을 우회해서는 안 된다. | REQ-DD-008, 009 | AC-DD-P0-01, AC-DD-P0-05 |

### 3.3 시험 관측 및 전제 이탈 처리

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-008** | 시험 로그는 테이블 식별자, 태스크별 시작·완료 `CLOCK_MONOTONIC` 타임스탬프, `ttsched_signal_ready()` 소요 시간, 사전 거절 사유를 연결할 수 있어야 한다. 연결 단절·시계 이상 등 Phase 0 시험 전제가 깨지면 성공으로 판정하지 않고 시험을 중단해 원인을 기록해야 하며, 자동 복구나 타임아웃 `SKIP`이 동작한다고 가정해서는 안 된다. | REQ-DD-013 | AC-DD-P0-01..06 |

---

## 4. 비기능 요구사항 (Phase 0 Non-Functional Requirements)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-N-001** | 변경한 RT critical path(`Timer Master` 루프, `libttsched` 신호/대기 경로, `scx_timpani` 콜백)에 동적 메모리 할당, 블로킹 IPC/gRPC, 파일/네트워크 I/O, 무한 busy-spin 또는 스케줄링 결정을 가로지르는 락 보유를 추가해서는 안 되며, 워크로드 컨테이너는 `CAP_BPF` 특권 없이 동작해야 한다. | REQ-DD-007, CON-DD-04 | AC-DD-P0-06 |
| **REQ-DD-P0-N-002** | `signal_ready` 오버헤드는 `5 µs` 이하여야 하며, L1/L2 그래프 혼합이 각 계층의 기존 타이밍 기준을 악화시키지 않아야 한다. | REQ-DD-007, 008, 013 | AC-DD-P0-01, AC-DD-P0-05 |
| **REQ-DD-P0-N-003** | DAG 순서·공통 의존성 메타·계층별 TT/CBS 배치가 입력 순서와 무관해야 한다. 실행 계층은 `predecessors` 유무로 결정되지 않는다. | REQ-DD-001, 002, 009, CON-DD-04 | AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-N-004** | 인터페이스 변경은 승인 후 반영하며 TT/CBS/SHM/간선 상한을 넘는 입력은 사전 거절한다. | REQ-DD-001, 015, CON-DD-05, CON-DD-06 | AC-DD-P0-04, AC-DD-P0-05 |

---

## 5. Phase 0 검증 시나리오

고정 `Epoch`/`Hyperperiod`(`100ms`)와 사전 지정 Isolated CPU에서 `Timpani-O` 생성 결과를 사용한다. host 단위 시험과 Linux 6.12+ `sched_ext` 목표 환경의 End-to-End 실행 증거를 구분하여 기록한다.

| 시험 ID | 입력·조작 | 관측 및 성공 판정 | REQ 수용 기준 |
|:---|:---|:---|:---|
| **TC-DD-P0-001** | 고정 DAG에서 L1→L1, L1→L2, L2→L1, L2→L2 최소 조합을 table-driven으로 반복 구동. 각 Task는 원래 클래스 유지 | 모든 Successor가 `ALL` 후에만 실행되고 L1은 TT gate, L2는 CBS gate를 준수. 순서 위반 `0`, `signal_ready <= 5 µs` | AC-DD-P0-01 |
| **TC-DD-P0-002** | 일부 선행만 반복 완료하고 L1/L2 Successor를 각각 포함 | 어느 계층 Successor도 wakeup/실행 `0`회, 제어 wake 오소비 없음 | AC-DD-P0-02 |
| **TC-DD-P0-003** | 순환 의존성(`A → B → A`), 자기 참조(`A → A`), 미존재 참조, 용량/간선 상한 초과 입력을 `Timpani-O` 및 `Timpani-N` 사전 확인에 제출 | 활성화 전 거절 및 사유 확인, `Timpani-O` 확정 모델과 `Timpani-N` 활성 테이블·SHM 상태 불변 | AC-DD-P0-04 |
| **TC-DD-P0-004** | 홀수 `generation`/`magic=0`에서 API 호출하고, ready·dispatch 순번 증가 없는 제어 wake 인가 | 불안정 상태에서 상태 불변·`-1`, 제어 wake 실행 `0`건 | AC-DD-P0-03 |
| **TC-DD-P0-005** | L1/L2 혼합 DAG와 기존 TT/CBS 회귀·011 일반 TT 갱신 시험 실행 | DAG 추가 후 각 Task의 기존 클래스·스케줄링 회귀 `0`건 | AC-DD-P0-05 |
| **TC-DD-P0-006** | 제출 순서를 섞고 동일 DAG 반복 생성 | 위상 순서·공통 의존성 메타·계층별 배치 100% 일치 | AC-DD-P0-06 |

---

## 6. 미결·승인 항목의 Phase 0 처리

| REQ 항목 | Phase 0 결정 / 계약 | 상세 근거 |
|:---|:---|:---|
| **OI-DD-01** | Task 실행 클래스와 무관한 공통 DAG 메타 전달 방식. TT/CBS entry에 의존성을 중복하지 않는 단일 표현 또는 동등한 손실 없는 표현 **(승인 필수)** | DDR-009 `D-DD-P0-03` |
| **OI-DD-02** | SHM 공통 `pipeline[]`, 준비 순번/dispatch 순번, `wait_data`/`signal_ready`, TT/CBS 어댑터 연결 **(SHM/API 승인)** | DDR-009 `D-DD-P0-01`, `D-DD-P0-04` |
| **OI-DD-03** | L1 TT slot gate와 L2 CBS release gate를 공통 `ALL` ready에 연결하는 Phase 0 최소 규약 | DDR-009 `D-DD-P0-03`, `D-DD-P0-05` |

---

## 7. 추적성 및 후속 단계 이관

§3·§4의 모든 활성 요구사항은 상위 [REQ-009](REQ-009-data-driven-workload.md) ID와 `AC-DD-P0-01` ~ `06`을 인용하며, 역방향으로 `AC-DD-P0-01` ~ `06`은 §5의 `TC-DD-P0-001` ~ `006`에 대응한다. [HLD-009](HLD-009-data-driven-workload.md)는 컴포넌트 책임과 흐름, [DDR-009](DDR-009-data-driven-workload.md)는 핵심 설계 결정, [LLD-009](LLD-009-data-driven-workload.md)는 내부 자료·동기화·검증 접점을 명세하며 본 SRS의 활성 ID를 인용한다.

| 후속 단계 | 이관 범위 (본 문서 비포함) |
|:---|:---|
| **Phase 1** | Window, Harmonic 주기, L1/L2 혼합 DAG의 `E2E Feasibility`, 계층별 CPU/slot/예산 간섭 정밀화, Pipeline 갱신 |
| **Phase 2** | `SKIP`, 최초 원인 `DATA_TIMEOUT`, Sink Fallback, 클래스 공통 복구 상태 초기화 |

이관 범위의 상태 플래그·타이머 훅·복구 상태 기계를 Phase 0에 선행 구현하지 않는다.
