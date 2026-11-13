<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-008: Event-Driven Workload Support 소프트웨어 요구사항 명세 (SRS)

**날짜:** 2026-11-09 (Last Updated: 2026-11-10)
**상태:** Draft (v3 — REQ-008 v4 반영)
**작성:** Human + AI
**상위 문서:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-000](SRS-000-timpani-system.md)
**관련 문서:** [DDR-001](DDR-001-workload-model.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-006](DDR-006-communication-architecture.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md)
**하위 문서:** DDR-008 (설계 결정), HLD-008, LLD-008

> 본 문서는 [REQ-008](REQ-008-event-driven-workload-support.md)의 상위 요구사항을 **소프트웨어 컴포넌트에 할당 가능하고 검증 가능한 수준**으로 구체화한다. SRS-000 §1의 규약대로 기능별 SRS(`SRS-008`)는 특정 결정 기록(`DDR-008`)의 요구사항을 구체화한다.
> 설계 선택(알고리즘, 자료구조, 스레드 모델)은 본 문서에서 **결정하지 않으며** DDR-008이 결정한다. 다만 검증 가능성을 위해 "관측 가능한 동작"은 구체적으로 기술한다.
> REQ-008의 두 원칙(**전송 계층 중립**, **Phase 1/2 분할**)을 계승한다. 각 요구사항에 Phase를 표시하며, `P1*`는 Phase 1에서 단순화된 형태로 충족하고 Phase 2에서 강화됨을 뜻한다.
>
> **키워드 규약**: **SHALL** = 필수, **SHOULD** = 권장, **MAY** = 선택.
> **검증 방법 약어**: A = 분석(Analysis), T = 테스트(Test, 단위/통합), M = 측정(Measurement, 타이밍 벤치), D = 시연(Demonstration, PoC).

---

## 요약 (Korean Summary)

- 목적: L2(Sporadic + SafetyCritical) Event-Driven 워크로드를 TIMPANI가 **도달 감지 → CBS 격리 실행 → 완료 판정 → 폴트 보고**의 종단간 흐름으로 결정론적으로 지원하기 위한 소프트웨어 요구사항을 컴포넌트(proto, TIMPANI-O, TIMPANI-N 사용자 공간, BPF 스케줄러, 애플리케이션 라이브러리, 검증 프레임워크)별로 정의한다.
- 구조: REQ-008 §2의 **공통 Event/Job 모델**(Event source·Event·Job, 생명주기 사건 4종, 규칙 R1~R4)을 기준으로, 관측 어댑터(BPF 커널 관측, `libttsched` 마커)와 집행(CBS·폴트·통계)을 분리해 컴포넌트에 할당한다(FR-G-002).
- 핵심(P1): (1) Job 경계(활성화/완료)의 결정적 규칙과 전송 계층 중립 마커 인터페이스(도달 통지·배치 소비·도달 기준 MIT), (2) 이용률 + 단순 블로킹 합 기반 승인, (3) 배정 CPU 고정·FIFO 서버 순서·바운드된 백업 보충, (4) 유형 손실 없는 폴트 분류(DMISS/BUDGET_EXCEED/MIT_VIOLATION)와 속도 제한, (5) 정량 타이밍 요구사항(측정 방법 포함), (6) 와이어 호환 proto 확장(MIT 필드, EVENT_DRIVEN 레이어, 폴트 유형).
- P2로 미룬 것: 마커 없는 태스크의 정밀 job 경계 분류, EDF 순서·윈도우 수요 승인, 태스크 단위 시간 클래스, 거부 사유 코드, TT 무교란 증분 갱신, NodeStatus 통계, watchdog/깨움 지연 폴트, HRTICK.
- 상태: Draft v3(공통 모델, 독립 검토 반영). 목표 수치는 REQ-008 §4.6의 후보값을 계승하며 측정으로 확정한다.

---

## 1. 서론 (Introduction)

### 1.1 목적
REQ-008의 각 요구사항을 구현·테스트 담당자가 직접 사용할 수 있는 소프트웨어 요구사항으로 분해하고, 각 요구사항에 **담당 컴포넌트, 검증 방법, 상위 추적성, Phase**를 부여한다.

### 1.2 범위
- 포함: TIMPANI-O(승인·테이블 생성·발행), TIMPANI-N 사용자 공간 데몬, `scx_timpani` BPF 스케줄러, 애플리케이션 라이브러리(`libttsched` 확장), Pullpiri↔O / O↔N 프로토콜, 검증 프레임워크가 요구하는 관측점.
- 제외: REQ-008 §7의 범위 외 항목(Aperiodic, D > MIT, 노드 간 체인, 미들웨어 고유 의미론, watchdog, QNX 포팅, Pullpiri YAML).

### 1.3 용어 정의 (본 문서에서의 정확한 의미)

| 용어 | 정의 |
|:--|:--|
| **L2 태스크** | `Sporadic + SafetyCritical` 워크로드에 속한 커널 스케줄링 단위(스레드). CBS 서버 1개에 1:1 대응 |
| **CBS 서버** | 예산 $C_s$, 보충 주기 $T_s$, 잔여 예산, 보충 시각, 상대 마감 $D$를 갖는 대역폭 서버 |
| **Event source** | L2 태스크를 활성화시키는 이벤트의 선언된 발생원. 속성: 식별자, 소비 태스크, MIT. P1: 태스크당 1개, 암묵 선언(식별자 = `(workload_id, task_id)`) |
| **Event** | Event source에서의 1회 도달. `(source, event_seq, t_arr)` |
| **미소비(pending) Event** | 도달했지만 아직 어떤 Job에도 소비되지 않은 Event. 처리 중 도달(R2)로 쌓이며, Job은 처리를 시작할 때 미소비 Event를 모두 소비한다(R4) |
| **Job** | 태스크가 wait point를 떠나 다음 wait point에 도달하기까지의 1회 처리 단위. 1개 이상의 Event를 소비한다(`n_events ≥ 1`, 정상 조건에서 1). 활성화 시각 $t_{act}$에서 시작하여 완료 시각 $t_{done}$에 끝남 |
| **Wait point** | 태스크가 "다음 Event를 기다리는" 블로킹 지점. 마커 API 또는 커널 관측 규칙으로 식별됨 |
| **Mid-job blocking** | job 처리 도중의 블로킹(뮤텍스, I/O, 선점). 완료가 아님 |
| **도달(Arrival)** | Event가 source에 도달하는 사건. $t_{arr}$, `event_seq` |
| **활성화(Activation)** | Job이 실행 가능해지는 사건(태스크 `WAITING → ACTIVE`). 대기 중 도달(R1) 또는 완료 시 미소비 Event 존재(R3)로 발생. $t_{act}$ 기록 |
| **완료(Completion)** | ACTIVE 상태의 태스크가 wait point에 진입하는 사건. $t_{done}$ 기록 |
| **응답 시간** | $R = t_{done} − t_{rel}$ ($t_{rel}$ = Job이 소비한 첫 Event의 $t_{arr}$). P1 판정값은 $R_{act} = t_{done} − t_{act}$ |
| **깨움 지연** | $L_{wake} = t_{start} − t_{act}$ ($t_{start}$ = job 최초 실행 개시) |
| **Gap** | 배정 CPU에서 L1 TT 슬롯이 활성이 아닌 시간 구간 |
| **$B_{tt}(k)$** | CPU $k$에서 길이 $L$인 임의 윈도우 내 TT 점유 시간의 최댓값 $B_{tt}(k, L)$(병합 슬롯 기준, hyperperiod 경계 wrap 포함). P1 승인식은 $L = D_i$를 사용한다. 윈도우가 여러 TT 블록을 덮으면 그 합이다 |
| **MIT 위반** | 같은 Event source의 연속 **도달** 간격이 `min_inter_arrival_us` 미만인 사건. 도달을 관측할 수 없는 경우 연속 활성화 간격으로 근사한다 |
| **보충 윈도우** | 서버 예산 $C_s$가 유효한 길이 $T_s$의 구간. Job 활성화 시(직전 윈도우가 끝난 경우) 또는 스로틀 후 승격 시에 시작한다. 격자에 정렬되지 않는다 |
| **Throttle 에피소드** | 잔여 예산이 0이 된 시점부터 보충으로 양수가 될 때까지의 구간 |
| **정격 부하** | 승인된 모든 L1/L2 워크로드가 활성이고 L2 이벤트가 MIT 간격으로 도달하는 조건 |
| **관측 어댑터** | 생명주기 사건을 실제 시스템에서 관측하는 수단. P1: **K**(BPF 커널 관측 — Activation/Start/Completion), **M**(`libttsched` 마커 — Arrival, `n_events`, 결정적 wait point) |
| **마커 API** | 앱이 Event 도달 통지(notify)와 wait point 진입(wait)을 TIMPANI 라이브러리에 명시하는 전송 계층 중립 C API(어댑터 M) |

### 1.4 참조
REQ-008, SRS-000, DDR-001~007/011/012, `timpani26` 소스(HEAD `4201573`), Linux sched_ext 문서(`Documentation/scheduler/sched-ext.rst`).

