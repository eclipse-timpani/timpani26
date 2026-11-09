<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# REQ-008: Event-Driven Workload Support 요구사항 정의서

**날짜:** 2026-10-25 (Last Updated: 2026-11-09)
**상태:** Draft (v2 — Phase 1/2 분할, 코드베이스 갭 분석 반영)
**작성:** Human + AI
**관련 문서:** [SRS-000](SRS-000-timpani-system.md), [DDR-001](DDR-001-workload-model.md), [DDR-002](DDR-002-scheduling-architecture.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md)
**하위 문서:** SRS-008 (소프트웨어 요구사항), DDR-008 (설계 결정), HLD-008 (상위 설계), LLD-008 (상세 설계)

---

## 요약 (Korean Summary)

- **목적**: TIMPANI에 Event-Driven(L2, Sporadic) 워크로드 지원을 추가하기 위한 상위 요구사항을 정의한다. Event는 "L2 태스크를 깨우는 자극"으로만 모델링하며, SOME/IP 등 특정 미들웨어 식별자·의미론은 TIMPANI 코어에 두지 않는다(전송 계층 중립).
- **범위**: Event/Job 개념 모델, 감지(DET), CBS 격리 실행(CBS), 보호·폴트(PRT), 순서(ORD), 정량 타이밍(TIM), 인터페이스(IF), 런타임 갱신(RTU), 관측(OBS), 비기능(NFR) 요구사항과 수용 기준 AC-008-01~12.
- **단계**: Phase 1은 기본 기능(Event 감지, CBS 실행, 최소 폴트 처리, 기본 관측)과 FCW 샘플 앱 완성에 집중하고, Phase 2는 정밀 승인식·EDF·시스템 콜 분류·증분 테이블 갱신 등 고급 기능을 다룬다. 각 요구사항에 P1/P1*/P2를 표기했다.
- **코드베이스 갭**: §10에 현행 `timpani26`(HEAD `4201573`) 대비 17개 갭을 파일 근거와 함께 정리했으며, §12에 PR #50~52 샘플 앱에 대한 가이드(마커 API 전환, SOME/IP 결합 제거)를 포함한다.
- **상태**: Draft v2. TIM 후보값은 RPi5 측정 후 확정(OI-5). 하위 문서 SRS/DDR/HLD/LLD-008이 본 문서를 기준으로 작성되었다.

---

> **문서 범위 및 작성 원칙**
> 본 문서는 TIMPANI의 `008: Event-Driven Workload Support`를 위한 **상위 요구사항 정의서(REQ)**이다. "무엇을, 왜" 보장해야 하는지를 정의하며, "어떻게"는 DDR-008/HLD-008/LLD-008이 다룬다.
>
> **두 가지 설계 원칙**
> 1. **전송 계층 중립(Transport-Neutral)**: TIMPANI 코어(TIMPANI-O/N, BPF, 애플리케이션 라이브러리 계약)는 SOME/IP, DDS, CAN 등 **특정 미들웨어·전송 계층의 식별자나 의미론을 포함하지 않는다.** TIMPANI가 아는 Event는 "L2 태스크를 깨우는 자극"뿐이며, 미들웨어 고유 처리(서비스/인스턴스/이벤트 ID, 구독, 역직렬화)는 애플리케이션 또는 애플리케이션 측 어댑터의 책임이다. 과거 검토 초안이 SOME/IP를 코어 계약에 직접 언급한 점을 본 개정에서 바로잡는다.
> 2. **단계적 접근(Phased)**: **Phase 1**은 현실적이고 단순한 기본 기능(Event 정의·감지, CBS 격리 실행, 최소 폴트 처리, 기본 관측)에 집중하여 `timpani26` 구현과 FCW 샘플 앱 완성을 함께 이끈다. **Phase 2**는 Phase 1 검증 후 고급 기능(정밀 job 경계 분류, EDF 순서, 윈도우 기반 승인, 증분 갱신, 미들웨어 경계 관측, watchdog 등)을 다룬다. 요구사항 ID는 Phase와 무관하게 안정적으로 유지하며, Phase 열로 범위를 표시한다.
>
> **키워드 규약**: **SHALL** = 필수, **SHOULD** = 권장, **MAY** = 선택.
> **단위 규약**: 모든 시간 값은 단위를 명기한다(µs/ms/ns). 정량 요구사항은 백분위수와 관측 구간을 함께 명기한다.

---

## 0. 변경 이력