### 1.5 Phase 매핑 요약

| Phase | 포함 SRS-008 ID |
|:--|:--|
| **P1** | FR-G-001, 002; FR-O-001, 003, 004(P1: 사유 로그), 005, 010, 011, 012(P1 범위), 013(P1식), 014, 015, 016, 020; FR-N-002, 003, 004, 005, 006, 007, 010, 011, 012, 013, 014, 015, 020, 021, 022, 024, 026; FR-B-001~003, 004(P1 규칙), 005, 006, 010~013, 014, 015, 016(FIFO), 017, 020~023; FR-L-001~008; TMG-001~013(측정 시작); IF-PO-001, 004, 005, 006; IF-ON-001~004, 006; IF-L-001~003; IF-V-001~004; NFR-001, 002, 003, 005, 006, 007 |
| **P2** | FR-O-002, 004(응답 사유 필드), 017, 018, 021; FR-N-001, 023, 025; FR-B-004(P2 규칙), 016(EDF), 022(예약 유형 활성화); IF-PO-002, 003; IF-ON-005; NFR-004 |

---

## 2. 시스템 컨텍스트 및 컴포넌트 할당

### 2.1 컨텍스트

```mermaid
flowchart LR
  P[Pullpiri] -- "SchedInfo (MIT field)" --> O[TIMPANI-O]
  O -- "HierarchicalScheduleTable<br/>EVENT_DRIVEN layer" --> N[TIMPANI-N daemon]
  N -- "BPF maps" --> B[scx_timpani BPF]
  B -- "fault_ringbuf / job trace" --> N
  N -- "FaultInfo (typed)" --> O
  O -- "FaultInfo (typed)" --> P
  A[L2 App<br/>+ app-side middleware adapter] -. "standard wait (epoll/socket/futex)" .-> K[(Kernel wakeup)]
  K --> B
  A -. "marker API (libttsched):<br/>evt_open / evt_notify / evt_wait" .-> S[(SHM /timpani_ttsched)]
  S -. futex .-> K
  V[Verification FW] -. "trace channel / counters" .-> N
```

### 2.2 할당 매트릭스

| 컴포넌트 | 약어 | 책임 (본 기능 관점) |
|:--|:--|:--|
| 공통 | **G** | 전송 계층 중립 원칙, 공통 Event/Job 모델과 3층 분리 |
| Pullpiri↔O 프로토콜 (`schedinfo.proto`) | **IF-PO** | MIT 전용 필드, 폴트 유형 확장 (P2: 태스크 단위 시간 클래스, 거부 사유 코드) |
| O↔N 프로토콜 (`node_control.proto`) | **IF-ON** | EVENT_DRIVEN 레이어, CBS 엔트리 확장(MIT, CPU), 폴트 유형 확장 (P2: 통계 보고) |
| TIMPANI-O | **O** | 파라미터 검증, 배치, 승인(이용률 + 블로킹 합), 테이블 생성·발행, 폴트 전달 |
| TIMPANI-N 사용자 공간 | **N** | 테이블 적용, 태스크 식별·지연 바인딩, 백업 보충, 폴트 집계·보고, 통계·트레이스 노출 |
| BPF 스케줄러 (`scx_timpani`) | **BPF** | 관측 어댑터 K(job 상태 머신, 활성화/완료 감지), CBS 예산 집행, CPU 고정, 서버 순서, 폴트 이벤트 발생 |
| 애플리케이션 라이브러리 (`libttsched`) | **LIB** | 관측 어댑터 M: 마커 API(Event source 핸들, notify + $t_{arr}$, wait → `n_events`), 도달 기준 MIT 계수 |
| 검증 프레임워크 | **VF** | 트레이스 수집, 히스토그램, 합부 판정(본 문서는 요구 관측점만 정의) |

---

## 3. 기능 요구사항 (Functional Requirements)

> ID 형식: `SRS-008-FR-<컴포넌트>-NNN`. 각 항목: 명세 / 상위 REQ / 검증 / Phase.

### 3.0 공통 (G)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-G-001** | TIMPANI-O, TIMPANI-N, BPF, `libttsched`의 소스·proto·공개 헤더는 특정 미들웨어/전송 계층의 식별자·타입·헤더(예: SOME/IP service/instance/event ID, vsomeip, DDS 토픽)를 참조하지 않아야 한다(SHALL). Event는 `(Event source, event_seq)`로 식별하며, P1에서 Event source의 식별자는 소비 태스크의 `(workload_id, task_id)`이다. | MOD-006 | A(코드 리뷰, grep) | P1 |
| **SRS-008-FR-G-002** | 구현은 REQ-008 §2.1의 3층을 분리해야 한다(SHALL): (①) 애플리케이션 계약·폴트 의미·관측 출력(Job 레코드, Event source 통계)은 생명주기 사건(Arrival/Activation/Start/Completion)과 규칙 R1~R4로만 정의한다. (②) 관측 어댑터 K(BPF)와 M(`libttsched`)은 그 사건을 만들어 내는 수단이며, 어댑터별 한계는 REQ-008 §2.4를 따른다. (③) 집행(CBS·CPU 고정·DMISS 판정)은 P1에서 어댑터 K가 관측한 사건만 사용한다. 어댑터의 추가(P2)가 ①·③의 인터페이스를 바꾸지 않아야 한다(SHALL). | MOD-008 | A | P1 |

### 3.1 워크로드 모델 및 검증 (O)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-O-001** | TIMPANI-O는 L2 태스크 명세에서 `min_inter_arrival_us`(MIT), `wcet_us`, `deadline_us`, `max_dmiss`를 읽어야 하며(SHALL), MIT 전용 필드가 0이고 `period`가 설정된 Sporadic 입력은 **전환 기간 동안** `period`를 MIT로 해석하되 경고를 기록해야 한다(SHOULD). | MOD-001, IF-001 | T | P1 |
| **SRS-008-FR-O-002** | TIMPANI-O는 태스크 단위 시간 클래스를 지원해야 한다(SHALL). 태스크 클래스가 미지정(0)이면 워크로드 클래스를 상속한다. 한 워크로드 내에 Periodic(TT)과 Sporadic(CBS) 태스크가 공존할 수 있어야 한다. | IF-002 | T | P2 |
| **SRS-008-FR-O-003** | TIMPANI-O는 L2 태스크에 대해 `0 < wcet_us ≤ deadline_us ≤ min_inter_arrival_us`, `min_inter_arrival_us > 0`, `max_dmiss ≥ 0`, 그리고 `task_id` 길이 ≤ 15바이트(Linux comm 제한)와 같은 노드 안에서의 `task_id` 유일성을 검증하고 위반 시 워크로드 전체를 거부해야 한다(SHALL). `deadline_us = 0`은 `min_inter_arrival_us`로 기본 설정한다. | MOD-004/005, A-03 | T | P1 |
| **SRS-008-FR-O-004** | TIMPANI-O는 승인 거부 시 구조화된 사유 코드(최소: `PARAM_INVALID`, `UTIL_EXCEEDED`, `DEADLINE_INFEASIBLE`, `NO_CPU`, `NODE_UNKNOWN`, `NON_HARMONIC`)와 메시지를 응답에 포함해야 한다(SHOULD, P2 — REQ-008 IF-006). P1에서는 동일 사유를 로그에 기록해야 한다(SHALL). 구현 내부 사유와의 대응은 LLD-008 §3.1의 표를 따른다. | IF-006 | T | P2 (로그는 P1) |
| **SRS-008-FR-O-005** | 승인 거부 시 TIMPANI-O는 **거부 이전의 유효한 워크로드 집합과 테이블을 보존**해야 하며(SHALL), 동일 `workload_id`의 갱신이 거부되면 이전 버전을 복원해야 한다. | RTU-001, AC-08 | T | P1 |

### 3.2 배치 및 승인 (O)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-O-010** | TIMPANI-O는 L2 태스크별 CBS 파라미터를 $C_s = \text{wcet\_us}$, $T_s = \text{min\_inter\_arrival\_us}$, $D = \text{deadline\_us}$로 산출해야 한다(SHALL). | CBS-001 | T | P1 |
| **SRS-008-FR-O-011** | TIMPANI-O는 CPU $k$별로 $U_{tt}(k) + \sum_j C_{s,j}/T_{s,j} + U_{overhead} \le U_{bound}$ 를 검증해야 한다(SHALL). $U_{bound}$, $U_{overhead}$는 DDR-004/007 값(0.80, 0.02)을 설정 가능 상수로 사용한다. TT 이용률 $U_{tt}(k) \le U_{bound}$ 도 함께 검증해야 한다(SHALL, 현행 미검사 보완). | CBS-001 | T | P1 |
| **SRS-008-FR-O-012** | TIMPANI-O는 CPU $k$의 TT 테이블로부터 길이 $L$ 윈도우 내 최대 TT 점유 $B_{tt}(k, L)$(병합 슬롯 기준, hyperperiod 경계 wrap 포함)를 계산할 수 있어야 한다(SHALL, P1; $L$ = 각 L2 태스크의 $D_i$). P2에서는 Gap 공급 함수 $sbf_k(L)$ 기반 정밀식으로 확장한다(SHALL). | CBS-004, ORD-001 | T | P1* |
| **SRS-008-FR-O-013** | TIMPANI-O는 테이블 생성 시 CPU $k$에 배치된 **모든** L2 태스크 $i$(새로 추가되는 태스크와 기존 태스크)에 대해 마감 충족 충분 조건을 검증하고 하나라도 불충족이면 `DEADLINE_INFEASIBLE`로 거부해야 한다(SHALL). **P1 조건(보수적)**: $C_{s,i} + B_{tt}(k, D_i) + L_{wake}^{max} + \sum_{j \ne i} C_{s,j} \le D_i$, 여기서 $L_{wake}^{max}$는 REQ-008 TIM-001 목표값(설정 상수)이고, $C_{s,i}$는 Job당 차감 실행 시간(활성화부터 wait point에서 잠들 때까지, 대기 진입 경로 포함)의 상한보다 커야 한다(선언 책임은 앱, HLD-008 §8). 이 식의 전제는 FR-B-016(FIFO, 스로틀 서버는 뒤로)과 FR-B-014(다른 서버가 앞서 실행하는 양 ≤ $C_{s,j}$)이다. CPU당 L2 태스크 수는 FR-B-015의 승격 스캔 상한(16) 이하여야 한다(SHALL). **P2 조건**: EDF 순서 전제의 윈도우 수요·공급 함수식(DDR-008 후속). | CBS-004 | T, A | P1* |
| **SRS-008-FR-O-014** | TIMPANI-O의 L2 CPU 배치는 FR-O-011과 FR-O-013을 모두 만족하는 CPU만 후보로 삼아야 하며(SHALL), 최소 Gap 길이가 `CBS_MIN_EXEC_US` 미만인 CPU는 후보에서 제외해야 한다(SHALL, 현행 경고→거부). 후보 중 선택 규칙(worst-fit 등)은 DDR-008에서 결정한다. | ORD-001 | T | P1 |
| **SRS-008-FR-O-015** | TIMPANI-O는 TT 슬롯 반복 주기와 테이블 `hyperperiod_us`의 불일치(CPU별 LCM vs 전역 LCM)를 제거해야 하며(SHALL), L2 전용 노드의 `hyperperiod_us`는 0이 아닌 정의된 값이어야 한다(SHALL). | §10-16 (선행 버그) | T | P1 |
| **SRS-008-FR-O-016** | TIMPANI-O는 L2 엔트리를 `LayerConfig.model = EVENT_DRIVEN`인 별도 레이어로 발행해야 하며(SHALL), 각 엔트리에 $C_s$, $T_s$, MIT, $D$, `current_limit`, 배정 CPU를 포함해야 한다. | IF-003 | T | P1 |
| **SRS-008-FR-O-017** | L2 워크로드만 변경된 테이블 발행에서 TIMPANI-O는 `epoch_ns`를 갱신하지 않아야 한다(SHALL). TT 슬롯 집합이 변경된 경우에만 epoch 갱신이 허용된다. | RTU-002/003 | T | P2 |
| **SRS-008-FR-O-018** | TIMPANI-O는 L2 워크로드 추가·제거 시 영향을 받는 노드에만 테이블을 발행해야 한다(SHOULD). | RTU-002 | T | P2 |

### 3.3 폴트 전달 (O)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-O-020** | TIMPANI-O는 TIMPANI-N으로부터 수신한 폴트 유형(`DMISS`, `BUDGET_EXCEED`, `MIT_VIOLATION`; 예약 유형 `WATCHDOG`은 P2에 활성화)을 **동일 의미의 유형**으로 Pullpiri에 전달해야 하며(SHALL), `UNKNOWN`으로 축약하지 않아야 한다. | PRT-005, IF-004 | T | P1 |
| **SRS-008-FR-O-021** | TIMPANI-O는 폴트 전달 실패 시 재시도 큐에 적재하되, 폴트 전달이 노드 스트림 수신 처리를 블로킹하지 않도록 해야 한다(SHOULD). | TIM-005, OI-7 | T | P2 |

### 3.4 테이블 적용 및 태스크 식별 (N)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-N-001** | TIMPANI-N은 수신 테이블을 현재 적용 테이블과 비교하여 **변경된 엔트리만** 적용해야 한다(SHALL). (TT 테이블이 동일할 때 Timer Master 재시작을 생략하는 P1* SHOULD는 FR-N-006.) | RTU-002 | T, M | P2 |
| **SRS-008-FR-N-002** | TIMPANI-N은 EVENT_DRIVEN 레이어의 각 엔트리에 대해 CBS 상태(예산 $C_s$, 주기 $T_s$, MIT, $D$, 잔여 = $C_s$, 보충 시각)를 BPF에 등록하고, 해당 태스크를 `SCHED_EXT`로 전환하며, 엔트리에 명시된 배정 CPU를 기록해야 한다(SHALL). | CBS-001/005 | T | P1 |
| **SRS-008-FR-N-003** | TIMPANI-N은 테이블 적용 시 식별되지 않은 L2 태스크 이름을 **미해결 목록**에 보관하고, `T_bind`(REQ-008 TIM-006) 이내 주기로 재식별을 시도하여 발견 즉시 FR-N-002를 수행해야 한다(SHALL). 재식별은 프로세스 및 스레드(`/proc/<pid>/task/<tid>/comm`) 모두를 대상으로 한다. | DET-006 | T, M | P1 |
| **SRS-008-FR-N-004** | TIMPANI-N은 식별된 L2 태스크의 종료(TID 소멸)를 감지하여 BPF 상태를 정리하고 해당 이름을 미해결 목록으로 되돌려야 한다(SHALL). | DET-006, RTU-004 | T | P1 |
| **SRS-008-FR-N-005** | 워크로드 제거(STOP 또는 테이블에서 소멸) 시 TIMPANI-N은 해당 L2 태스크의 CBS 상태·태스크 메타·통계·미해결 항목을 모두 제거하고 스케줄링 클래스를 `SCHED_OTHER`로 원복해야 한다(SHALL). 테이블을 다시 적용할 때 변하지 않은 L2 엔트리의 BPF 상태(Job 상태, 예산, 카운터)는 건드리지 않아야 하고(SHALL), 파라미터가 바뀐 엔트리는 제거 후 재등록으로 처리한다(P1: 진행 중인 Job의 관측은 끊길 수 있다). | RTU-004 | T | P1 |
| **SRS-008-FR-N-006** | 테이블 적용 과정은 gRPC 수신 스레드를 1 hyperperiod 이상 블로킹하지 않아야 하며(SHOULD), TT 슬롯 집합·오프셋·hyperperiod·epoch가 동일한 테이블(L2 엔트리만 변경)에서는 Timer Master 스레드를 재시작하지 않아야 한다(SHOULD, DDR-008 D-17(b)). | RTU-002, TIM-007 | M | P1* |
| **SRS-008-FR-N-025** | 테이블 적용 과정은 실행 중인 L1 TT 활성화를 누락시키지 않아야 한다(SHALL). FR-N-001의 증분 적용과 함께 구현한다. | RTU-002, TIM-007 | M | P2 |
| **SRS-008-FR-N-007** | TIMPANI-N은 L2 태스크의 식별 수단으로 P1에서는 comm 이름(15바이트 이하, 노드 내 유일 — FR-O-003)을 사용하되, 식별 로직을 교체 가능한 단일 모듈(TaskRegistry)에 격리해야 한다(SHALL). | A-03, REQ-008 NFR-004 | A | P1 |