| 버전 | 날짜 | 변경 내용 |
|:--|:--|:--|
| v1 | 2026-10-25 | Baseline 초안. MOD/DET/CBS/PRT 9개 요구사항, UC-01/02, AC-01~04 |
| v2 | 2026-11-09 | (a) 전송 계층 중립 원칙과 Phase 1/2 분할 원칙 명시. (b) `timpani26` HEAD `4201573` 코드베이스 갭 분석(§1.3, §10) 반영. (c) 신규 요구사항 그룹 ORD/IF/TIM/RTU/OBS/NFR 추가, 기존 ID 유지. (d) 정량 타이밍 요구사항(TIM)과 측정 방법 도입. (e) 수용 기준에 수치·측정법 추가. (f) 범위 외·가정·오픈 이슈 명시. (g) UC-03~06 추가. (h) 선행 샘플 앱(PR #50~#52)과의 관계 및 가이드 방향(§12) |

### 0.1 Phase 정의

| Phase | 목표 | 완료 기준 |
|:--|:--|:--|
| **P1** | Event-Driven 기본 기능: 일반(generic) Event의 활성화/완료 감지, CBS 격리 실행, 배정 CPU 고정, 바운드된 보충, 최소 폴트 분류(DMISS/BUDGET_EXCEED/MIT_VIOLATION), 기본 관측·트레이스, 전송 계층 중립 마커 API. FCW 샘플 앱이 TIMPANI 계약만으로 완성됨 | §5 수용 기준 중 P1 표시 항목 전부 통과 |
| **P2** | 고급 기능: 마커 없는 태스크의 정밀 job 경계 분류, EDF 서버 순서, 윈도우 수요 기반 승인, 태스크 단위 시간 클래스, 증분 테이블 갱신(TT 무교란), NodeStatus 통계, watchdog, 미들웨어 경계 관측, 깨움 지연 폴트, HRTICK 기반 예산 정밀도 | P1 운영 데이터 기반으로 별도 개정 |

---

## 1. 개요 및 문제 정의 (Overview & Problem Statement)

### 1.1 배경
SDV(Software-Defined Vehicle) 환경의 워크로드는 고정 주기로 동작하는 제어 태스크(L1 Periodic)뿐만 아니라, 레이더/라이다 객체 감지, 긴급 제동 트리거, 미들웨어 메시지 수신 등 **외부 이벤트 도달에 의해 비주기적(Sporadic)으로 활성화되는 안전 중요(Safety-Critical) 워크로드(L2)**를 포함한다. 이러한 워크로드는 "언제 실행되는가"가 외부 세계에 의해 결정되므로, TIMPANI는 실행 시각을 미리 정할 수 없고 **(a) 도달 시점을 정확히 포착하고, (b) 도달 이후의 응답 시간을 바운드하며, (c) 예산 초과가 L1 및 다른 L2에 전파되지 않도록 격리**해야 한다.

### 1.2 일반적 Event-Driven 워크로드의 요구 특성 (설계 중립적 정리)
TIMPANI 구현과 무관하게, 안전 중요 Event-Driven 워크로드를 실시간 스케줄러가 지원하려면 다음이 필요하다. 본 문서의 요구사항은 이 목록을 TIMPANI 아키텍처(HSF, sched_ext, TT+CBS)에 투영한 것이다.

| # | 특성 | 설명 | Phase |
|:--|:--|:--|:-:|
| G1 | **Sporadic 태스크 모델** | MIT(최소 도달 간격), WCET, 상대 마감(D)을 선언. D는 **이벤트 도달 시점** 기준이어야 함(디스패치 시점 기준이 아님) | P1 |
| G2 | **도달(arrival) 관측** | 이벤트가 "언제 도달했는가"를 전송 계층과 무관하게 타임스탬프. 관측점이 늦을수록 응답 시간 측정이 낙관적으로 왜곡됨 | P1(커널 wakeup) / P2(미들웨어 경계) |
| G3 | **Job 경계 정의** | 1회 이벤트 처리의 시작/끝이 명확해야 응답 시간·마감 판정이 성립. 처리 도중의 내부 블로킹(뮤텍스/I/O)을 새 job으로 오인하면 안 됨 | P1(마커 기반) / P2(마커 없는 정밀 분류) |
| G4 | **대역폭 격리** | 예산(C_s)/주기(T_s) 서버로 CPU 사용량을 상한. 이벤트 폭주(MIT 위반)·실행 시간 초과가 타 워크로드에 전파되지 않아야 함(FFI) | P1 |
| G5 | **승인 제어** | 이용률 검사만으로는 D < MIT인 경우의 마감 보장이 불충분. 상위 계층(L1 TT 슬롯)에 의한 최대 블로킹과 동일 CPU 내 타 서버 간섭을 반영한 응답 시간 바운드 필요 | P1(단순 블로킹 합) / P2(윈도우 수요) |
| G6 | **깨움 지연 바운드** | 도달 → 실행 개시 지연을 백분위수로 바운드. CPU가 유휴/Gap일 때와 상위 계층이 점유 중일 때를 구분 | P1 |
| G7 | **보충 결정론** | 예산 소진 후 보충 시각에 "확실히" 실행 가능 상태로 복귀해야 함. 다른 활동에 기대는 지연 보충은 바운드가 없음 | P1 |
| G8 | **MIT 위반 처리** | MIT보다 빠른 도달을 감지하고, 격리 유지(예산 내 흡수/지연/폐기)와 보고를 분리 | P1 |
| G9 | **동일 CPU 다중 서버 순서** | 여러 Sporadic 서버가 한 CPU를 공유할 때의 우선 순서(FIFO/EDF)가 마감 보장식에 들어가야 함 | P1(FIFO) / P2(EDF) |
| G10 | **이벤트 침묵 감시** | 기대 도달이 없을 때(센서 탈락)를 감지하는 감시(watchdog) 수단 | P2 |
| G11 | **런타임 추가/제거** | Sporadic 워크로드의 추가/제거가 기존 L1/L2 타이밍을 교란하지 않아야 함 | P1(추가/제거 가능) / P2(무교란 보장) |
| G12 | **관측성** | 도달/활성화/시작/완료 타임스탬프, 예산 사용량, 위반 카운터를 검증 프레임워크가 수집 가능해야 함 | P1 |
| G13 | **애플리케이션 식별/복원력** | 이벤트 처리 스레드가 늦게 생성되거나 재시작되어도 정책이 재적용되어야 함 | P1(재스캔) / P2(cgroup 메타데이터) |

### 1.3 현재 한계점 및 해결 과제
기존 설계([DDR-001](DDR-001-workload-model.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md))와 `timpani26` 코드베이스는 L2 워크로드에 CBS 예산을 할당·차감하는 메커니즘을 **이미 end-to-end로 갖추고 있다**(TemporalClass → Mechanism::CBS → `CbsConfig` → BPF `cbs_map` → 예산 차감/스로틀링). 그러나 **Event-Driven 워크로드의 근본 동작 모델**에 대해 다음이 정의되지 않았거나 구현이 결정론 요구를 만족하지 못한다(근거: §10 갭 요약표).

1. **Event의 정의 부재**: TIMPANI 관점에서 무엇을 "Event 발생"과 "Event 처리 작업(Job)"의 시작·끝으로 볼지 기준이 모호하다. 현 BPF는 **모든** wakeup에서 `activation_ns`를 덮어쓰므로(`timpani.bpf.c:256`), job 도중의 뮤텍스/I/O 블로킹 후 재깨움이 새 활성화로 오인되어 마감 판정과 도달 카운트가 오염된다.
2. **Event 감지·활성화 시점의 불명확성**: 활성화 시각(`activation_ns`)과 마감(`deadline_us`)의 기준점이 "커널 wakeup"인지 "미들웨어가 관측한 도달"인지 정해지지 않았다. 또한 L2 마감 판정이 job 완료 시점이 아니라 **매 stopping마다** 수행된다.
3. **Event 감지와 CBS 런타임의 연결**: 활성화된 태스크가 L1(TT)과 간섭 없이 CBS 예산 내에서 실행되고 완료 후 재대기하는 종단간 흐름은 존재하나, (a) 보충이 BPF ops 경로 진입에만 의존해 **보충 지연 상한이 없고**, (b) `DSQ_CBS`가 전역 큐라서 L2 태스크가 **배정 CPU 밖에서 실행**될 수 있어 CPU별 Gap 분석(DDR-007)이 무효화되며, (c) 승인 검사가 이용률만 보고 **TT 블로킹·D<MIT·동일 CPU 서버 간섭**을 반영하지 않는다.
4. **정량 바운드 부재**: 깨움 지연, 보충 지연, 예산 초과 허용치, 감지·보고 지연에 대한 수치 요구사항이 없어 검증 프레임워크가 합부를 판정할 수 없다.
5. **인터페이스 불일치**: Pullpiri→O proto에 MIT 전용 필드가 없고 `period`를 재사용한다. O→N에는 `ExecutionModel.EVENT_DRIVEN`이 선언만 되어 있고 항상 `TIME_TRIGGERED`로 발행된다. BUDGET_EXCEED/WATCHDOG 폴트는 O→Pullpiri 경계에서 UNKNOWN으로 소실된다.
6. **전송 계층 특정 PoC**: `examples/someip/libtimpani_rt`는 SOME/IP의 service/instance/event ID를 API 계약에 포함하고(`timpani_evt_register(wl, task, svc, inst, evt, mit)`), 자체 SHM(`/timpani_rt`)에 MIT·E2E 폴트를 기록하지만 TIMPANI-N은 이를 소비하지 않는다. 서버 키 해시도 BPF와 불일치한다. 이는 원칙 1에 위배되므로 재설계 대상이다(§12).

---

## 2. 개념 모델: Event 정의 및 실행 흐름 (Conceptual Model)

### 2.1 TIMPANI에서의 Event 및 Job 정의
- **Event**: 대기(Blocked/Sleeping) 상태의 L2 워크로드 태스크를 실행 가능(Runnable) 상태로 전환시키는 외부 또는 내부 자극(예: 소켓/IPC 패킷 수신, 이벤트 디스크립터 신호, 조건 변수 깨움, 선행 태스크 완료 통지)이다. **TIMPANI는 Event의 출처·내용·전송 계층을 알지 못하며 알 필요가 없다.**
- **Event Job (작업 인스턴스)**: 단일 Event 도달로 태스크가 깨어난 시점(Activation)부터 해당 이벤트 처리를 마치고 **다음 이벤트를 기다리기 위한 대기 지점(wait point)**에 다시 진입하기까지의 1회 실행 단위이다.
- **Wait point**: 태스크가 "다음 이벤트를 기다린다"고 선언되는 블로킹 지점. 애플리케이션이 마커 API로 명시한 지점이 우선하며(P1), 마커가 없으면 §4.2 DET-005의 기본 규칙이 적용된다.
- **Mid-job blocking**: job 처리 도중 발생하는 뮤텍스/I/O 대기 후 재깨움. **새 Event Job이 아니다.**

### 2.2 Event 소스 분류와 TIMPANI의 관여 범위

| 소스 유형 | 예시 | TIMPANI 관여 | Phase |
|:--|:--|:--|:-:|
| 커널 wakeup 일반 | socket/epoll/futex/condvar/eventfd/timerfd에서의 깨움 | 기본 감지 수단. 출처 불문 | P1 |
| 애플리케이션 마커 | 앱(또는 앱 측 어댑터)이 "이벤트가 도달했다/다음 이벤트를 기다린다"를 TIMPANI 라이브러리로 표시 | 결정적 wait point와 도달 시각 주입. **미들웨어 식별자는 포함하지 않음** | P1 |
| 미들웨어 디스패치 경계 | SOME/IP(vsomeip) 핸들러, DDS 리스너 | TIMPANI는 관여하지 않음. 앱 어댑터가 마커 API를 호출 | P1(앱 측) / P2(관측 정밀화) |
| 선행 태스크 완료 | 파이프라인/DAG 후속 활성화 | — | P2+ (REQ-DD-001) |
| 하드웨어 인터럽트 | 드라이버 IRQ → 사용자 스레드 깨움 | 커널 wakeup으로 포착 | P1 |

### 2.3 Event-Driven Job 생명주기 (Lifecycle)

```text
  [Event 대기 (Waiting at wait point)]
        │
        │ ① Event 발생 → 커널 Wakeup   (t_arr: 도달, t_act: 활성화 감지)
        ▼
  [Job 활성화 (Activated / Runnable)]  ──→ activation_ns = t_act 기록(한 job당 1회), MIT 검사, CBS 잔여 예산 확인
        │
        │ ② TT Gap 구간에서 배정 CPU 할당 (CBS Dispatch)   (t_start: 최초 실행 개시)
        ▼
  [CBS 기반 실행 (Running)]            ──→ 실행 시간(exec_ns) 측정 및 CBS 예산 차감
        │   ├─ L1 TT 슬롯 도래 시 선점/양보 → 재개(Running 재진입; 새 activation 아님)
        │   └─ mid-job blocking → 재깨움(Running 재진입; 새 activation 아님)
        │
        │ ③ 이벤트 처리 완료 후 wait point 진입 (Stopping & Not Runnable)   (t_done: 완료)
        ▼
  [Job 완료 (Completed)]               ──→ 응답시간 R = t_done − t_act, 마감 판정(R ≤ D)
        │
        └─→ 다음 Event 대기 상태로 복귀
```

### 2.4 핵심 타이밍 파라미터

**선언 파라미터** (Pullpiri → TIMPANI-O)

| 파라미터 | 기호 | 의미 | 제약 |
|:--|:--|:--|:--|
| `min_inter_arrival_us` | MIT | 연속된 두 Event 도달 사이의 최소 간격. CBS 보충 주기 $T_s$의 기준 | > 0 |
| `wcet_us` | C | 단일 Event Job 최악 실행 시간. CBS 최대 예산 $C_s$의 기준 | 0 < C ≤ D |
| `deadline_us` | D | 활성화(`t_act`)로부터 Job 완료까지의 상대 마감 | C ≤ D ≤ MIT |
| `max_dmiss` | — | 보고 전 허용되는 누적 마감 실패 횟수(DDR-012 `current_limit`) | ≥ 0 |

**파생 파라미터** (TIMPANI-O 산출)

| 파라미터 | 산출 | 비고 |
|:--|:--|:--|
| $C_s$ | = C (WCET) | 서버 예산 |
| $T_s$ | = MIT | 보충 주기 |
| $U_s$ | = $C_s / T_s$ | 서버 이용률 |
| $B_{tt}(k, L)$ | CPU k에서 길이 L인 임의 윈도우 내 TT 점유 시간의 최댓값(P1 승인식은 L = L2 태스크의 D). L이 최장 연속 TT 블록 이상이면 최대 연속 TT 점유 길이와 같다 | 승인 검사의 블로킹 항(§4.3 CBS-004) |

**측정 지점** (검증 프레임워크 공통 어휘)

| 기호 | 정의 | 관측 주체 |
|:--|:--|:--|
| $t_{arr}$ | 이벤트가 노드에 도달한 시각 | 앱/어댑터가 마커로 주입(P1 선택) — 미주입 시 $t_{act}$로 근사 |
| $t_{act}$ | 태스크 wakeup이 TIMPANI-N에 관측된 시각 | BPF |
| $t_{start}$ | job의 최초 실행 개시 시각 | BPF |
| $t_{done}$ | wait point 진입 시각 | BPF |
| 깨움 지연 | $t_{start} − t_{act}$ | |
| 응답 시간 | $R = t_{done} − t_{act}$ | |

---

## 3. 핵심 유스케이스 (Core Use Cases)

### UC-01: 정상 Event 도달 및 CBS 기반 실행 (P1)
1. L2 태스크가 I/O 또는 동기화 객체(epoll, socket, futex 등) 또는 TIMPANI 마커 wait API에서 이벤트를 기다리며 대기(Blocked) 상태로 머문다.
2. 외부 이벤트가 도달하여 커널이 해당 태스크를 깨우면(Wakeup), TIMPANI-N이 이를 **Event 도착(Job Activation)**으로 감지하고 활성화 타임스탬프(`activation_ns`)를 기록한다.
3. TIMPANI-N BPF 스케줄러는 해당 태스크의 CBS 잔여 예산(`remaining_us > 0`)을 확인한 후 배정 CPU의 L2 큐에 삽입한다.
4. 배정된 Isolated CPU에서 L1 TT 슬롯이 실행 중이 아닐 때(Gap 구간), 태스크가 디스패치되어 실행되며 사용한 CPU 시간만큼 CBS 예산이 차감된다.
5. 태스크가 이벤트 처리를 완료하고 다음 이벤트를 기다리기 위해 wait point로 진입하면, TIMPANI-N은 이를 **Job 완료**로 감지하고 마감(`activation_ns + deadline_us`) 준수 여부를 확인한다.

### UC-02: 예산 소진 시 기본 Throttling 및 주기적 보충 (P1)
1. 예상보다 빈번한 이벤트 발생 또는 긴 실행 시간으로 태스크의 CBS 잔여 예산(`remaining_us`)이 `0`에 도달한다.
2. TIMPANI-N은 해당 태스크를 즉시 Throttled 큐로 이동시켜 실행을 보류함으로써, 같은 CPU의 L1 TT 태스크 및 다른 L2 CBS 태스크를 보호한다.
3. 보충 주기($T_s = \text{MIT}$)가 경과하면 예산이 $C_s$로 회복(Replenishment)되고, 태스크는 다시 L2 큐로 승격되어 실행을 재개한다. 보충은 **다른 태스크의 활동 여부와 무관하게** 바운드된 지연 내에 이루어진다(§4.4 PRT-003).

### UC-03: TT + CBS 공존 하 L1 보호 (P1)
1. 동일 Isolated CPU에 L1(TT) 태스크 A(period 20 ms, WCET 2 ms)와 L2(CBS) 태스크 X(MIT 18 ms, WCET 2 ms, D 5 ms)가 배치된다(FCW 구성).
2. X의 이벤트가 A의 TT 슬롯 시작 0.5 ms 전에 도달한다. X는 Gap에서 실행을 시작하되 슬라이스가 "다음 TT 시작까지"로 제한되어 A 시작 전에 CPU를 양보한다.
3. A가 슬롯을 마치고 wait point로 진입하면 X가 재개되어 잔여 예산 내에서 완료한다. A의 활성화 지터는 X의 존재 여부와 무관하게 REQ-NFR-001 바운드 내에 있다.

### UC-04: 런타임 L2 워크로드 추가/제거 (P1 기본 / P2 무교란)
1. 시스템이 L1/L2 워크로드를 실행 중인 상태에서 Pullpiri가 새 L2 워크로드 Y를 `AddSchedInfo`로 등록한다.
2. TIMPANI-O는 Y의 승인 검사를 수행하고 성공 시 테이블을 발행한다(P1: 전체 테이블 재발행 허용; P2: 영향 노드만, epoch 유지).
3. TIMPANI-N은 Y를 추가한다(P1: 기존 태스크의 정상 재등록 허용; P2: 기존 L1/L2 활성화 무누락 보장). 제거(`RecoveryService` STOP) 시에도 동일하다.

### UC-05: 동일 CPU 다중 L2 서버 (P1)
1. 한 Isolated CPU에 L2 태스크 X(D 5 ms)와 Z(D 20 ms)가 배치되고, 두 이벤트가 거의 동시에 도달한다.
2. TIMPANI-N은 정의된 순서 정책(P1: 도착 순서 FIFO)에 따라 디스패치하며, 승인 검사는 그 정책 하에서 두 태스크 모두의 마감 충족을 사전에 확인한다.

### UC-06: MIT 위반 버스트 (P1)
1. 센서 오동작으로 X의 이벤트가 MIT(18 ms)보다 짧은 간격(약 MIT/3 = 6 ms)으로 반복 도달한다(FCW `--burst-ms 6`).
2. TIMPANI-N은 각 도달에 대해 MIT 위반을 기록하고, X는 CBS 예산 범위 내에서만 실행되어 L1 및 타 L2에 영향을 주지 않는다.
3. 예산 소진에 따른 스로틀링으로 발생하는 마감 실패는 **MIT 위반 연관 표식**과 함께 보고되어, 정상 격리 동작이 결함으로 오분류되지 않는다.

### UC-07: 이벤트 침묵 감시 (P2, 참고)
기대 최대 도달 간격 내에 이벤트가 도달하지 않으면 `WATCHDOG` 폴트를 발생시킨다.

---

## 4. 시스템 요구사항 (System Requirements)

> 표기: **Phase** 열 `P1` = Phase 1(기본 기능), `P2` = Phase 2. `P1*` = Phase 1에서는 단순화된 형태로 충족하고 Phase 2에서 강화. v1 ID는 변경하지 않았다.

### 4.1 Event 정의 및 워크로드 모델 (MOD)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-MOD-001** | 시스템은 외부 이벤트에 의해 비주기적으로 활성화되며 최소 도달 간격(MIT)을 가지는 Safety-Critical 워크로드를 **L2 (Sporadic + SafetyCritical)**로 분류하고, 태스크별로 `min_inter_arrival_us`, `wcet_us`, `deadline_us`를 명세할 수 있어야 한다(SHALL). | High | P1 |
| **REQ-008-MOD-002** | 시스템은 Event-Driven 태스크의 실행 단위를 **대기(Wait) → 활성화(Activation) → 실행(Execution) → 완료(Completion)**의 반복 주기(Event Job)로 모델링해야 한다(SHALL). | High | P1 |
| **REQ-008-MOD-003** | 시스템은 Event Job 도중의 내부 블로킹(뮤텍스, I/O, 선점) 후 재깨움을 **새 활성화로 간주하지 않아야** 하며, 한 Event Job당 활성화 시각은 정확히 1회 기록되어야 한다(SHALL). P1에서는 마커 API를 사용하는 태스크에 대해 보장하고, 마커 없는 태스크는 §4.2 DET-005의 P1 규칙과 알려진 한계를 따른다. | High | P1* |
| **REQ-008-MOD-004** | 시스템은 L2 태스크의 타이밍 파라미터에 대해 `0 < wcet_us ≤ deadline_us ≤ min_inter_arrival_us` 를 승인 시 검증하고, 위반 시 워크로드를 거부해야 한다(SHALL). `deadline_us > min_inter_arrival_us`(arbitrary deadline)는 범위 외이다. | High | P1 |
| **REQ-008-MOD-005** | 시스템은 MIT가 정의되지 않는 Aperiodic 워크로드를 L2로 수용하지 않아야 한다(SHALL). (DDR-001 §3 준수) | Medium | P1 |
| **REQ-008-MOD-006** | TIMPANI의 워크로드 모델·프로토콜·라이브러리 계약은 특정 미들웨어/전송 계층의 식별자(서비스·인스턴스·토픽·이벤트 ID 등)를 포함하지 않아야 한다(SHALL). Event의 식별 단위는 **L2 태스크**이다. | High | P1 |

### 4.2 Event 감지 및 활성화 (DET)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-DET-001** | TIMPANI-N은 대기(Blocked/Sleeping) 상태에 있던 L2 태스크가 이벤트 도달로 깨어나는(Wakeup / Runnable 전환) 시점을 **Event 활성화(Job Activation)**로 감지하고, 해당 시점의 단조 시계 타임스탬프(`activation_ns`)를 기록해야 한다(SHALL). | High | P1 |
| **REQ-008-DET-002** | TIMPANI-N은 실행 중이던 L2 태스크가 이벤트 처리를 마치고 wait point에서 자발적으로 대기 상태(`!runnable`)로 전환되는 시점을 **Event Job 완료(Completion)**로 감지해야 한다(SHALL). 선점(preemption)·슬라이스 만료에 의한 정지는 완료가 아니다. | High | P1 |
| **REQ-008-DET-003** | Event 감지 및 스케줄링 제어는 **애플리케이션 투명성(Transparency)**을 보장해야 한다(SHALL). L2 애플리케이션은 표준 리눅스 이벤트 대기 메커니즘(socket, epoll, poll, futex, condvar 등)을 그대로 사용할 수 있어야 하며, TIMPANI 전용 주기 동기화 API(`ttsched_wait_next_period()`)나 직접적인 RT 스케줄링 시스템 콜 호출을 강제받지 않아야 한다. | High | P1 |
| **REQ-008-DET-004** | TIMPANI는 애플리케이션이 **선택적으로** job 경계를 명시할 수 있는 경량·전송 계층 중립 마커 인터페이스를 제공해야 한다(SHALL): (a) 다음 이벤트를 기다리는 wait point 진입, (b) 지정 L2 태스크를 깨우는 signal(선택적으로 도달 시각 $t_{arr}$ 동반). 마커가 제공되면 커널 wakeup 추정보다 우선한다. 이는 Phase 2 미들웨어 경계 관측(REQ-FUN-009)의 주입점이 된다. | High | P1 |
| **REQ-008-DET-005** | 마커가 없는 태스크에 대해 TIMPANI-N은 mid-job blocking과 새 활성화를 구분하는 **결정적 규칙**을 적용해야 한다(SHALL). **P1 규칙**: 모든 자발적 블로킹을 완료로, 그 후 첫 wakeup을 활성화로 간주한다(알려진 한계: mid-job blocking이 있는 앱은 job이 분할 측정됨 → 마커 사용 권고). **P2 규칙**: 블로킹 시스템 콜 분류와 블로킹 지속 시간 임계를 이용한 정밀 분류(DDR-008). | High | P1* |
| **REQ-008-DET-006** | TIMPANI-N은 테이블 적용 시점에 존재하지 않던 L2 태스크 스레드(늦게 생성되는 워커, 컨테이너 재시작으로 PID가 바뀐 스레드)를 **스레드 생성 후 `T_bind` 이내**에 식별하여 CBS 정책을 적용해야 한다(SHALL). `T_bind` 목표값은 §4.6 TIM-006. P1은 주기적 재스캔, P2는 cgroup/Pullpiri 메타데이터 기반(REQ-NFR-004). | High | P1* |

### 4.3 CBS 기반 스케줄링 동작 (CBS)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-CBS-001** | TIMPANI-O는 L2 태스크의 타이밍 명세로부터 CBS 서버 파라미터(예산 $C_s = \text{wcet\_us}$, 보충 주기 $T_s = \text{min\_inter\_arrival\_us}$)를 산출하고, 대상 Isolated CPU의 L1 TT 슬롯 배치 후 잔여 용량(Gap) 내에서 스케줄 가능성($U_{\text{tt}} + U_{\text{cbs}} + U_{\text{overhead}} \le U_{\text{bound}}$)을 검증해야 한다(SHALL). | High | P1 |
| **REQ-008-CBS-002** | TIMPANI-N은 활성화된 L2 태스크를 배정된 Isolated CPU의 **TT Gap 구간**(L1 TT 슬롯이 비활성인 시간 구간)에서만 디스패치해야 하며, 실행 도중 L1 TT 슬롯 시작 시각이 도래하면 즉시 CPU를 양보(Slice Capping 및 Preemption)하여 L1의 시간적 결정론을 보장해야 한다(SHALL). | High | P1 |
| **REQ-008-CBS-003** | TIMPANI-N은 L2 태스크가 CPU에서 실행될 때마다 실제 실행 시간을 추적하여 CBS 서버의 잔여 예산(`remaining_us`)을 차감해야 한다(SHALL). | High | P1 |
| **REQ-008-CBS-004** | TIMPANI-O의 승인 검사는 이용률 조건에 더해, **D ≤ MIT인 L2 태스크에 대해 마감 충족의 충분 조건**을 검증해야 한다(SHALL). **P1 조건**(보수적 단순식): $C_{s,i} + B_{tt}(k, D_i) + L_{wake}^{max} + \sum_{j \ne i} C_{s,j} \le D_i$ (동일 CPU 타 서버 각 1 job, FIFO 가정; $B_{tt}(k, D_i)$ = 길이 $D_i$ 윈도우 내 최대 TT 점유). **P2 조건**: 윈도우 수요·공급 함수 기반 정밀식(EDF 순서). | High | P1* |
| **REQ-008-CBS-005** | L2 태스크는 TIMPANI-O가 배정한 CPU에서만 실행되어야 한다(SHALL, Partitioned Scheduling — DDR-004 §1). 타 Isolated CPU로의 마이그레이션은 허용되지 않는다. | High | P1 |
| **REQ-008-CBS-006** | CBS 예산 차감은 1 µs 해상도로 이루어져야 하며, 서버당 한 보충 주기 내 누적 **초과 실행(overrun)**은 §4.6 TIM-004의 바운드를 넘지 않아야 한다(SHALL). | High | P1* |

### 4.4 보호 및 폴트 처리 (PRT)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-PRT-001** | L2 태스크의 CBS 잔여 예산이 `0`이 되면, TIMPANI-N은 해당 태스크의 실행을 즉시 제한(Throttling)하고, 보충 시각(`replenish_at_ns`, 주기 $T_s$)이 도래했을 때 예산을 $C_s$로 보충하여 실행 가능 상태로 전환해야 한다(SHALL). | High | P1 |
| **REQ-008-PRT-002** | TIMPANI-N은 L2 태스크의 Event 활성화 시각(`activation_ns`)으로부터 **Job 완료 시점**까지의 경과 시간이 `deadline_us`를 초과할 경우 이를 **Deadline Miss(`FAULT_DMISS`)**로 감지하고, 설정된 허용 임계치(`max_dmiss`) 초과 시 상위(TIMPANI-O)로 보고해야 한다(SHALL). 실행 중(runnable)인 태스크에 대해서는 마감 경과 시 **1회만** 조기 감지할 수 있으며(MAY), 동일 job에 대한 중복 보고는 금지된다. | High | P1 |
| **REQ-008-PRT-003** | 예산 보충은 **스케줄러 활동(타 태스크의 enqueue/dispatch)에 의존하지 않고** 보충 시각으로부터 §4.6 TIM-003의 지연 바운드 내에 완료되어야 한다(SHALL). (DDR-007 §4.5 Lazy + Backup 중 Backup 경로의 필수화) | High | P1 |
| **REQ-008-PRT-004** | TIMPANI-N은 L2 태스크의 연속 활성화 간격이 `min_inter_arrival_us`보다 짧은 경우를 **MIT 위반**으로 감지·계수해야 한다(SHALL). MIT 위반 시에도 CBS 격리는 유지되어야 하며(FFI), **P1 정책**은 "잔여 예산 내 흡수(위반 도달은 새 예산을 받지 않음)"이다. 대안 정책(지연/폐기)은 P2에서 검토한다. | High | P1 |
| **REQ-008-PRT-005** | 시스템은 다음 이상 징후를 **서로 구분되는 폴트 유형**으로 분류·보고해야 한다(SHALL): `DMISS`(마감 실패), `BUDGET_EXCEED`(예산 소진 — 정상 격리 동작, 정보성), `MIT_VIOLATION`(모델 위반) [P1]; `WAKEUP_LATENCY_EXCEED`(깨움 지연 초과), `WATCHDOG`(이벤트 침묵) [P2]. 각 유형은 TIMPANI-N → TIMPANI-O → Pullpiri 경로에서 **유형 손실 없이** 전달되어야 한다. (SRS-000 REQ-FUN-014) | High | P1* |
| **REQ-008-PRT-006** | 정보성 이벤트(`BUDGET_EXCEED`)는 서버당 Throttle 에피소드마다 최대 1회로 **집계(rate-limit)**되어 보고되어야 하며, 폴트 보고 경로의 대역폭을 소진하지 않아야 한다(SHALL). | Medium | P1 |
| **REQ-008-PRT-007** | MIT 위반에 기인한 스로틀링으로 발생한 `DMISS`는 보고 시 **MIT 위반 연관 표식**을 포함해야 하며(SHOULD), 상위 계층이 "모델 위반(입력 측 문제)"과 "실행 결함(워크로드 측 문제)"을 구분할 수 있어야 한다. | Medium | P1 |

### 4.5 배치 및 순서 (ORD)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-ORD-001** | TIMPANI-O는 L2 태스크를 Isolated CPU에 배치할 때 CPU별 Gap 구조(최소 Gap 길이 ≥ `CBS_MIN_EXEC_US`, 최대 TT 연속 점유 $B_{tt}$)를 고려해야 하며, 배치 결과가 CBS-004의 충분 조건을 만족하지 못하면 다른 CPU를 시도하거나 거부해야 한다(SHALL). | High | P1 |
| **REQ-008-ORD-002** | 동일 CPU에 둘 이상의 L2 서버가 존재할 때 TIMPANI-N은 **결정적 디스패치 순서 정책**을 적용해야 한다(SHALL). **P1**: 활성화 순서(FIFO). **P2**: 절대 마감 기반(EDF). 승인 검사(CBS-004)는 동일 정책을 전제로 해야 한다. | High | P1* |
| **REQ-008-ORD-003** | L2 서버 간에는 예산 격리가 성립해야 한다(SHALL). 한 L2 태스크의 예산 초과·MIT 위반이 동일 CPU의 다른 L2 태스크의 응답 시간을 승인 시 가정한 간섭 이상으로 증가시키지 않아야 한다. | High | P1 |

### 4.6 정량 타이밍 요구사항 (TIM)

> 각 항목은 측정 방법을 함께 명시한다. 목표값 중 `(후보)` 표시는 대상 HW(aarch64 — 예: Raspberry Pi 5, PREEMPT_RT 6.12, sched_ext)에서의 측정으로 확정해야 하며, 확정 전까지는 SRS-008/테스트 전략의 합부 기준으로 임시 사용한다. 관측 조건은 "Isolated CPU, 정격 부하(승인된 L1/L2 전부 활성), 1시간 연속 관측"을 기본으로 한다. 모두 P1에서 측정을 시작한다.

| ID | 요구사항 명세 | 목표값 | 측정 방법 |
|:--|:--|:--|:--|
| **REQ-008-TIM-001** | **깨움 지연 (Gap 상태)**: 배정 CPU가 Gap이고 유휴일 때, $t_{start} − t_{act}$ 는 바운드되어야 한다(SHALL). | ≤ 50 µs @ p99.99 (후보) | BPF 트레이스($t_{act}$ at wakeup, $t_{start}$ at running) → 검증 프레임워크 히스토그램 |
| **REQ-008-TIM-002** | **깨움 지연 (TT 점유 상태)**: TT 슬롯이 활성인 동안 도달한 이벤트의 $t_{start} − t_{act}$ 는 $B_{tt} + L_{wake}$ 를 넘지 않아야 한다(SHALL). | ≤ $B_{tt}$ + 50 µs (후보) | 동일 트레이스. $B_{tt}$는 테이블에서 계산 |
| **REQ-008-TIM-003** | **보충 지연**: 예산 소진 후 `replenish_at_ns` 도래 시점부터 태스크가 실행 가능(L2 큐 승격 + CPU 재스케줄)해지기까지의 지연은 바운드되어야 한다(SHALL). 이벤트/디스패치 활동이 전혀 없는 조건에서도 성립해야 한다. | ≤ 100 µs @ p99.99 (후보) | Backup Replenisher 발화 시각과 BPF `remaining_us` 복원 시각의 차 |
| **REQ-008-TIM-004** | **예산 초과 허용치**: 서버당 보충 주기 내 실제 실행 시간이 $C_s$를 초과하는 양(overrun)은 바운드되어야 한다(SHALL). | P1: ≤ 1 scheduler tick(HZ=1000 기준 1 ms); P2: ≤ 50 µs(HRTICK 채택 시, 후보) | BPF stopping에서 exec 누적 vs budget; 커널 설정(CONFIG_HZ, HRTICK)별 측정 |
| **REQ-008-TIM-005** | **마감 실패 감지·보고 지연**: 마감 경과 후 `FAULT_DMISS`가 TIMPANI-O에 도달하기까지의 지연은 바운드되어야 한다(SHALL). | 커널 감지 ≤ 마감 + $C_s$; O 도달 ≤ 감지 + 200 ms (후보) | ringbuf 타임스탬프 vs O 수신 시각 |
| **REQ-008-TIM-006** | **지연 식별(`T_bind`)**: DET-006의 늦은 스레드 식별 지연. | ≤ 1 s (후보) | 스레드 생성 시각 vs `task_meta_map` 등록 시각 |
| **REQ-008-TIM-007** | **L1 무간섭**: L2 워크로드의 존재·폭주(P1) 및 추가·제거(P2)가 L1 TT 활성화 지터에 미치는 영향은 REQ-NFR-001 바운드 내에서 **통계적으로 무차이**여야 한다(SHALL). | L2 유/무 비교 시 p99.99 지터 차 ≤ 10 µs (후보) | Timer Master 지터 히스토그램 A/B 비교 |
| **REQ-008-TIM-008** | **관측 오버헤드**: Event 감지(활성화 기록, MIT 검사, 마커 처리)가 이벤트당 추가하는 CPU 시간은 바운드되어야 한다(SHALL). | ≤ 2 µs/이벤트 (후보) | BPF ops 실행 시간 측정(`bpf_prog` 통계) + 마커 API 마이크로벤치 |

### 4.7 인터페이스 요구사항 (IF)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-IF-001** | Pullpiri → TIMPANI-O 인터페이스는 L2 태스크의 MIT를 **주기(`period`)와 구분되는 전용 필드**로 전달할 수 있어야 한다(SHALL). 기존 `period` 재사용 입력에 대한 하위 호환은 명시된 전환 기간 동안 유지한다(SHOULD). (DDR-001 §5, DDR-003 §3) | High | P1 |
| **REQ-008-IF-002** | Pullpiri → TIMPANI-O 인터페이스는 **태스크 단위**로 시간 클래스(Periodic/Sporadic)를 지정할 수 있어야 한다(SHALL). 한 워크로드(컨테이너)가 TT 스레드와 Event-Driven 스레드를 함께 가질 수 있어야 한다. P1에서는 워크로드 단위 클래스(현행)로 충분하며 샘플 앱은 TT/L2를 별도 워크로드로 등록한다. | Medium | P2 |
| **REQ-008-IF-003** | TIMPANI-O → TIMPANI-N 인터페이스는 L2 엔트리를 `ExecutionModel.EVENT_DRIVEN` 레이어로 발행해야 하며(SHALL), 엔트리는 최소한 $C_s$, $T_s$, MIT, D, `current_limit`, 배정 CPU를 포함해야 한다. | High | P1 |
| **REQ-008-IF-004** | TIMPANI-O → Pullpiri 폴트 인터페이스는 PRT-005의 모든 폴트 유형을 표현할 수 있어야 한다(SHALL). | High | P1 |
| **REQ-008-IF-005** | 애플리케이션 마커 인터페이스(DET-004)는 C ABI로 제공되어야 하며, 호출당 비용이 TIM-008 내여야 하고, 힙 할당·블로킹 I/O 없이(단일 atomic 연산 + 최대 1회 futex 시스템 콜) 동작해야 한다(SHALL). 인터페이스는 **미들웨어 식별자를 인자로 받지 않는다**(SHALL, MOD-006). | High | P1 |
| **REQ-008-IF-006** | 승인 거부 시 TIMPANI-O는 거부 사유(이용률 초과 / 마감 불충족 / 파라미터 위반 / CPU 없음)를 Pullpiri에 **구조화된 코드**로 반환해야 한다(SHOULD). P1은 로그 + `status`만으로 충분. | Medium | P2 |

### 4.8 런타임 갱신 요구사항 (RTU)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-RTU-001** | L2 워크로드는 데몬 재시작 없이 런타임에 추가·제거·수정될 수 있어야 한다(SHALL). (SRS-000 REQ-FUN-012, DDR-011) | High | P1 |
| **REQ-008-RTU-002** | L2 워크로드의 추가·제거는 기존 L1 TT 슬롯의 오프셋·epoch 정렬을 변경하지 않아야 하며, 기존 L1/L2 태스크의 활성화를 누락시키지 않아야 한다(SHALL). (TIM-007로 검증) | High | P2 |
| **REQ-008-RTU-003** | L2 엔트리는 hyperperiod 위상(phase)을 갖지 않으므로, L2만의 추가·제거는 hyperperiod 경계 대기 없이 **즉시** 적용될 수 있어야 한다(SHOULD). (DDR-006 §5 전환 정책) | Medium | P2 |
| **REQ-008-RTU-004** | L2 워크로드 제거(STOP) 시 TIMPANI-N은 해당 태스크의 CBS 상태·메타데이터를 제거하고 스케줄링 클래스를 원복해야 하며, 잔존 참조가 없어야 한다(SHALL). | High | P1 |

### 4.9 관측성 요구사항 (OBS)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-OBS-001** | TIMPANI-N은 L2 태스크별로 다음 통계를 수집·노출해야 한다(SHALL): 도달 수, 완료 수, MIT 위반 수, DMISS 수, BUDGET_EXCEED 에피소드 수, 응답 시간 히스토그램, 깨움 지연 히스토그램, 보충 주기당 예산 사용량. | High | P1 |
| **REQ-008-OBS-002** | 검증 프레임워크가 $t_{act}$/$t_{start}$/$t_{done}$ 타임스탬프를 job 단위로 추출할 수 있는 **트레이스 포인트**를 제공해야 한다(SHALL). 트레이스 활성화가 TIM-008 오버헤드 바운드를 초과하지 않아야 한다. | High | P1 |
| **REQ-008-OBS-003** | OBS-001 통계는 TIMPANI-O로 주기 보고(`NodeStatus`)되어 노드 간 비교가 가능해야 한다(SHOULD). | Medium | P2 |

### 4.10 비기능 요구사항 (NFR)

| ID | 요구사항 명세 | 우선순위 | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-NFR-001** | 노드당 최소 32개, CPU당 최소 8개의 L2 태스크를 지원해야 한다(SHALL). | Medium | P1 |
| **REQ-008-NFR-002** | Event 감지·보충·폴트 감지의 RT 임계 경로에는 gRPC·힙 할당·블로킹 I/O가 없어야 한다(SHALL). (DDR-006 §4) | High | P1 |
| **REQ-008-NFR-003** | L2 지원 추가로 인해 L1 전용 구성의 동작·성능이 변하지 않아야 한다(SHALL, 회귀 금지). | High | P1 |
| **REQ-008-NFR-004** | 설계는 QNX 등 타 RTOS로의 이식을 고려하여 Event 감지 지점을 OS 추상화 경계 뒤에 두어야 한다(SHOULD). (SRS-000 REQ-ENV-004) | Low | P2 |
| **REQ-008-NFR-005** | 프로토콜 변경은 기존 필드 번호를 유지하고 새 필드만 추가하여 **와이어 호환**을 유지해야 한다(SHALL). | High | P1 |

---

## 5. 수용 기준 (Acceptance Criteria)

| ID | 기준 | 합격 조건 | 검증 수단 | 관련 요구사항 | Phase |
|:--|:--|:--|:--|:--|:-:|
| **AC-008-01** | Event 감지 및 비침투성 | (a) 마커 API 사용 L2 워크로드: 100,000회 이벤트 중 활성화 기록 누락·중복 0건. (b) TIMPANI 전용 API 없이 표준 블로킹 호출(socket/epoll/timerfd)로 대기하는 L2 워크로드가 `SCHED_EXT` CBS 태스크로 등록되고 이벤트마다 활성화·디스패치됨 | FCW 소비자 + `cbstask` + BPF 트레이스 | DET-001~004, MOD-003 | P1 |
| **AC-008-02** | TT + CBS 공존 및 L1 보호 | 동일 Isolated CPU의 L1/L2 공존 1시간 관측에서 L1 TT 활성화 지터 p99.99가 L2 없을 때 대비 +10 µs 이내, L1 DMISS 0건(기동 직후 안정화 구간 제외) | Timer Master 지터 히스토그램 A/B | CBS-002, TIM-007 | P1 |
| **AC-008-03** | 예산 차감 및 Throttling | 예산 소진 즉시 Throttled 전환, 보충 주기 내 overrun ≤ TIM-004(P1), 보충 지연 ≤ TIM-003 (이벤트 무도달 조건 포함) | `cbstask` 오버런 모드 / FCW `-be` + BPF 맵 샘플링 | CBS-003/006, PRT-001/003 | P1 |
| **AC-008-04** | Deadline Miss 감지 | 단일 job 실행 시간이 D 초과 시 `FAULT_DMISS` 1회 감지, `max_dmiss` 초과 시 TIMPANI-O 수신까지 ≤ TIM-005; 동일 job 중복 보고 0건 | FCW `--overrun-ms 10` | PRT-002, TIM-005 | P1 |
| **AC-008-05** | 깨움 지연 바운드 | Gap 조건 $t_{start}−t_{act}$ p99.99 ≤ TIM-001, TT 점유 조건 ≤ TIM-002 | 이벤트 생성기 + BPF 트레이스 | TIM-001/002 | P1 |
| **AC-008-06** | CPU 고정 | 1시간 관측에서 L2 태스크가 배정 CPU 외에서 실행된 횟수 0 | `sched_switch` 트레이스 | CBS-005 | P1 |
| **AC-008-07** | MIT 위반 격리 | 약 MIT/3 간격(FCW: MIT 18 ms → 6 ms) 버스트 주입 시 `MIT_VIOLATION` 계수 정확(±0), L1 DMISS 0, 타 L2 응답시간 승인 가정 내, 위반 연관 `DMISS`에 표식 존재 | FCW 생산자 `--burst-ms 6` | PRT-004/005/007, ORD-003 | P1 |
| **AC-008-08** | 승인 제어 | $C_{s,i} + B_{tt}(k, D_i) + L_{wake}^{max} + \sum_{j \ne i} C_{s,j} > D_i$ 인 L2 요청은 거부(CBS-004 P1식); 거부된 요청이 기존 테이블을 훼손하지 않음 | GTest(timpani-o) | CBS-004, ORD-001 | P1 |
| **AC-008-09** | 런타임 추가/제거 | 실행 중 L2 추가/제거 10회 반복 동안 데몬 재시작 없음, 제거 후 잔존 BPF 상태 0 (P1); 기존 L1/L2 활성화 누락 0 (P2) | 통합 테스트 | RTU-001/004 (P1), RTU-002/003 (P2) | P1 / P2 |
| **AC-008-10** | 늦은 스레드 식별 | 테이블 적용 후 생성된 워커 스레드가 `T_bind` 내 CBS 적용 | 통합 테스트 | DET-006, TIM-006 | P1 |
| **AC-008-11** | 폴트 유형 보존 및 속도 제한 | `BUDGET_EXCEED`/`MIT_VIOLATION`이 Pullpiri 수신 시 유형 그대로 도달; `BUDGET_EXCEED`는 에피소드당 ≤ 1건 | mock_pullpiri 수신 로그 | PRT-005/006, IF-004 | P1 |
| **AC-008-12** | 전송 계층 중립 | TIMPANI 코어(O/N/BPF/libttsched)에 미들웨어 식별자 참조 0건; FCW 샘플의 SOME/IP 처리가 앱 어댑터에만 존재 | 코드 리뷰 + grep | MOD-006, IF-005 | P1 |

---

## 6. 상위 요구사항 추적성 (Traceability to SRS-000)

| 본 문서 ID | SRS-000 요구사항 ID | 관련 DDR | 비고 |
|:--|:--|:--|:--|
| `REQ-008-MOD-001`~`006` | `REQ-FUN-001`, `REQ-FUN-002` | DDR-001, DDR-008 | Sporadic + SafetyCritical (L2) 모델, Aperiodic 제외, 전송 계층 중립 |
| `REQ-008-DET-001`~`006` | `REQ-NFR-003`, `REQ-NFR-004`, `REQ-FUN-009`(주입점) | DDR-002, DDR-005, DDR-008 | 커널 레벨 Event 감지, 투명성, 마커 인터페이스, 지연 식별 |
| `REQ-008-CBS-001`, `004` | `REQ-FUN-013` | DDR-004, DDR-007, DDR-008 | 오프라인 CBS 예산 할당 및 Feasibility 검사(응답시간 조건 포함) |
| `REQ-008-CBS-002`, `003`, `005`, `006` | `REQ-FUN-004`, `REQ-NFR-002` | DDR-002, DDR-005, DDR-007 | TT Gap 기반 CBS 런타임 집행, 파티션 고정, FFI |
| `REQ-008-PRT-001`~`007` | `REQ-FUN-008`, `REQ-FUN-014`, `REQ-FUN-015` | DDR-005, DDR-007, DDR-012, DDR-008 | Throttling/보충, 폴트 분류·보고, MIT 위반 |
| `REQ-008-ORD-001`~`003` | `REQ-FUN-013`, `REQ-NFR-002` | DDR-004, DDR-007, DDR-008 | 배치, 다중 서버 순서, 서버 간 격리 |
| `REQ-008-TIM-001`~`008` | `REQ-NFR-001`, `REQ-NFR-002` | DDR-008 | 정량 바운드. 테스트 전략의 합부 기준 |
| `REQ-008-IF-001`~`006` | `REQ-FUN-011` | DDR-003, DDR-006 | proto 확장(MIT 필드, EVENT_DRIVEN 레이어, 폴트 유형; P2: 태스크 단위 클래스, 사유 코드) |
| `REQ-008-RTU-001`~`004` | `REQ-FUN-012`, `REQ-FUN-015` | DDR-011 | L2 런타임 갱신 |
| `REQ-008-OBS-001`~`003` | `REQ-FUN-014` | DDR-006 | 통계·트레이스 |
| `REQ-008-NFR-001`~`005` | `REQ-NFR-005`, `REQ-ENV-004` | DDR-006 | 용량, RT 경로 순수성, 회귀 금지, 이식성, 와이어 호환 |
| (Phase 2) | `REQ-FUN-006`, `REQ-FUN-009`, `REQ-ED-001` | DDR-008(후속 개정) | 이벤트 침묵 감시, 미들웨어 경계 관측 |

---

## 7. 범위 외 (Out of Scope)

| 항목 | 사유 / 처리 |
|:--|:--|
| Aperiodic 워크로드(MIT 없음) | DDR-001 §3: CBS 적용 불가. Pullpiri가 L3/L4(CFS)로 직접 배포 |
| `deadline_us > min_inter_arrival_us` (arbitrary deadline) | job 큐잉·다중 미완료 job 추적 필요. Phase 2+ 후보 |
| 노드 간 이벤트 체인·E2E 마감(파이프라인/DAG) | SRS-000 `REQ-DD-001`. 본 문서는 단일 노드 내 활성화 → 완료만 다룸 |
| 미들웨어(SOME/IP, DDS 등) 고유 의미론 | 원칙 1. 앱/어댑터 책임. TIMPANI는 마커 API 주입점만 제공 |
| 이벤트 침묵(watchdog), TT floor 이중 모드 | DDR-002 §7 vs DDR-007 §0 불일치는 DDR-008에서 결정. P1은 watchdog 미구현 |
| L2 태스크의 NPU/GPU 자원 | `REQ-AI-001` |
| QNX 포팅 | `REQ-ENV-004`; NFR-004로 설계 고려만 |
| Pullpiri 측 YAML 스키마 변경 | TIMPANI는 proto 계약만 정의. Pullpiri 저장소 별도 작업 |

---

## 8. 가정 및 제약 (Assumptions & Constraints)

| ID | 내용 | 출처 |
|:--|:--|:--|
| A-01 | Linux 6.12+, `sched_ext`, PREEMPT_RT, cgroup v2 isolated partition | SRS-000 REQ-ENV-001~003 |
| A-02 | L2 태스크는 Isolated CPU에서만 실행되며 L3/L4는 Non-Isolated CPU에 있다 | DDR-002 §5 |
| A-03 | L2 태스크 스레드는 `task_id`와 동일한 comm 이름을 가진다(P1 식별 수단). cgroup/Pullpiri 메타데이터 기반 식별은 REQ-NFR-004에 따라 P2 | DDR-005 User App Interface |
| A-04 | 이벤트 도달 관측의 P1 기준점은 커널 wakeup($t_{act}$)이다. 마커로 $t_{arr}$가 주입되면 기록·보고하되 P1 바운드 판정은 $t_{act}$ 기준이다 | §2.4 |
| A-05 | `U_bound`, `U_overhead`, `CBS_MIN_EXEC_US`는 DDR-004/007의 후보값(0.80 / 0.02 / 100 µs)을 그대로 사용하며, 확정은 해당 DDR의 오픈 아이템에 따른다 | DDR-004 §5, DDR-007 §11 |
| A-06 | P1에서 마커 없는 L2 태스크는 mid-job blocking을 하지 않는다고 가정한다(DET-005 P1 규칙). 이 가정이 깨지는 앱은 마커 API를 사용한다 | §4.2 |
| C-01 | sched_ext 슬라이스 만료는 스케줄러 틱 또는 HRTICK에 의존한다. TIM-004 P1 값은 HZ=1000을 가정한다 | §4.6 |
| C-02 | BPF에서 futex wake를 직접 호출할 수 없으므로, 사용자 공간 보조 스레드(Replenisher)가 필요하다 | DDR-007 §4.5 |
| C-03 | proto 인터페이스 변경은 명시적 승인이 필요하며, 필드 번호 유지·추가만 허용한다 | `.github/copilot-instructions.md`, NFR-005 |
| C-04 | 현재 CI는 Rust만 빌드하므로 C++ 경로의 검증은 로컬 GTest·통합 테스트에 의존한다. 테스트 전략에서 보완 | §10 |

---

## 9. 향후 확장 로드맵 (Phase 2+)

P1 검증 후 다음 항목을 후속 리비전에서 구체화한다.

1. **마커 없는 태스크의 정밀 job 경계 분류**(DET-005 P2): 블로킹 시스템 콜 분류 + 블로킹 지속 시간 임계.
2. **EDF 서버 순서 및 윈도우 수요 기반 승인**(ORD-002, CBS-004 P2).
3. **태스크 단위 시간 클래스**(IF-002), **승인 거부 사유 코드**(IF-006).
4. **TT 무교란 런타임 갱신**(RTU-002/003): diff 기반 테이블 적용, L2 전용 변경 시 epoch 유지, 증분 `ScheduleTableUpdate`.
5. **미들웨어 디스패치 경계 관측**(`REQ-FUN-009`, `REQ-ED-001`): 마커로 주입된 $t_{arr}$ 기반 E2E/MIT 정밀 감시. TIMPANI 코어는 여전히 미들웨어 중립.
6. **이벤트 침묵(Watchdog) 감시 및 TT floor 결정**(`REQ-FUN-006`), **깨움 지연 초과 폴트**(`WAKEUP_LATENCY_EXCEED`).
7. **HRTICK 기반 예산 정밀도**(TIM-004 P2).
8. **cgroup/Pullpiri 메타데이터 기반 식별**(`REQ-NFR-004`), **NodeStatus 통계 보고**(OBS-003).
9. **Arbitrary deadline (D > MIT)**, **파이프라인/DAG 활성화**(`REQ-DD-001`).

---

## 10. 현행 구현 대비 갭 요약 (Gap Analysis vs `timpani26` HEAD `4201573`)

> 본 절은 요구사항 도출의 근거이며, 설계 결정은 DDR-008, 구현 변경은 HLD/LLD-008에서 다룬다.

| # | 영역 | 현행 구현 (근거) | 요구사항 | 갭 | Phase |
|:--|:--|:--|:--|:--|:-:|
| 1 | 활성화 기록 | `select_cpu()`가 TT/CBS **모든** wakeup에서 `activation_ns` 덮어씀 (`timpani-n/src/bpf/timpani.bpf.c:256`) | MOD-003, DET-005 | mid-job blocking → 새 활성화 오인. job당 1회 기록 규칙 없음 | P1(마커) / P2(분류) |
| 2 | 완료 감지 | CBS DMISS 판정이 매 `stopping()`에서 수행, 완료(`!runnable`) 구분 없음 (`:387-401`) | DET-002, PRT-002 | 선점/슬라이스 만료 시점에 조기 판정; 완료 기준 응답시간 미정의 | P1 |
| 3 | 보충 | Lazy만 구현(`cbs_lazy_replenish`). Backup Replenisher 스레드(DDR-007 §4.5.3) 미구현 | PRT-003, TIM-003 | 활동 없는 CPU에서 보충 지연 상한 없음 | P1 |
| 4 | CPU 고정 | `DSQ_CBS`(101)/`DSQ_THROTTLED`(102)가 전역 DSQ; `dispatch()`는 어떤 CPU든 consume (`:313-328`) | CBS-005 | L2가 비배정 Isolated CPU에서 실행 가능 → CPU별 Gap 분석 무효 | P1 |
| 5 | 승인 검사 | 이용률(`0.80 − U_tt − 0.02`)만 검사; `min_gap < 100 µs`는 경고만; D ≥ C, D ≤ MIT 미검증; TT 블로킹 미반영 (`timpani-o/src/global_scheduler.cpp:403-479`) | MOD-004, CBS-004, ORD-001 | D < MIT 마감 보장 불가 | P1(단순식) / P2(정밀식) |
| 6 | 서버 순서 | `DSQ_CBS` FIFO consume; 순서 정책 미정의 | ORD-002 | 승인 가정과 런타임 불일치 가능 | P1(FIFO 명문화) / P2(EDF) |
| 7 | MIT 위반 | BPF/N 어디에도 MIT 비교 없음. `libtimpani_rt`가 자체 SHM에만 기록(TIMPANI-N 미소비) | PRT-004/007 | 감지·계수·보고 부재 | P1 |
| 8 | 폴트 유형 | BPF `FaultEvent.fault_type` ∈ {DMISS, BUDGET_EXCEED}; O→Pullpiri `schedinfo.FaultType` ∈ {UNKNOWN, DMISS} → BUDGET_EXCEED/WATCHDOG가 UNKNOWN으로 소실 (`orchestrator_service.cpp:77-85`) | PRT-005, IF-004 | 유형 손실 | P1 |
| 9 | 폴트 폭주 | `remaining_us == 0`인 **매 stopping**마다 `BUDGET_EXCEED` 발행, FaultMonitor는 비-DMISS 무조건 전달 (`timpani.bpf.c:422`, `fault_monitor.cpp:112-115`) | PRT-006 | 스로틀 상태에서 O/Pullpiri로 폴트 폭주 | P1 |
| 10 | 식별 | 테이블 적용 시 `/proc` comm 1회 스캔(`main.cpp:305-310`), 재스캔 없음. `TaskRegistry`는 미사용 | DET-006, TIM-006 | 늦게 생성된 워커·재시작 스레드 미등록 | P1(재스캔) / P2(cgroup) |
| 11 | proto (Pullpiri→O) | `TaskInfo.period`를 MIT로 재사용; `TemporalClass`가 `SchedInfo`(워크로드) 단위 (`schedinfo.proto:45-92`) | IF-001/002 | MIT 전용 필드(P1)·태스크 단위 클래스(P2) 부재 | P1 / P2 |
| 12 | proto (O→N) | `ExecutionModel.EVENT_DRIVEN` 선언만; O는 항상 `TIME_TRIGGERED` 레이어에 `cbs_entries` 포함 (`global_scheduler.cpp:516`); `CbsConfig.period_us`가 MIT 겸용; 배정 CPU 미명시 | IF-003 | 레이어 구분·MIT·CPU 전달 부재 | P1 |
| 13 | 런타임 갱신 | 모든 변경이 전체 테이블 재발행 + epoch 갱신(+500 ms); `ScheduleTableUpdate`/`update_callback` 미연결; `TimerMaster::set_schedule_table`이 스레드 join으로 gRPC 스레드 블로킹 | RTU-002/003 | L2 추가가 전 노드 재동기·TT 스레드 재시작 유발 | P2 |
| 14 | 관측성 | 지터 통계 stdout만; `NodeStatus` 미전송; 깨움 지연은 `CONFIG_TRACE_BPF_EVENT` 빌드 옵션 + gpdata 파일 한정 | OBS-001~003 | 검증 프레임워크 입력 부재 | P1(트레이스·카운터) / P2(NodeStatus) |
| 15 | 마커 API | `libttsched`는 TT 전용(SHM 슬롯 TT만 할당); `libtimpani_rt`는 SOME/IP 식별자를 API에 포함, `cbs_key` 해시가 BPF와 불일치(`FNV("wl:task")` vs `FNV(wl)^FNV(task)`) | DET-004, IF-005, MOD-006 | L2용 공식·중립 마커 인터페이스 부재 | P1 |
| 16 | 선행 버그 | CPU별 vs 전역 hyperperiod 불일치(슬롯이 테이블 HP마다 1회 반복), CBS-only 노드 `hyperperiod_us = 0`, TT 이용률 `U_bound` 미검사 (`global_scheduler.cpp`) | CBS-001/004 전제 | Gap 분석 신뢰성 저하. 선행 수정 필요 | P1 |
| 17 | 설계 문서 불일치 | DDR-005 "ttsched_wait_next_period는 L1/L2 필수" vs DET-003 투명성; DDR-002 §7 TT floor vs DDR-007 미채택 | DET-003, §9-6 | DDR-008에서 결정 후 DDR-002/005 정합 갱신 필요 | P1(문서) |

---

## 11. 오픈 이슈 (Open Issues)

| # | 이슈 | 영향 | 결정 주체 / 문서 | Phase |
|:--|:--|:--|:--|:-:|
| OI-1 | 마커 없는 태스크의 mid-job blocking 정밀 구분 규칙(DET-005 P2): 시스템 콜 분류 집합, 블로킹 지속시간 임계 | 응답시간 측정 정확도 | DDR-008 후속 | P2 |
| OI-2 | EDF 순서(ORD-002 P2) 도입 시 승인식·BPF vtime 설계 | 승인식·dispatch | DDR-008 후속 | P2 |
| OI-3 | MIT 위반 처리 대안 정책(지연/폐기) 필요성 | 격리 vs 지연 | DDR-008 후속 | P2 |
| OI-4 | CBS 보충 의미론: 현행 "주기 격자 정렬 보충" 유지 vs 활성화 기준 보충(sporadic-server식). P1 결정 필요 | 승인식, BPF 변경 범위, 버스트 하 간섭 상한 | DDR-008 D-04 (P1: 격자 정렬 보충 유지로 결정 → 종결; 활성화 기준 보충 전환 여부는 DDR-008 F-2) | P1(종결) / P2 |
| OI-5 | TIM-001~008 목표값 확정을 위한 측정 환경·절차(RPi5 기준) | 합부 기준 | 테스트 전략 | P1 |
| OI-6 | TIM-004 P2 달성을 위한 커널 요건(HRTICK) 채택 여부 | 배포 요건 | DDR-008 후속 / SRS-000 REQ-ENV | P2 |
| OI-7 | 폴트 보고 경로 비동기화(현재 NodeStream 리더 스레드에서 동기 `NotifyFault`) | TIM-005, 노드 스트림 안정성 | DDR-012 개정 | P2 |
| OI-8 | `period` 재사용 하위 호환 전환 기간(IF-001) | Pullpiri 연동 | Pullpiri 협의 | P1 |
| OI-9 | 백업 보충의 CPU 재스케줄 유발 메커니즘(BPF syscall 프로그램의 `scx_bpf_kick_cpu` vs 보조 스레드 깨움) 가용성 PoC | PRT-003 구현 | DDR-008 | P1 |

---

## 12. 선행 샘플 앱(PR #50~#52)과의 관계 및 가이드 방향

### 12.1 현황
- [eclipse-timpani/timpani26#50](https://github.com/eclipse-timpani/timpani26/pull/50) (FCW L1/L2 예제 + 이상 주입), [#51](https://github.com/eclipse-timpani/timpani26/pull/51) (timpani-n 스레드 이름 식별), [#52](https://github.com/eclipse-timpani/timpani26/pull/52) (예산 초과 주입, 프레임 로그)는 **저장소에 존재하지 않는 과거 DDR-008 검토 초안**("DDR-008 §2.3", "DDR-008 LLD §1.3", "FR-041")을 근거로 작성되었다.
- 그 초안은 SOME/IP를 코어 계약에 직접 언급했고, 그 결과 `libtimpani_rt`가 service/instance/event ID를 인자로 받는 API, 자체 SHM(`/timpani_rt`), 라이브러리 측 MIT·E2E 폴트 판정(3 ms 하드코딩)을 갖게 되었다. TIMPANI-N은 이들을 소비하지 않으므로 현재 "라이브러리 로그"에 머문다.
- PR #51의 스레드 이름 식별은 본 요구사항(DET-006 P1의 전제)과 부합하며 유지한다.

### 12.2 가이드 방향 (상세는 HLD-008 §샘플 앱 가이드, LLD-008)
본 설계가 확정되면 샘플 앱에 다음 방향의 변경 가이드를 제공한다.

| 항목 | 현행(PR #50/52) | 가이드 방향 (P1) |
|:--|:--|:--|
| 이벤트 등록 | `timpani_evt_register(wl, task, svc, inst, evt, mit)` | 제거. TIMPANI는 Pullpiri `SchedInfo`(MIT 필드)로만 L2를 안다. 미들웨어 구독은 앱이 처리 |
| 도달 표시 | `timpani_evt_mark_arrival(svc, inst, evt, seq)` → 자체 SHM | `ttsched_evt_signal(task_name, t_arr_ns)` (중립 마커, DET-004(b)) — 수신 스레드(vsomeip 콜백)에서 호출 |
| 워커 wait point | `std::condition_variable` 대기 | `ttsched_evt_wait()` (DET-004(a)) 또는 표준 블로킹 유지(그 경우 A-06 준수) |
| CBS bind/unbind | `timpani_cbs_bind_thread/unbind` (스케줄링 효과 없음) | 제거. job 경계는 wait/signal로 결정됨 |
| MIT·E2E 판정 | 라이브러리 내부(3 ms 하드코딩) | 제거. TIMPANI-N이 `MIT_VIOLATION`/`DMISS`를 판정·보고(PRT-004/005). 앱은 자체 로그만 유지 가능 |
| 서버 키 해시 | `FNV("wl:task")` | 앱에서 해시 불필요(제거) |
| 워크로드 등록 | `mock_pullpiri.yaml` SPORADIC, `period`=MIT | MIT 전용 필드 사용(IF-001). 전환 기간 `period` 허용 |
| 이상 주입 옵션 | `--burst-ms`, `--overrun-ms`, `-be`, `--wakeup-delay-ms`, `--tt-overrun-ms` | 유지. AC-008-03/04/07의 검증 수단으로 채택 |
| 검증 | 앱 stdout 로그 | TIMPANI-N 트레이스 채널(OBS-002) 기준으로 판정. 앱 로그는 보조 |

### 12.3 역할 분담 원칙
- **TIMPANI 저장소**: 요구사항·설계 확정 → `timpani-o`/`timpani-n`/`libttsched` 구현 → 샘플 앱 변경 가이드 발행.
- **샘플 앱(FCW)**: 가이드에 따라 어댑터(vsomeip ↔ 마커 API)로 축소. SOME/IP 고유 코드는 `examples/someip` 안에만 존재.
- 두 작업은 **설계 확정 이후** 병행하되, 인터페이스(마커 API 시그니처, proto 필드)는 LLD-008 확정본을 단일 기준으로 삼는다.