### 3.5 보충 및 폴트 집계 (N)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-N-010** | TIMPANI-N은 스로틀된 L2 서버가 보충 윈도우의 끝(`replenish_at_ns`)에 도달했을 때, BPF 경로의 다른 활동과 무관하게 배정 CPU의 재스케줄을 유발하여 BPF가 그 서버를 승격하게 하는 **백업 보충 경로**를 제공해야 한다(SHALL). 백업 경로는 서버 상태(잔여 예산, 윈도우)를 직접 쓰지 않으며(SHALL), 상태 변경은 BPF가 배정 CPU의 런큐 잠금 아래에서만 수행한다. 불필요한 재스케줄 유발은 무해해야 한다(멱등). | PRT-003 | T, M | P1 |
| **SRS-008-FR-N-011** | 백업 보충 경로는 RT 임계 경로 규칙(gRPC·힙 할당·블로킹 I/O 금지)을 준수해야 하며(SHALL), 실행 우선순위는 Timer Master보다 낮아야 한다(SHALL). | NFR-002 | A | P1 |
| **SRS-008-FR-N-012** | TIMPANI-N은 BPF로부터 수신한 `DMISS` 이벤트를 `(workload, task)`별로 계수하고, 누적 횟수가 `current_limit`을 초과할 때 보고해야 한다(SHALL, DDR-012 규칙 유지). `DMISS` 보고에는 REQ-008 PRT-007의 규칙(해당 Job의 활성화 시각 − $T_s$ 이후에 같은 source의 MIT 위반 관측)에 따라 MIT 위반 연관 표식을 포함해야 하고, BPF가 남긴 예산 이월 표식(FR-B-017)을 그대로 전달해야 한다(SHALL). | PRT-002/007 | T | P1 |
| **SRS-008-FR-N-013** | TIMPANI-N은 `BUDGET_EXCEED`를 서버당 Throttle 에피소드별 최대 1건으로 집계하여 보고해야 한다(SHALL). 동일 에피소드 내 중복 이벤트는 카운터만 증가시킨다. | PRT-006 | T | P1 |
| **SRS-008-FR-N-014** | TIMPANI-N은 MIT 위반을 Event source별로 계수하고, source당 $T_s$ 윈도우마다 최대 1건의 집계 보고(위반 횟수 포함)를 TIMPANI-O로 전송해야 한다(SHALL). 계수의 원천은 **도달 기준**(어댑터 M의 누적 카운터, FR-L-007)이 있으면 그것을, 없으면 **활성화 기준**(BPF `MIT_VIOLATION` 이벤트, FR-B-006)을 사용하며, 같은 위반을 두 원천에서 중복 계수하지 않아야 한다(SHALL): source가 마커 활성으로 바뀌면 그 뒤로는 도달 기준만 쓴다. 도달 기준의 기준값은 source의 슬롯을 활성화하기 직전에 잡고, 마커 활성 source의 활성화 기준 이벤트는 전달하지 않는다. 바인딩된 뒤 처음부터 통지를 쓰는 source에서는 빠지거나 두 번 세는 위반이 없어야 한다(SHALL). 표준 깨움만 쓰다가 도중에 통지를 시작한 source는 전환 직전 한 수집 주기(100 ms) 안의 활성화 기준 위반이 계수에서 빠질 수 있다(중복은 없다). | PRT-004/005 | T | P1 |
| **SRS-008-FR-N-015** | TIMPANI-N은 폴트 보고 시 `task_id`, `node_id`, `timestamp_ns`를 채워야 한다(SHALL, 현행 미설정 보완). | IF-004 | T | P1 |

### 3.6 관측성 (N)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-N-020** | TIMPANI-N은 L2 태스크별로 다음 카운터를 유지해야 한다(SHALL): Job 활성화 수, 완료 수, DMISS 수, BUDGET_EXCEED 에피소드 수, MIT 위반 수, 보충 수, 배정 외 CPU 실행 수(0이어야 함). 어댑터 M을 쓰는 Event source에 대해서는 Event 도달 수·소비 수·미소비 수도 유지해야 한다(SHALL). Job당 예산 사용량의 최근값과 최댓값도 유지해야 한다(SHALL). | OBS-001, CBS-005, MOD-007 | T | P1 |
| **SRS-008-FR-N-021** | TIMPANI-N은 L2 태스크별 응답 시간 $R$과 깨움 지연 $L_{wake}$의 히스토그램(최소 10 µs 해상도, p50/p99/p99.9/p99.99 산출 가능)을 유지해야 한다(SHALL). | OBS-001 | T | P1 |
| **SRS-008-FR-N-022** | TIMPANI-N은 job 단위 $(t_{act}, t_{start}, t_{done}, \text{cpu}, \text{budget\_used})$ 레코드를 검증 프레임워크가 구독할 수 있는 트레이스 채널로 노출해야 한다(SHALL). 트레이스 비활성 시 추가 비용은 0에 가까워야 하며, 활성 시에도 REQ-008 TIM-008 내여야 한다. | OBS-002, TIM-008 | T, M | P1 |
| **SRS-008-FR-N-023** | TIMPANI-N은 FR-N-020/021의 요약을 `NodeStatus`로 주기 보고해야 한다(SHOULD). 보고 주기는 설정 가능하며 기본 1 s이다. | OBS-003 | T | P2 |
| **SRS-008-FR-N-024** | Timer Master 지터 통계는 stdout 외에 FR-N-022 트레이스 채널로도 노출되어야 한다(SHOULD, TIM-007 A/B 비교용). | TIM-007 | T | P1 |
| **SRS-008-FR-N-026** | TIMPANI-N은 어댑터 M을 쓰는 Event source의 통계 스냅샷 `{event_seq(도달), consumed_seq(소비), pending, mit_violations, last_t_arr}`를 주기적으로(기본 100 ms) 수집하여 FR-N-022 트레이스 채널로 노출해야 한다(SHALL). 누적 카운터이므로 수집 주기 사이의 변화가 유실되지 않아야 한다. | OBS-001/002, MOD-007 | T | P1 |

### 3.7 Job 상태 및 활성화/완료 감지 (BPF)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-B-001** | BPF는 L2 태스크별 job 상태 `{WAITING, ACTIVE}`를 유지해야 한다(SHALL). 초기 상태는 `WAITING`이다. | MOD-002/003 | T | P1 |
| **SRS-008-FR-B-002** | BPF는 `WAITING` 상태 태스크의 wakeup에서만 `activation_ns`를 기록하고 상태를 `ACTIVE`로 전이해야 한다(SHALL). `ACTIVE` 상태에서의 wakeup(mid-job 재깨움, 선점 후 재개)은 `activation_ns`를 변경하지 않아야 한다(SHALL). | DET-001, MOD-003 | T | P1 |
| **SRS-008-FR-B-003** | BPF는 `ACTIVE` 태스크가 **wait point로 분류되는 블로킹**으로 수면에 들어갈 때(`ops.quiescent`, `SCX_DEQ_SLEEP`) 완료로 판정하여 $t_{done}$을 기록하고 상태를 `WAITING`으로 전이해야 한다(SHALL). 선점·슬라이스 만료(`ops.stopping(runnable=true)`)는 완료가 아니며, `ops.stopping(runnable=false)`는 예산 차감만 수행한다(SHALL). 수면 없이 이어지는 back-to-back Job(R3)은 어댑터 K에서 하나의 관측 구간으로 병합되며, 이 구간에 대한 DMISS 판정은 마감 실패를 놓치지 않지만 후행 Job에 거짓 DMISS를 낼 수 있다(REQ-008 §2.4 K-(c)). | DET-002 | T | P1 |
| **SRS-008-FR-B-004** | wait point 분류 규칙은 결정적이어야 하며 휴리스틱 난수를 포함하지 않아야 한다(SHALL). **P1 규칙**: 마커 사용 여부와 무관하게 L2 태스크의 **모든** 자발적 수면을 wait point 진입(완료)으로 본다. 알려진 한계: mid-job blocking이 Job을 분할하며 분할된 조각의 응답 시간은 과소 측정된다 — REQ-008 A-06이 전제. **P2 규칙**: 마커 wait point 식별(FR-L-001의 대기만 완료로 인정), 블로킹 시스템 콜 분류(이벤트 대기 계열 = wait point)와 블로킹 지속 시간 임계(`midjob_block_max_us`, 기본 $D$) 기반 사후 재분류로 정밀화(DDR-008 후속). | DET-005 | T, A | P1* |
| **SRS-008-FR-B-005** | BPF는 job의 최초 실행 개시 시각 $t_{start}$를 `ACTIVE` 전이 후 첫 `running` 콜백에서 기록해야 한다(SHALL). | TIM-001 | T | P1 |
| **SRS-008-FR-B-006** | BPF는 활성화 시 직전 활성화와의 간격이 MIT − `CBS_ACT_TOL_US`보다 짧으면(깨움 지연의 변동을 FR-B-014와 같은 허용 오차로 흡수) `MIT_VIOLATION` 이벤트를 발생시키고 태스크별 위반 카운터를 증가시켜야 한다(SHALL). 이는 어댑터 K의 **활성화 기준 근사**이며, 도달 기준 계수는 FR-L-007(어댑터 M)이 담당한다. 도달 기준 원천이 있는 source에 대해 TIMPANI-N은 이 이벤트를 폴트로 전달하지 않는다(FR-N-014). | PRT-004 | T | P1 |

### 3.8 CBS 집행 (BPF)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-B-010** | BPF는 L2 태스크를 배정 CPU의 L2 큐에만 삽입하고, CPU $k$의 dispatch는 CPU $k$의 L2 큐만 소비해야 한다(SHALL). L2 태스크가 비배정 CPU에서 실행되면 안 된다(SHALL). | CBS-005 | T, M | P1 |
| **SRS-008-FR-B-011** | BPF는 배정 CPU에서 TT가 활성(runnable TT 태스크 존재 또는 TT 슬롯 진행 중)인 동안 L2를 디스패치하지 않아야 하며(SHALL), L2 실행 중 슬라이스를 `min(잔여 예산, 다음 TT 시작까지 남은 시간)`으로 제한해야 한다(SHALL). | CBS-002 | T, M | P1 |
| **SRS-008-FR-B-012** | BPF는 L2 태스크의 매 실행 구간(`running`→`stopping`)의 실제 실행 시간을 1 µs 해상도로 잔여 예산에서 차감해야 한다(SHALL). 절사 오차의 누적을 방지하기 위해 ns 단위 누산 후 µs 변환해야 한다(SHOULD). 틱 해상도에 의한 overrun 한계는 TMG-004(P2에 강화). | CBS-003/006 | T | P1* |
| **SRS-008-FR-B-013** | 잔여 예산이 0이 되면 BPF는 태스크를 Throttled 큐로 이동시키고 보충 시각을 기록해야 하며(SHALL), 이 전이 시점에 `BUDGET_EXCEED` 이벤트를 **에피소드당 1회** 발생시켜야 한다(SHALL). 보충 전까지 Throttled 큐의 태스크는 디스패치되지 않아야 한다(SHALL). | PRT-001, PRT-006 | T | P1 |
| **SRS-008-FR-B-014** | BPF는 **활성화 기준 보충 윈도우**를 적용해야 한다(SHALL, DDR-008 D-04): (a) Job 활성화 시 직전 윈도우가 끝났으면(허용 오차 `CBS_ACT_TOL_US` 이내 포함) 잔여 예산을 $C_s$로 채우고 윈도우를 `[t_act, t_act + T_s)`로 다시 잡는다. 윈도우가 아직 끝나지 않았으면 잔여 예산을 유지한다. (b) 스로틀된 서버는 윈도우가 끝난 뒤 배정 CPU의 첫 dispatch에서(그 시각 `now ≥ replenish_at`) 잔여 예산을 $C_s$로 채우고 `[now, now + T_s)`를 새 윈도우로 잡는다. (c) 활성 상태이고 스로틀되지 않은 서버는 윈도우가 끝나도 예산을 다시 채우지 않는다. 그 결과 다른 서버가 어떤 Job보다 앞서 실행할 수 있는 양은 서버당 $C_s$ 이하이고(FR-O-013의 전제), 길이 $L$인 임의 구간에서 서버의 소비는 $(\lceil L/(T_s - tol) \rceil + 1) \cdot C_s$ 이하다($tol$ = `CBS_ACT_TOL_US`). | ORD-003, CBS-006, PRT-001 | A, M | P1 |
| **SRS-008-FR-B-015** | BPF는 윈도우가 끝난 Throttled 태스크를 L2 큐로 승격하는 경로를 배정 CPU의 dispatch 진입점에 가져야 하며(SHALL), 사용자 공간 백업 경로(FR-N-010)는 그 dispatch를 유발하기만 한다. 승격 스캔은 반복 상한(16)을 가지며, CPU당 L2 태스크 수가 그 상한 이하로 승인되므로(FR-O-013) 윈도우가 끝난 태스크가 스캔에서 누락되지 않는다(SHALL). | PRT-001/003 | T | P1 |
| **SRS-008-FR-B-016** | 동일 CPU에 둘 이상의 L2 태스크가 실행 가능할 때 BPF는 결정적으로 선택해야 한다(SHALL). **P1**: 활성화 시각 순서(FIFO). 선점 후 재삽입되는 Job은 자신의 활성화 시각 순위를 유지하고, 스로틀 후 승격되는 Job은 승격 시각을 순위로 받아 큐의 뒤로 들어간다(SHALL). **P2**: 절대 마감 오름차순(EDF, DDR-008 후속). | ORD-002 | T | P1* |
| **SRS-008-FR-B-017** | BPF는 직전 윈도우가 끝나기 전의 활성화(MIT 위반 활성화, 또는 이전 위반·예산 초과의 여파로 윈도우가 밀려 있는 상태의 활성화)에 새 예산을 주지 않아야 하며(SHALL, **P1 정책 "잔여 예산 내 흡수"**), 그 Job에 **예산 이월 표식**을 남겨야 한다(SHALL). 그 Job이 잔여 예산을 다 쓰면 윈도우 끝까지 스로틀되고 FR-B-014 (b)에 따라 보충된다. 어떤 경우에도 한 보충 윈도우 안에서 서버의 CPU 소비가 $C_s$(+ TMG-004의 overrun 한계)를 초과하지 않아야 한다(SHALL). 선언(MIT, Job당 차감 실행 시간 < $C_s$)을 지켜 온 서버의 활성화는 항상 온전한 $C_s$로 시작해야 한다(SHALL). 위반이 있었던 서버는 이후 Job들의 여유($C_s$ − 실제 실행 시간)가 빌려 쓴 예산을 채우면 이 상태로 돌아온다(DDR-008 D-04). | PRT-001/004/007, ORD-003 | T, M | P1 |

### 3.9 폴트 감지 (BPF)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-B-020** | BPF는 완료 시 $R = t_{done} − t_{act} > D$ 이면 `DMISS` 이벤트를 1회 발생시켜야 한다(SHALL). 이벤트에는 $t_{act}$, 절대 마감, $t_{done}$, CPU, 그리고 해당 job의 MIT 위반 여부 또는 Throttle 경유 여부 표식을 포함해야 한다(SHALL). | PRT-002/007 | T | P1 |
| **SRS-008-FR-B-021** | BPF는 `ACTIVE` 상태에서 절대 마감이 경과한 것을 `stopping`에서 관측하면 **조기 `DMISS`**를 1회 발생시킬 수 있다(MAY). 조기 발생한 job에 대해서는 완료 시 중복 발생시키지 않아야 한다(SHALL). | PRT-002 | T | P1 |
| **SRS-008-FR-B-022** | BPF 폴트 이벤트 구조는 유형 필드에 `DMISS`, `BUDGET_EXCEED`, `MIT_VIOLATION`을 구분 표현해야 하며(SHALL), `WAKEUP_LATENCY_EXCEED`, `WATCHDOG` 값을 예약해야 한다(SHOULD). | PRT-005 | T | P1 (예약 활성화는 P2) |
| **SRS-008-FR-B-023** | BPF 폴트 이벤트는 ringbuf 공간 부족 시 드롭 카운터를 증가시켜야 하며(SHALL), 드롭이 발생해도 스케줄링 동작에 영향을 주지 않아야 한다(SHALL). | NFR-002 | T | P1 |

### 3.10 애플리케이션 마커 인터페이스 (LIB)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-FR-L-001** | `libttsched`는 L2 태스크용 **명시적 wait point API**(wait)를 제공해야 한다(SHALL). (a) 미소비 Event가 없으면 다음 도달까지 블로킹하며, 이 블로킹은 BPF에 의해 항상 wait point로 분류된다(FR-B-004의 P1 규칙). (b) 복귀 시 이번 Job이 소비할 Event 수 `n_events ≥ 1`(직전 복귀 이후 도달한 수)을 반환한다(R4). (c) 미소비 Event가 있으면 블로킹 없이 복귀한다(R3). (d) 수락된 통지(FR-L-002가 0을 반환한 통지)에 대해서는 어떤 도달·대기 순서에서도 Event가 유실되거나 중복 전달되지 않아야 한다(SHALL). 보장 범위는 TIMPANI-N 데몬이 살아 있는 동안이다: source가 테이블에서 빠졌다가 다시 들어와도 미소비 Event는 보존된다(앱은 자신의 큐를 그대로 둔다). 데몬이 종료·재시작하면 계수는 0에서 다시 시작하며, 소비 태스크는 새 데몬의 SHM 객체에 처음 붙는 wait에서 **재설정 코드(−2)**를 한 번 받아 자신의 큐를 비운다. source당 wait 호출자는 하나(그 `task_id`를 comm으로 가진 스레드)여야 한다. (e) 데몬 종료·source 비활성화·쓰기 불가 매핑 등으로 대기를 계속할 수 없으면 −1을 반환한다(재초기화 후 재시도, 앱 큐는 유지, FR-L-005). | DET-004(a), MOD-007 | T | P1 |
| **SRS-008-FR-L-002** | `libttsched`는 지정 Event source에 **Event 도달을 통지하는 API**(notify)를 제공해야 한다(SHALL). 호출마다 `event_seq`를 1 증가시키고 도달 시각 $t_{arr}$를 기록하며(인자 0이면 호출 시각), 소비 태스크가 대기 중이면 깨운다. 호출자는 같은 프로세스의 임의 스레드, 또는 SHM 쓰기 권한을 가진 같은 호스트의 프로세스다(OI-15). source가 등록되지 않았거나 SHM이 준비되지 않아 통지를 수락할 수 없으면 −1을 반환하고 Event를 계수하지 않는다(SHALL). API는 미들웨어 식별자를 인자로 받지 않는다(SHALL). | DET-004(b), MOD-006/007 | T | P1 |
| **SRS-008-FR-L-003** | 마커 API 없이 표준 블로킹 호출만 사용하는 L2 태스크도 FR-B-004의 P1 규칙에 의해 지원되어야 하며, 마커 API 사용은 선택이어야 한다(SHALL). | DET-003 | T, D | P1 |
| **SRS-008-FR-L-004** | 마커 API는 C ABI, 스레드 안전, 힙 할당 없음, 호출당 비용 ≤ REQ-008 TIM-008을 만족해야 한다(SHALL). wait의 대기를 제외한 모든 호출은 소수의 atomic 연산 + 최대 1회 futex 시스템 콜 이내여야 한다(SHALL, REQ-008 IF-005; 설계 목표 atomic 연산 8회 이내). 같은 Event source에 대한 notify의 동시 호출은 Event를 유실시키지 않아야 하고 데이터 경합을 일으키지 않아야 한다(SHALL). 도달 기준 MIT 계수의 정확성은 source당 notify가 직렬화된 경우에 보장한다. | IF-005 | M | P1 |
| **SRS-008-FR-L-005** | 마커 API는 기존 TT API(`ttsched_init`, `ttsched_wait_next_period`)와 같은 SHM 객체와 초기화 경로를 공유하고, 데몬 종료·source 비활성화 시 같은 오류 코드 규약(−1 → 재초기화 후 재시도)으로 복귀해야 한다(SHALL). 데몬 재시작으로 Event 계수가 초기화된 경우는 −1과 구분되는 코드(−2)로 소비 태스크에 한 번 알려야 한다(SHALL). 테이블을 다시 발행해도 유지되는 Event source에 대한 통지·대기는 재발행 때문에 중단되거나 Event를 잃지 않아야 한다(SHALL). | NFR-003, MOD-007 | T | P1 |
| **SRS-008-FR-L-006** | `examples/someip/libtimpani_rt`의 L2 관련 기능(`timpani_evt_*`, `timpani_cbs_*`)은 FR-L-001/002의 **앱 측 어댑터**로 축소되거나 제거되어야 하며(SHALL), TIMPANI 코어와 별도의 SHM·폴트 판정·서버 키 해시를 갖지 않아야 한다(SHALL). | §10-15, §12 | T | P1 |
| **SRS-008-FR-L-007** | `libttsched`는 notify 시 직전 도달과의 간격이 해당 source의 MIT(TIMPANI-N이 SHM에 게시) 미만이면 **도달 기준 MIT 위반 카운터**를 증가시켜야 한다(SHALL). 라이브러리는 source별 누적 값(도달 수, 소비 수, MIT 위반 수)과 최근 시각(마지막 도달, 마지막 MIT 위반)을 TIMPANI-N이 읽을 수 있게 유지해야 한다(SHALL). 라이브러리는 폴트를 판정·보고하지 않으며 계수만 한다. | PRT-004, OBS-001 | T | P1 |
| **SRS-008-FR-L-008** | `libttsched`는 태스크 식별자로 **Event source 핸들**을 얻는 API(open)를 제공해야 한다(SHALL). 핸들은 테이블 갱신과 TIMPANI-N 재시작 후에도 재획득 없이 유효해야 하며, 대상 source가 아직 등록되지 않았으면 notify가 오류를 반환하고 등록 후 자동으로 동작해야 한다(SHALL). | DET-004(c) | T | P1 |

---

## 4. 타이밍 및 결정론 요구사항 (Timing & Determinism)

> REQ-008 §4.6의 후보값을 계승한다. 모든 항목은 **Isolated CPU, PREEMPT_RT, 정격 부하, 1시간 연속 관측**을 기본 조건으로 하며, 측정은 FR-N-022 트레이스 채널을 통해 검증 프레임워크가 수행한다. 목표값 확정 전까지 "(후보)"로 표기한다. 모든 항목은 P1에서 측정을 시작한다.

| ID | 요구사항 | 목표 | 조건 | 측정 |
|:--|:--|:--|:--|:--|
| **SRS-008-TMG-001** | Gap·유휴 상태 깨움 지연 $L_{wake} = t_{start} − t_{act}$ | ≤ 50 µs @ p99.99 (후보) | 배정 CPU에 runnable TT 없음, 타 L2 없음 | job 레코드 히스토그램 |
| **SRS-008-TMG-002** | TT 점유 중 도달 시 깨움 지연 | ≤ $B_{tt}(k) + 50$ µs @ p99.99 (후보) | 도달 시 TT 활성 | job 레코드 + 테이블의 $B_{tt}$ |
| **SRS-008-TMG-003** | 백업 보충 지연: 스로틀된 서버의 `replenish_at_ns` → 태스크 실행 가능(L2 큐 승격 + CPU 재스케줄) | ≤ 100 µs @ p99.99 (후보) | 배정 CPU가 유휴이고 보충 시각 이후 어떠한 enqueue/dispatch도 발생하지 않는 조건 포함 | Replenisher 발화 시각 vs BPF 승격 시각 |
| **SRS-008-TMG-004** | 서버당 $T_s$ 윈도우 내 예산 초과(overrun) | P1: ≤ 1 scheduler tick (HZ=1000 기준 1 ms); P2: ≤ 50 µs (HRTICK 채택 시, 후보) | 연속 CPU 소모 job | BPF exec 누산 vs $C_s$ |
| **SRS-008-TMG-005** | DMISS 커널 감지 시점 | Job 완료 처리와 동시(완료 콜백 안에서 동기 발행). 조기 감지(FR-B-021, MAY)를 쓰면 마감 경과 후 첫 정지 시점 | FR-B-020/021. 스로틀·선점으로 완료가 늦어지면 감지도 늦어진다 | ringbuf 타임스탬프 − $t_{done}$ |
| **SRS-008-TMG-006** | DMISS TIMPANI-O 도달 지연 | ≤ 커널 감지 + 200 ms (후보) | FaultMonitor 폴링 100 ms 포함 | O 수신 로그 시각 |
| **SRS-008-TMG-007** | 지연 식별 `T_bind` | ≤ 1 s (후보) | 테이블 적용 후 스레드 생성 | 스레드 생성 시각 vs BPF 등록 시각 |
| **SRS-008-TMG-008** | L1 무간섭: L2 유/무 A/B에서 TT 활성화 지터 p99.99 차이 | ≤ 10 µs (후보) | 동일 TT 테이블, L2 정격·폭주(MIT/3) 양 조건 | Timer Master 지터 트레이스 |
| **SRS-008-TMG-009** | Event·Job 계수 정확성(어댑터 M): 100,000회 도달에 대해 (a) 도달 계수(`event_seq`) 누락·중복, (b) 소비 누락(도달 수 − source의 누적 소비 수), (c) MIT 준수·$R \le D$·A-06 준수 구간에서 Job 레코드 수 ≠ 도달 수 | 각 0건 | 어댑터 M 사용. 어댑터 K만 쓰는 앱은 Event를 계수하지 않으므로 대상이 아니며, Job 레코드 수와 생성기 기록의 오프라인 비교로만 확인한다 | Event source 통계 + job 레코드 vs 생성기 로그 대조 |
| **SRS-008-TMG-010** | 배정 외 CPU 실행 | 0회 / 1시간 | 정격 + 폭주 | `sched_switch` 트레이스 |
| **SRS-008-TMG-011** | Event 감지 오버헤드(이벤트당 추가 CPU 시간, BPF + 마커) | ≤ 2 µs (후보) | 정격 | `bpf_prog` run_time_ns / run_cnt, 마커 마이크로벤치 |
| **SRS-008-TMG-012** | 폴트 보고 속도: `BUDGET_EXCEED` 보고 건수 | ≤ 1건 / 서버 / Throttle 에피소드 | 지속 폭주 | O 수신 카운트 |
| **SRS-008-TMG-013** | 런타임 L2 추가·제거 중 기존 L1/L2 활성화 누락 | 0건 / 10회 반복 (P2) ; P1: 데몬 재시작 0, 잔존 상태 0 | UC-04 | job·TT 레코드 연속성 |

---

## 5. 인터페이스 요구사항 (Interface Requirements)

> 필드 번호·타입의 최종 확정은 DDR-008/LLD-008에서 하며, 본 절은 **필수 정보 요소**와 호환 규칙을 정의한다.

### 5.1 Pullpiri → TIMPANI-O (`schedinfo.proto`) — IF-PO

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-IF-PO-001** | `TaskInfo`에 MIT 전용 정보 요소(`min_inter_arrival_us`, µs, 32비트 이상)를 **신규 필드 번호**로 추가해야 한다(SHALL). 기존 필드 1~10의 번호·타입은 변경하지 않는다(SHALL). | IF-001, NFR-005 | T | P1 |
| **SRS-008-IF-PO-002** | `TaskInfo`에 태스크 단위 시간 클래스 정보 요소를 추가해야 하며(SHALL), 값 0은 "워크로드 클래스 상속"이어야 한다(SHALL, proto3 기본값 호환). | IF-002 | T | P2 |
| **SRS-008-IF-PO-003** | `Response`에 거부 사유 코드(enum) 및 메시지 정보 요소를 추가해야 한다(SHOULD, REQ-008 IF-006). `status` 필드 의미(0 성공 / 비0 실패)는 유지한다. | IF-006 | T | P2 |
| **SRS-008-IF-PO-004** | `schedinfo.FaultType`에 `BUDGET_EXCEED`, `MIT_VIOLATION`을 추가하고 `WATCHDOG`을 예약해야 하며(SHALL; `WATCHDOG`의 사용은 P2), 값은 `node_control.FaultType`의 같은 유형과 동일 번호를 사용해야 한다(SHOULD). `node_control`에만 있는 예약 유형(`WAKEUP_LATENCY_EXCEED`)은 P2에 추가한다. | IF-004 | T | P1 |
| **SRS-008-IF-PO-005** | `FaultInfo`에 `timestamp_ns`, 집계 카운트(위반 횟수), MIT 위반 연관 표식 정보 요소를 추가해야 한다(SHOULD). | PRT-007 | T | P1 |
| **SRS-008-IF-PO-006** | 기존 클라이언트(필드 1~10만 사용, `temporal_class` 워크로드 단위)의 요청은 변경 없이 수용되어야 한다(SHALL). | NFR-005 | T | P1 |

### 5.2 TIMPANI-O → TIMPANI-N (`node_control.proto`) — IF-ON

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-IF-ON-001** | L2 엔트리는 `LayerConfig.model = EVENT_DRIVEN`인 레이어에 실려야 하며(SHALL), TT 레이어(`TIME_TRIGGERED`)에는 `cbs_entries`가 비어 있어야 한다(SHALL). TIMPANI-N은 레이어 모델 값을 읽어 처리 경로를 분기해야 한다(SHALL). | IF-003 | T | P1 |
| **SRS-008-IF-ON-002** | `CbsConfig`(또는 후속 명칭)에 MIT 정보 요소를 $T_s$(`period_us`)와 **별도 필드**로 추가해야 한다(SHALL). 전환 기간 동안 MIT 필드가 0이면 $T_s$를 MIT로 해석한다(SHOULD). | IF-003 | T | P1 |
| **SRS-008-IF-ON-003** | `CbsConfig`에 배정 CPU 정보 요소를 명시해야 한다(SHALL, 현행 파티션 cpuset 첫 원소 암묵 사용 제거). CPU 0을 "미지정"과 구분할 수 있도록 필드 존재 여부를 표현해야 한다(SHALL). TIMPANI-N은 필드가 없거나 Isolated CPU가 아닌 엔트리를 적용 오류로 처리한다(SHALL). | CBS-005 | T | P1 |
| **SRS-008-IF-ON-004** | `node_control.FaultType`에 `MIT_VIOLATION`을 추가해야 하며(SHALL), `WAKEUP_LATENCY_EXCEED`를 예약해야 한다(SHOULD). 기존 `WATCHDOG` 값은 유지하며 P2까지 사용하지 않는다. `FaultInfo`에 집계 카운트와 MIT 연관 표식 정보 요소를 추가해야 한다(SHALL, PRT-007). | PRT-005/007 | T | P1 |
| **SRS-008-IF-ON-005** | `NodeStatus`에 L2 태스크별 통계 요약(FR-N-020/021) 정보 요소를 추가해야 한다(SHOULD). | OBS-003 | T | P2 |
| **SRS-008-IF-ON-006** | `HierarchicalScheduleTable.table_id`는 발행마다 단조 증가하는 식별자여야 한다(SHOULD, 현행 고정 `"table_v1"` 개선; `TableApplied` 상관을 위함). | RTU-002 | T | P1 |

### 5.3 TIMPANI-N ↔ 애플리케이션 (SHM / `libttsched`) — LIB

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-IF-L-001** | L2 태스크도 SHM 태스크 슬롯을 할당받아야 하며(SHALL, 현행 TT 전용 해제), 유지되는 태스크의 슬롯 인덱스와 Event source의 누적 값은 테이블을 다시 발행해도 바뀌지 않아야 한다(SHALL). 한 번 L2 `task_id`에 배정된 슬롯은 데몬이 살아 있는 동안 다른 `task_id`에 재사용하지 않아야 하며(SHALL), 같은 `task_id`가 다시 등록되면 같은 슬롯과 누적 값을 이어 쓴다. 최대 태스크 수는 REQ-008 NFR-001(노드당 ≥ 32 L2 + 기존 TT)을 수용해야 한다(SHALL). | DET-004, NFR-001 | T | P1 |
| **SRS-008-IF-L-002** | SHM 슬롯은 L2 Event source용으로 최소 다음 정보 요소를 가져야 한다(SHALL): 도달 카운터(`event_seq`), 대기·깨움용 futex 워드(도달 카운터와 분리), 소비 카운터, 마지막 도달 시각 $t_{arr}$(ns), 도달 기준 MIT 위반 카운터, 마지막 MIT 위반 도달 시각, MIT(TIMPANI-N 게시), 슬롯 종류(미사용/TT/EVT). 레이아웃 변경은 `magic`/버전 필드로 식별 가능해야 한다(SHALL). 헤더는 SHM 객체의 인스턴스 식별자(데몬이 객체를 만들 때마다 다른 값)를 가져야 한다(SHALL): 소비 태스크가 데몬 재시작을 식별하는 수단이다. | DET-004, MOD-007 | T | P1 |
| **SRS-008-IF-L-003** | 마커 API 시그니처(LLD-008 확정): `ttsched_evt_t ttsched_evt_open(const char *task_id)` — Event source 핸들 획득(≥ 0, 실패 −1); `int ttsched_evt_notify(ttsched_evt_t src, uint64_t t_arr_ns)` — 도달 통지(0 정상, −1 source 미등록/SHM 미준비); `int ttsched_evt_wait(void)` — wait point. 반환값 n ≥ 1은 이번 Job이 소비할 Event 수, −1은 재초기화 필요(`ttsched_wait_next_period`와 동일 규약). | FR-L-001/002/008 | T | P1 |

### 5.4 TIMPANI-N ↔ 검증 프레임워크 — VF

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-IF-V-001** | job 레코드 트레이스 채널은 레코드당 최소 `{workload_id_hash, task_id_hash, job_seq, cpu, t_act, t_start, t_done, budget_used_us, flags(mit_violation, throttled, early_dmiss, marker_used, dmiss, task_exit, carryover)}`를 제공해야 한다(SHALL). 필드 순서와 플래그 값의 정본은 LLD-008 §4.1의 `struct JobRecord`와 `JOB_FLAG_*`이며, 수집기는 출력할 때 `node_id`와 태스크 이름을 덧붙인다. 레코드는 **커널이 관측한 Job 구간**(어댑터 K)이며 REQ-008 §2.4의 P1 적합 조건에서 모델의 Job과 일치한다. `n_events`는 레코드에 없고 IF-V-004의 누적 소비 수로 확인한다. | OBS-002 | T | P1 |
| **SRS-008-IF-V-002** | 트레이스 채널은 런타임에 활성/비활성 전환이 가능해야 하며(SHALL), 비활성 시 BPF 경로의 추가 분기 1회 이하여야 한다(SHOULD). | TIM-008 | M | P1 |
| **SRS-008-IF-V-003** | 검증 프레임워크가 이벤트 생성 시각(생성기 측 $t_{arr}$)과 TIMPANI 측 $t_{act}$를 대조할 수 있도록 공통 단조 시계(`CLOCK_MONOTONIC`) 기준을 사용해야 한다(SHALL). | OBS-002 | A | P1 |
| **SRS-008-IF-V-004** | 트레이스 채널은 Event source 통계 레코드 `{workload_id_hash, task_id_hash, event_seq, consumed_seq, pending, mit_violations, last_t_arr, 수집 시각}`를 제공해야 한다(SHALL, FR-N-026). 검증 프레임워크는 이 레코드와 job 레코드, 생성기 로그를 공통 단조 시계로 대조하여 Event↔Job 대응과 $t_{rel}$ 기준 응답 시간을 산출한다. | OBS-002, MOD-007 | T | P1 |

---

## 6. 비기능 요구사항 (Non-Functional Requirements)

| ID | 요구사항 | 상위 | 검증 | Phase |
|:--|:--|:--|:-:|:-:|
| **SRS-008-NFR-001** | 노드당 L2 태스크 ≥ 32, CPU당 ≥ 8(상한 16, FR-B-015). BPF 맵 용량·SHM 슬롯(TT와 L2 합계 64: L2 32 + TT 32)·N 자료구조가 이를 수용해야 한다(SHALL). | NFR-001 | T | P1 |
| **SRS-008-NFR-002** | RT 임계 경로(BPF ops, Timer Master, Replenisher, 마커 API)는 gRPC·힙 할당·블로킹 I/O를 포함하지 않아야 한다(SHALL). BPF 경로에 추가되는 명령은 검증기(verifier) 한계 내여야 하며 반복문은 상한이 명시되어야 한다(SHALL). | NFR-002 | T, A | P1 |
| **SRS-008-NFR-003** | L2 엔트리가 없는 테이블에서 TIMPANI-N/BPF의 동작·타이밍은 변경 전과 동일해야 한다(SHALL). 회귀 측정: TT 지터 p99.99 차 ≤ 5 µs. 예외(REQ-008 NFR-003): SHM ABI v2 때문에 `libttsched`를 링크한 TT 앱은 다시 빌드해야 하며, 배포 순서는 LLD-008 §9를 따른다. | NFR-003 | M | P1 |
| **SRS-008-NFR-004** | Event 감지·분류 로직(FR-B-001~006)은 OS 의존 부분(시스템 콜 번호, 커널 콜백)과 정책 부분(상태 머신)을 분리하여 구현해야 한다(SHOULD). | NFR-004 | A | P2 |
| **SRS-008-NFR-005** | 모든 proto 변경은 필드 추가만 허용하고, 생성 코드가 기존 메시지를 역직렬화할 수 있어야 한다(SHALL). | NFR-005 | T | P1 |
| **SRS-008-NFR-006** | 신규 코드(timpani-o, timpani-n C++)는 GTest 단위 테스트를 동반해야 하며, 승인 로직(FR-O-010~014)과 job 상태 머신(FR-B-001~006)은 결정적 입력에 대한 표 기반(table-driven) 테스트를 가져야 한다(SHALL). | 개발 규칙 | T | P1 |
| **SRS-008-NFR-007** | C++ 경로의 테스트가 CI에서 실행되지 않는 현행 제약을 고려하여, 로컬 실행 스크립트와 결과 보고 형식을 테스트 전략에 정의해야 한다(SHOULD). | C-04 | A | P1 |

---

## 7. 제약 및 가정 (Constraints & Assumptions)

REQ-008 §8의 A-01~06, C-01~04를 계승하며 다음을 추가한다.

| ID | 내용 |
|:--|:--|
| **A-07** | sched_ext 콜백(`enqueue/running/stopping`)과 보조 BPF 프로그램(P2: `sys_enter`류 트레이스포인트)이 동일 BPF 객체에 공존할 수 있다(Linux 6.12+). |
| **C-05** | BPF에서 futex 깨움·CPU 재스케줄을 사용자 공간 요청으로 직접 수행할 수 없으므로, 백업 보충은 사용자 공간 스레드가 BPF 프로그램 실행(syscall 타입) 또는 보조 스레드 깨움으로 재스케줄을 유발해야 한다. 가용성은 DDR-008 PoC로 확인한다(REQ-008 OI-9). |
| **C-06** | (REQ-008 C-01과 동일 내용의 SRS 표기) 슬라이스 만료 해상도는 커널 틱(HZ)에 의존한다. TMG-004 P1 값은 HZ=1000을 가정한다. |
| **C-07** | P1 샘플 앱(FCW)은 TT 생산자와 L2 소비자를 **별도 워크로드**로 등록한다(IF-002는 P2). |

---

## 8. 추적성 매트릭스 (Traceability)

### 8.1 REQ-008 → SRS-008

| REQ-008 | SRS-008 |
|:--|:--|
| MOD-001 | FR-O-001 |
| MOD-002 | FR-B-001, FR-B-002, FR-B-003, FR-B-005 |
| MOD-003 | FR-B-001, FR-B-002, FR-B-004 |
| MOD-004 | FR-O-003 |
| MOD-005 | FR-O-003 (MIT > 0) |
| MOD-006 | FR-G-001, FR-L-002, FR-L-006 |
| MOD-007 | FR-L-001, FR-L-002, FR-N-020, FR-N-026, TMG-009 |
| MOD-008 | FR-G-002 |
| DET-001 | FR-B-002 |
| DET-002 | FR-B-003 |
| DET-003 | FR-L-003, FR-B-004 |
| DET-004 | FR-L-001, FR-L-002, FR-L-008, IF-L-001~003 |
| DET-005 | FR-B-004 |
| DET-006 | FR-N-003, FR-N-004, TMG-007 |
| CBS-001 | FR-O-010, FR-O-011, FR-N-002 |
| CBS-002 | FR-B-011 |
| CBS-003 | FR-B-012 |
| CBS-004 | FR-O-012, FR-O-013 |
| CBS-005 | FR-B-010, FR-N-002, IF-ON-003, TMG-010 |
| CBS-006 | FR-B-012, FR-B-014, TMG-004 |
| PRT-001 | FR-B-013, FR-B-015 |
| PRT-002 | FR-B-020, FR-B-021, FR-N-012, TMG-005/006 |
| PRT-003 | FR-N-010, FR-N-011, FR-B-015, TMG-003 |
| PRT-004 | FR-B-006, FR-B-017, FR-L-007, FR-N-014 |
| PRT-005 | FR-B-022, FR-O-020, IF-PO-004, IF-ON-004 |
| PRT-006 | FR-B-013, FR-N-013, TMG-012 |
| PRT-007 | FR-B-020, FR-N-012, IF-PO-005 |
| ORD-001 | FR-O-012, FR-O-014 |
| ORD-002 | FR-B-016, FR-O-013 |
| ORD-003 | FR-B-014, FR-B-016, FR-B-017 |
| TIM-001~008 | TMG-001~004, 005/006, 007, 008, 011 |
| IF-001~006 | IF-PO-001~006, IF-ON-001~006, FR-O-004, FR-L-004 |
| RTU-001~004 | FR-O-005, FR-O-017, FR-O-018, FR-N-001, FR-N-005, FR-N-006, FR-N-025, TMG-013. RTU-003(L2만의 변경을 hyperperiod 경계 대기 없이 즉시 적용, P2 SHOULD)은 FR-N-001의 증분 적용에 포함한다. FR-N-006의 P1* 부분(Timer Master 재시작 생략)은 RTU-002를 P1에서 부분 충족하는 것으로 DDR-008 D-17(b)에 근거한다 |
| OBS-001~003 | FR-N-020~024, FR-N-026, IF-V-001~004, IF-ON-005 |
| NFR-001~005 | NFR-001~005, FR-N-011 |
| AC-008-01~12 | 정량 합격 기준은 TMG-001~013이 구체화한다: AC-008-01↔TMG-009, AC-008-02↔TMG-008, AC-008-03↔TMG-003/004, AC-008-04↔TMG-005/006, AC-008-05↔TMG-001/002, AC-008-06↔TMG-010, AC-008-10↔TMG-007, AC-008-11↔TMG-012. 그 외: AC-008-07↔FR-L-007·FR-N-014·FR-B-017, AC-008-08↔FR-O-005·FR-O-013, AC-008-09↔FR-N-005·TMG-013, AC-008-12↔FR-G-001/002. 요구사항 행의 "상위" 열에 적힌 `AC-08`은 AC-008-08의 약기다 |

### 8.2 SRS-000 연계 (요약)
REQ-008 §6을 그대로 계승한다. 특히 `REQ-FUN-004`(L2 CBS 실행) ↔ FR-B-010~017, `REQ-FUN-013`(feasibility) ↔ FR-O-010~014, `REQ-FUN-014`(이상 징후 분류) ↔ FR-B-020~022/FR-N-012~014/FR-O-020, `REQ-NFR-003`(투명성) ↔ FR-L-003, `REQ-NFR-004`(재시작 복원력) ↔ FR-N-003/004/007.

---

## 9. 오픈 이슈 (Open Issues)

REQ-008 §11의 OI-1~9, OI-16, OI-17을 계승한다(OI-16/17은 P2). 본 문서에서 추가된 항목(REQ-008과 번호 체계 공유):

| # | 이슈 | 영향 | 결정 문서 | Phase |
|:--|:--|:--|:--|:-:|
| OI-10 | (REQ-008 OI-1의 구현 지점 측면) FR-B-004 P2 규칙의 구현 지점(sched_ext 콜백 내 레지스터 판독 vs `sys_enter` 트레이스포인트 병행) 및 시스템 콜 집합 | 커널 호환성, 오버헤드 | DDR-008 후속 | P2 |
| OI-11 | FR-O-013 P1식의 보수성이 FCW 구성(C 2 ms, $B_{tt}$ 2 ms, D 5 ms, 타 L2 없음 → 4 ms + $L_{wake}$ ≤ 5 ms)에서 수용 가능함을 확인; 다중 L2 시 과도한 거부 가능성 | 승인 수용률 | DDR-008 D-09 (FCW 구성 확인, LLD-008 GTest `FifoAdmission_FcwConfigPasses` → 종결; 다중 L2 거부율은 F-3, P2) | P1 |
| OI-12 | FR-B-013 "에피소드당 1회" 보장을 BPF에서 할지 N 집계에서 할지(양쪽 모두 권장) | ringbuf 대역폭 | DDR-008 D-10 (BPF 전이 시 1회 발행 + N 에피소드 키 집계, 이중 방어 → 종결) | P1 |
| OI-13 | `CbsConfig` 명칭 유지 vs `EventEntry` 신설(IF-ON-002/003) | proto 승인 범위 | DDR-008 D-11 (`CbsConfig` 유지, 필드 추가 → 종결) | P1 |
| OI-14 | 마커 API 명칭·헤더 배치(`libttsched` 확장 vs 별도 `libttevent`) | 앱 마이그레이션 | DDR-008 D-03 (`libttsched` 확장, `ttsched_evt_open/notify/wait` → 종결) | P1 |
| OI-15 | `ttsched_evt_notify`가 다른 프로세스의 L2 태스크를 깨울 수 있어야 하는가(SHM 공유 범위·권한) | 어댑터 배치 자유도 | DDR-008 D-03 (P1: 동일 프로세스 또는 SHM 쓰기 권한을 가진 동일 호스트 프로세스 → 종결; 권한 축소 정책은 P2) | P1 |
