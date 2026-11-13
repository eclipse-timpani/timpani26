<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-008: Event-Driven Workload Support — 설계 결정 기록

**날짜:** 2026-11-10
**상태:** Proposal (Phase 1 결정 확정 대기)
**작성:** Human + AI
**관련 요구사항:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), SRS-000 `REQ-FUN-004/009/013/014`, `REQ-NFR-003/004`, `REQ-ED-001`
**관련 DDR:** [DDR-001](DDR-001-workload-model.md), [DDR-002](DDR-002-scheduling-architecture.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-006](DDR-006-communication-architecture.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md)
**하위 문서:** HLD-008, LLD-008

> 본 문서는 SRS-000이 참조하던 `DDR-008`의 **정본**이다. 과거 검토 초안(저장소 미반영, SOME/IP 특정)은 폐기하며, PR #50~#52가 인용한 "DDR-008 §2.3 / LLD §1.3 / FR-041"은 본 문서로 대체된다.
> 각 결정은 **D-NN** 번호를 가지며, Phase(P1/P2)와 대안·결과를 기록한다. HLD/LLD는 결정을 재논증하지 않고 D-NN을 참조한다.

---

## 요약 (Korean Summary)

- TIMPANI는 REQ-008 §2의 **공통 Event/Job 모델**(Event source·Event·Job, 생명주기 사건 4종, 규칙 R1~R4)에 대응하도록 설계하며, **모델 / 관측 어댑터 / 집행**의 3층으로 구현한다. 미들웨어 의미론은 애플리케이션 측 어댑터에 둔다(D-01).
- 관측 어댑터 K(커널): BPF의 **태스크별 job 상태 머신(WAITING/ACTIVE)**이 Activation/Start/Completion을 관측한다. P1 규칙은 마커 사용 여부와 무관하게 "자발적 수면 = 완료, 그 후 첫 wakeup = 활성화"이며 집행은 항상 이 관측을 쓴다(D-02). 관측 어댑터 M(마커): `libttsched`의 `ttsched_evt_open/notify/wait`(futex·SHM 기반)가 Arrival(`event_seq`, $t_{arr}$, 도달 기준 MIT)과 배치 소비(`n_events`)를 제공한다(D-03). 시스템 콜 분류 기반 정밀 규칙은 P2.
- CBS 보충은 **활성화 기준 보충 윈도우**로 바꾼다: 활성화 시 직전 윈도우가 끝났으면 예산을 채우고, 스로틀된 서버는 윈도우가 끝난 뒤 첫 dispatch에서 채워 큐 뒤로 보낸다(새 윈도우는 그 시각부터). 선언을 지켜 온 서버는 스로틀되지 않고, 위반의 비용은 위반한 태스크만 진다. 현행 "주기 격자 정렬 보충"은 MIT를 지키는 태스크도 스로틀시킬 수 있어 기각했다(D-04). **백업 Replenisher 스레드**를 필수화하되 서버 상태는 BPF만 쓴다(D-07). MIT 위반은 도달 기준(M) 우선·활성화 기준(K) 근사로 계수하고 "잔여 예산 내 흡수"(D-05).
- L2 태스크는 **CPU별 DSQ**로 배정 CPU에 고정하고(D-06), 동일 CPU 서버 순서는 P1 **활성화 순서 FIFO**(스로틀 후 승격된 서버는 뒤로), P2 EDF(D-08).
- 승인은 이용률 + **FIFO 전제 블로킹 합 충분조건**($C_i + B_{tt} + L_{wake} + \sum_{j\ne i} C_j \le D_i$)(D-09). CPU의 모든 L2 태스크에 대해 검사한다.
- 폴트는 `DMISS / BUDGET_EXCEED / MIT_VIOLATION`을 유형 손실 없이 전달하고, `BUDGET_EXCEED`는 에피소드당 1회(D-10). proto는 **필드 추가만**(D-11).
- P1 구현 대상은 **C++ 경로**(timpani-o/timpani-n C++ + BPF)이며 Rust 포트는 후속(D-15). TT floor·watchdog은 P1 미채택(D-13).
- 상태: Proposal. OI로 남긴 PoC(백업 보충의 CPU 재스케줄 유발 수단) 확인 후 Accepted 전환.

---

## 1. 컨텍스트 (Context)

### 1.1 문제
REQ-008 §1.3/§10이 정리한 바와 같이, `timpani26`에는 L2(CBS) 경로가 end-to-end로 존재하지만 Event-Driven 워크로드의 **동작 모델**(Event·Job 정의, 활성화/완료 기준, 보충 결정론, CPU 고정, 승인 충분조건, 폴트 분류)이 미정의이거나 결정론 요구를 만족하지 못한다. 또한 선행 PoC(`examples/someip/libtimpani_rt`)가 SOME/IP를 코어 계약에 끌어들였다.

### 1.2 설계 세력 (Forces)

| 세력 | 설명 | 출처 |
|:--|:--|:--|
| **결정론** | 모든 타이밍 주장은 측정 가능한 바운드여야 한다 | SRS-000 REQ-NFR-001, REQ-008 TIM |
| **FFI** | L2의 폭주·초과가 L1·타 L2로 전파되면 안 된다 | SRS-000 REQ-NFR-002 |
| **투명성** | 앱이 표준 대기 메커니즘을 그대로 써야 한다 | SRS-000 REQ-NFR-003, REQ-008 DET-003 |
| **전송 계층 중립** | 코어에 미들웨어 의미론 금지 | REQ-008 MOD-006 |
| **단순성(P1)** | 현실적·단순한 기본 기능으로 FCW 샘플 완성 | REQ-008 §0.1 |
| **기존 결정 존중** | DDR-001~007·011·012의 모델·메커니즘 유지, 필요한 최소 수정만 | 본 DDR §6 |
| **RT 경로 순수성** | BPF/RT 스레드에 gRPC·할당·블로킹 금지 | DDR-006 §4 |
| **커널 제약** | BPF에서 futex wake 불가, `bpf_timer`는 PREEMPT_RT에서 부적합, 슬라이스 만료는 틱 의존 | DDR-007 §4.5.1, REQ-008 C-01/02 |
| **proto 거버넌스** | 필드 번호 유지·추가만, 명시 승인 | `.github/copilot-instructions.md` |

---

## 2. 결정 (Decisions)

### D-01. 공통 Event/Job 모델과 3층 구조(전송 계층 중립) — P1

**결정**
1. **모델을 먼저 고정한다.** REQ-008 §2의 공통 모델을 TIMPANI의 유일한 기준으로 삼는다: Event source(선언된 발생원, 속성 MIT) → Event(`(source, event_seq, t_arr)`) → Job(`(task, job_seq, n_events ≥ 1, t_act, t_start, t_done)`), 생명주기 사건 Arrival/Activation/Start/Completion, 규칙 R1(대기 중 도달 → 활성화) / R2(처리 중 도달 → 미소비 상태로 보관) / R3(완료 시 미소비 Event가 있으면 즉시 다음 Job) / R4(Job은 wait point를 떠날 때까지 도달한 미소비 Event를 모두 소비). 정상 조건에서 Job:Event = 1:1(I1).
2. **3층으로 구현한다.** (①) 모델: 애플리케이션 계약, 폴트 의미, 관측 출력(Job 레코드, Event source 통계)은 모델 용어로만 정의한다. (②) 관측 어댑터: **K** = BPF 커널 관측(D-02), **M** = `libttsched` 마커(D-03). P2에 미들웨어 경계(X), 선행 태스크 완료(D)를 어댑터로 추가한다. (③) 집행: CBS 예산·CPU 고정·순서·DMISS 판정(D-04~D-10)은 P1에서 어댑터 K가 관측한 사건만 사용한다. 따라서 어떤 어댑터 조합에서도 CBS 집행(예산, CPU 고정, 순서)과 $t_{act}$ 기준 DMISS 판정은 같다. 어댑터에 따라 달라지는 것은 MIT 위반의 원천과 정밀도(D-05), 그리고 Event 계수의 유무다. M은 도달 정보(계수, $t_{arr}$, 도달 기준 MIT)와 유실 없는 배치 소비(R2~R4)를 더하며, P1에서 커널이 관측하는 Job 경계는 바꾸지 않는다.
3. **전송 계층 중립.** 코어(O/N/BPF/`libttsched`)의 proto·API·자료구조는 미들웨어 식별자(서비스/인스턴스/이벤트/토픽 ID)와 미들웨어 헤더를 포함하지 않는다. 미들웨어 수신 → Event 도달 통지의 연결은 **애플리케이션(또는 앱 측 어댑터)**이 표준 대기 메커니즘(K만 사용) 또는 마커 API(M)로 수행한다.
4. **P1 제한.** Event source는 L2 태스크당 1개이며 암묵 선언한다(식별자 = `(workload_id, task_id)`, MIT = `TaskInfo.min_inter_arrival_us`). proto에 Event source 메시지를 추가하지 않는다. `task_id`는 15바이트 이하이고 같은 노드 안에서 유일해야 하며 TIMPANI-O가 검증한다(D-09 (4)). 그래서 마커 API와 SHM 슬롯은 `task_id`만으로 source를 찾는다. 태스크당 다중 source의 명시 선언은 P2(REQ-008 OI-16)이며, 마커 API는 처음부터 **source 핸들**을 인자로 받아 그 확장에서 시그니처가 바뀌지 않게 한다.

**근거**: 이전 초안은 Event를 "태스크를 Runnable로 바꾸는 자극"으로, Job 경계를 sched_ext 콜백으로 정의해 모델이 구현 수단에 종속되었다. 그 결과 Event 1개 = wakeup 1개가 암묵 전제가 되어 처리 중 도달(유실 방지를 위한 통지 합침)과 도달 기준 MIT를 표현할 수 없었다. 모델을 분리하면 (a) 도달과 처리를 따로 계수할 수 있고, (b) 어댑터의 관측 한계를 모델 위반이 아닌 "관측 정밀도"로 다룰 수 있으며, (c) P2 어댑터가 집행 코드를 건드리지 않는다.

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| A. 코어가 SOME/IP 계약 보유(선행 PoC) | FCW에 즉시 맞춤 | DDS/CAN/zenoh마다 코어 수정; 인증 범위 확대; TIMPANI가 메시지 의미론을 알게 됨 | 기각 |
| B. TIMPANI-N에 미들웨어 플러그인 계층 | 전송별 관측 정밀 | N에 미들웨어 의존성·스레드 추가, RT 경로 오염 위험, P1 범위 초과 | P2 검토(관측 전용 플러그인) |
| **C. 공통 모델 + 관측 어댑터(커널/마커) + 앱 측 미들웨어 어댑터** | 코어 단순·인증 범위 최소·확장성; 모델이 구현 수단과 독립 | 미들웨어 내부 지연($t_{arr}$↔$t_{act}$)과 도달 계수는 앱이 마커로 주입해야 함 | **채택** |
| D. Event ≡ 커널 wakeup(이전 초안) | 구현이 가장 작음 | Event와 Job을 구분할 수 없음: 처리 중 도달·도달 기준 MIT·Event 식별이 표현 불가, 마커는 커널 추론을 덮어쓰는 힌트로 전락 | 기각 |

**결과**: `libtimpani_rt`의 `timpani_evt_register(svc,inst,evt,...)`, `timpani_cbs_bind/unbind`, 자체 SHM `/timpani_rt`, 라이브러리 측 MIT/E2E 판정은 폐기 대상(REQ-008 §12). 샘플 앱은 vsomeip 콜백에서 `ttsched_evt_notify()`를 호출하는 어댑터로 축소된다. 워크로드 선언(proto)은 MIT 필드 외에 바뀌지 않는다.

---

### D-02. 관측 어댑터 K: BPF 태스크별 job 상태 머신 — P1 (규칙 정밀화는 P2)

**결정**: 모델의 Activation/Start/Completion을 커널에서 관측하는 어댑터 K를 BPF 상태 머신으로 구현한다. 집행(D-04~D-10)은 이 관측만 사용한다. `task_meta_map`의 `TaskMeta`에 job 상태 `{WAITING, ACTIVE}`, `job_seq`, `t_act`, `t_start`, `last_act_ns`, 플래그를 추가한다. 상태 전이는 다음과 같다.

```text
WAITING --(ops.runnable: 수면에서 깨어남)--> ACTIVE
          · t_act = now, job_seq++, MIT 검사(last_act_ns), 보충 윈도우 검사(D-04), activation 기록 1회
ACTIVE  --(ops.enqueue: 선점·슬라이스 만료 후 재삽입)--> ACTIVE (t_act 불변)
ACTIVE  --(ops.stopping: 실행 중단)--> ACTIVE
          · 예산 차감. 마감이 이미 지났으면 Job당 1회 조기 DMISS(MAY, FR-B-021). 완료 판정은 quiescent에서 한다
ACTIVE  --(ops.quiescent(SCX_DEQ_SLEEP): 자발적 수면)--> WAITING
          · t_done = now, 응답시간 판정(DMISS), job 레코드 발행
```

활성화의 유일한 계기는 상태가 `WAITING`일 때의 `ops.runnable`이다. `ops.enqueue`는 깨움이 아닌 재삽입 경로에서도 호출되므로 활성화 판정에 쓰지 않는다. 수면이 아닌 dequeue(속성 변경 등, `SCX_DEQ_SLEEP` 없음)는 완료가 아니며 그 뒤의 `ops.runnable`은 상태가 `ACTIVE`이므로 활성화가 아니다.

**wait point 분류 규칙**
- **P1**: L2 태스크의 **모든** 자발적 수면(`ops.quiescent`, `SCX_DEQ_SLEEP`)을 wait point로 본다. 마커를 쓰는 태스크도 같다 — P1 BPF는 마커 wait와 다른 수면을 구분하지 않는다. 따라서 "Job 처리 도중 블로킹하지 않는다"(REQ-008 A-06)는 **모든** L2 태스크의 전제다.
- **P2**: 정밀화. 보조 BPF 프로그램(`tp_btf/sys_enter`, 등록 TID 필터)이 블로킹 직전 시스템 콜을 분류하여 `TaskMeta.wait_kind`에 기록: 이벤트 대기 계열(`epoll_wait/epoll_pwait2/poll/ppoll/select/pselect6/recvmsg/recvfrom/recvmmsg/read*`), 마커 futex(`FUTEX_WAIT_BITSET` + 매직 비트셋, D-03), 기타(뮤텍스 futex 등 = mid-job). mid-job으로 분류된 블로킹이 `midjob_block_max_us`(기본 $D$)를 초과하면 다음 wakeup에서 사후적으로 "완료@block_ns + 새 활성화"로 재분류한다.

**모델 대비 관측 한계(REQ-008 §2.4 K)** — 마커 태스크에도 (a)(c)(d)가 그대로 적용된다.
- (a) **mid-job blocking이 Job을 분할한다**(A-06 위반 시). 분할된 조각의 응답 시간은 실제보다 짧게 측정되어 마감 실패를 놓칠 수 있고, 조각 사이의 간격이 MIT보다 짧으므로 MIT를 지키는 source에도 활성화 기준 `MIT_VIOLATION`이 거짓으로 관측된다. 즉 A-06을 어긴 태스크에 대한 K의 DMISS·MIT 판정은 유효하지 않다. 마커 source는 도달 기준 MIT(D-05)를 쓰므로 거짓 MIT 위반은 생기지 않는다.
- (b) **처리 중 도달(R2)은 보이지 않는다.** 도달 계수는 어댑터 M이 담당한다.
- (c) **back-to-back Job(R3)이 하나의 구간으로 병합된다.** 수면이 없어 `quiescent`가 호출되지 않기 때문이다. 병합 구간은 선행 Job의 $t_{act}$부터 후행 Job의 $t_{done}$까지이므로 마감 실패를 놓치지는 않지만 후행 Job에 대해 거짓 DMISS를 낼 수 있다. I1에 의해 이 상황은 MIT 위반 또는 선행 Job의 마감 초과에서만 발생한다. 병합된 Job들은 예산을 새로 받지 않는다(D-04 규칙 4).
- (d) **응답 시간은 $t_{act}$부터 잰다.** 미들웨어·깨움 지연($t_{act} − t_{arr}$)이 빠지므로 그만큼 낙관적이다(REQ-008 A-04). $t_{rel}$ 기준 응답 시간은 오프라인 대조로 산출한다(D-16).

정밀 관측은 P2다(REQ-008 OI-17). 후보: 마커 라이브러리가 wait point에서(수면 없이 복귀하는 경우 포함) `sched_yield()`를 호출하고 BPF `ops.yield`가 마커 사용 플래그가 선 태스크에 한해 Job 경계를 기록하는 방법, 또는 위 P2 시스템 콜 분류. 전자는 BPF 변경이 작아 PoC 결과에 따라 P1로 앞당길 수 있다(F-8).

**현행 대비 변경**: `select_cpu()`에서 매 wakeup마다 `activation_ns`를 덮어쓰는 코드(`timpani.bpf.c:256`)를 제거하고, 상태 머신으로 대체. CBS DMISS 판정을 매 `stopping`에서 **완료 시점**으로 옮긴다. 확정 판정은 Completion에서 내린다. 마감이 지난 뒤 처음 실행이 중단되는 시점에 Job당 최대 1회 조기 `DMISS`(`flags.early`)를 낼 수 있으며(MAY, SRS-008 FR-B-021), 조기 보고한 Job은 완료 시 다시 보고하지 않는다.

**대안**: (i) 매 wakeup = 활성화(현행) — mid-job 오염, 기각. (ii) 사용자 공간 EventMonitor가 SHM 마커만으로 판정 — BPF 집행과 괴리, 마커 없는 앱 지원 불가, 기각. (iii) uprobe/USDT로 라이브러리 함수에 훅 — 컨테이너별 attach 운영 부담, 1–2 µs/호출, P2 후보로 보류.

---

### D-03. 관측 어댑터 M: `libttsched` 마커 API(futex/SHM 확장) — P1

**결정**: 기존 `libttsched`(SHM `/timpani_ttsched`, 태스크 슬롯 카운터 + futex)를 L2 Event source로 확장하여, 모델의 Arrival과 R2~R4를 애플리케이션 쪽에서 관측·구현하는 어댑터 M을 제공한다.
- **SHM v2 레이아웃**: `TIMPANI_MAX_TASKS` 32→64; 헤더에 `version`과 `instance_id`(객체를 만들 때마다 다른 값) 추가, `magic` 값 갱신(ABI 단절 명시). L2 태스크도 슬롯을 할당받으며(현행 TT 전용 해제) 슬롯 하나가 Event source 하나의 상태를 담는다: `kind`(미사용/TT/EVT), `counter`(대기·깨움용 futex 워드), `event_seq`(누적 도달 수), `consumed_seq`, `last_arr_ns`, `last_viol_ns`, `mit_us`(TIMPANI-N 게시), `mit_viol_count`. **누적 값은 `event_seq`, `consumed_seq`, `mit_viol_count`**이며 TIMPANI-N이 주기적으로 읽어도 증분이 유실되지 않는다. `last_arr_ns`·`last_viol_ns`는 최근 시각, `mit_us`는 설정값이다. `counter`를 `event_seq`와 분리한 이유: source를 비활성화하거나 데몬이 종료할 때 대기자를 깨우려면 futex 워드를 바꿔야 하는데, 그 변화가 Event 도달로 계수되면 안 된다.
- **슬롯 규약**(writer는 TimerMaster 하나, 모든 저장은 원자 연산):
  1. **고정 배정**: 한 번 L2 `task_id`에 배정한 슬롯은 데몬(SHM 객체)이 살아 있는 동안 그 이름의 것이다. 다른 이름에 재사용하지 않고 0으로 지우지도 않는다. source가 테이블에서 빠지면 **비활성**(`kind = 미사용`)이 되고, 다시 들어오면 같은 슬롯이 **재활성**되어 누적 값이 이어진다. 그래서 통지자가 다른 source의 슬롯을 건드리는 일이 구조적으로 없고, 누적 값이 거꾸로 가지 않는다. 데몬 수명 동안 등장하는 서로 다른 L2 `task_id`가 남은 슬롯 수를 넘으면 그 엔트리의 적용을 실패로 보고한다.
  2. **유효성**: EVT 슬롯은 `kind == EVT`이고 이름이 일치하면 유효하다. `magic`/`generation`에 의존하지 않으므로 TT 테이블을 다시 발행하는 동안(기존 `magic = 0` 구간)에도 유지되는 source의 통지·대기는 끊기지 않고 Event를 잃지 않는다.
  3. **순서**: 새 배정은 이름과 `mit_us`를 쓴 뒤 **마지막에** `kind = EVT`. 비활성화와 종료는 `kind = 미사용` → `counter++` → 깨움. `event_seq`는 건드리지 않는다. 대기 중인 소비 태스크는 반드시 복귀해 −1을 받는다(잠들기 직전이었다면 `counter`가 바뀌어 대기가 즉시 실패한다).
  4. **TT 슬롯**은 기존 `magic`-last/`generation` 핸드셰이크를 그대로 쓴다. 현행 "재발행 시 전 슬롯 `counter` 증가"는 TT 슬롯에만 적용한다.
  5. **시작**: 이전 데몬이 비정상 종료해 SHM 객체가 남아 있으면, 새 데몬은 그 객체를 열어 종료 절차(3)를 수행한 뒤 unlink하고 새 객체를 배타적으로 만든다. 새 객체에는 새 `instance_id`를 쓰고, `shm_open`의 모드는 umask에 깎이므로 `fchmod`로 권한을 맞춘다. 남은 객체에 붙어 있던 앱은 −1을 받고 새 객체로 옮겨 온다.
- **`ttsched_evt_t ttsched_evt_open(const char *task_id)`**: Event source 핸들 획득. 프로세스 내 고정 크기 테이블(힙 없음, 최대 16개)에 이름을 저장하고 슬롯 인덱스를 **원자 정수 하나**로 캐시한다. source가 아직 등록되지 않았어도 핸들은 발급되며, notify 시점에 찾는다.
- **`int ttsched_evt_notify(ttsched_evt_t src, uint64_t t_arr_ns)`** — **Arrival**. 수락 조건: SHM(v2)이 쓰기 가능으로 매핑되어 있고 자신의 슬롯이 `kind == EVT`다. 아니면 **−1을 반환하고 Event를 계수하지 않는다**(판정이 증가보다 먼저이므로 −1이면 계수되지 않았음이 보장된다. 앱은 방금 큐에 넣은 항목이 처리되지 않게 한다). 수락하면 `prev = exchange(last_arr_ns, t_arr)`(`t_arr_ns`가 0이면 `CLOCK_MONOTONIC` now); `t_arr − prev < mit_us`이면 `last_viol_ns = t_arr`, `mit_viol_count++`(도달 기준 MIT, D-05); `event_seq++`; `counter++`; `FUTEX_WAKE(1)`; 0 반환. 여러 스레드가 같은 source에 동시에 통지해도 Event는 유실되지 않으며, 도달 기준 MIT 계수의 정확성은 source당 notify가 직렬화된 경우에 보장한다. 힙 할당 없음, 시스템 콜 1회.
- **`int ttsched_evt_wait(void)`** — **wait point**. 소비 태스크 자신의 슬롯에서 반복마다 `w = load(counter)` → 슬롯 유효성 확인(아니면 −1) → `e = load(event_seq)`를 **한 번** 읽는다. `e != consumed_seq`이면 `n = e − consumed_seq`를 계산하고 `consumed_seq = e`를 저장한 뒤 n을 반환한다(R3/R4: 같은 스냅샷 `e`를 비교·계산·저장에 쓰므로 그 사이에 도달한 Event가 소비된 것으로 처리되어 사라지지 않는다). 같으면 `FUTEX_WAIT_BITSET(counter, expected = w, val3 = TTSCHED_EVT_BITSET)`로 블로킹하고(R1), 깨어나면 새 스냅샷으로 다시 판단한다. 처리 중 도달한 Event(R2)는 `event_seq`에 누적되어 다음 Job의 `n`으로 전달된다. `consumed_seq`는 소비 태스크만 쓰고, source당 wait 호출자는 하나다(그 `task_id`를 comm으로 가진 스레드). EVT 대기는 SHM 쓰기 매핑이 필요하다(읽기 전용이면 −1). −1은 지금 대기할 수 없다는 뜻이다(데몬 종료, source 비활성화 또는 미등록, 매핑 불가): 앱은 큐를 그대로 두고 `ttsched_init()` 후 다시 시도한다. 같은 SHM 객체 안에서는 계수가 이어지기 때문이다. 데몬이 재시작하면 새 객체의 계수가 0에서 시작하므로, wait는 직전에 붙어 있던 것과 다른 객체(`instance_id`가 다름)에 처음 붙을 때 **`TTSCHED_EVT_RESET`(−2)**을 한 번 반환한다: 앱은 큐를 비우고 다시 시도한다. 매직 비트셋은 P2 시스템 콜 분류(D-02)에서 마커 wait를 식별하는 수단이며 P1 BPF 동작에는 영향이 없다.
- **보장 범위**: 수락된 Event는 데몬이 살아 있는 동안 유실·중복 없이 전달된다. source가 비활성화되었다가 재활성화되어도 미소비 Event는 보존된다. 데몬이 종료·재시작하면 계수는 0에서 다시 시작하고 소비 태스크는 −2를 한 번 받는다. 재시작 경계에서 수락된 Event는 버려질 수 있으나 그 뒤의 Event와 큐 항목은 어긋나지 않는다.
- 애플리케이션 계약: 큐에 넣은 뒤 notify한다(−1이면 넣은 항목이 처리되지 않게 한다: 큐에서 빼거나 무효로 표시). `n = ttsched_evt_wait()` 후 자신의 큐에서 **정확히 n건**(큐 길이를 넘지 않게)을 처리한다("빌 때까지 처리" 금지). wait가 −1을 반환하면 큐는 그대로 두고 `ttsched_init()` 후 다시 시도한다. −2를 반환하면 큐를 비우고 다시 시도한다. 정상 조건에서 n = 1이다(I1).
- 도달 정보의 용도(P1): (1) 도달 기준 MIT 위반 계수 → TIMPANI-N이 보고(D-05, D-10), (2) Event source 통계(도달·소비·미소비) → 트레이스 채널(D-16), (3) $t_{arr}$ 기록. DMISS·깨움 지연 판정은 어댑터 K의 $t_{act}$ 기준이다(REQ-008 A-04). P2 미들웨어 경계 관측(X)은 같은 슬롯에 도달을 기록하는 또 하나의 notifier가 된다.
- 신뢰 경계: P1의 SHM 권한은 `0666`이어서(TimerMaster가 객체를 만든 뒤 `fchmod`로 맞춘다) 호스트의 임의 프로세스가 슬롯을 쓸 수 있다. 통지의 누락·조작이나 슬롯 변조는 해당 source의 도달·MIT 통계와 소비 태스크가 받는 `n`(헛깨움, 잘못된 건수)에 영향을 준다. 그래서 앱은 `n`을 자신의 큐 길이로 제한한다. 반면 CBS 집행과 DMISS 판정은 커널 관측만 쓰므로 다른 태스크에 대한 격리(FFI)는 깨지지 않는다.

**대안**: (i) 앱이 pinned BPF 맵을 직접 갱신 — 권한(`CAP_BPF`/pinned path 노출) 문제·컨테이너 마운트 의존, 기각. (ii) 별도 라이브러리 `libttevent` — ABI/SHM 이중화, 기각(오픈 이슈 OI-14는 "libttsched 확장"으로 종결). (iii) eventfd 기반 — fd 전달 필요, futex 대비 지연 큼(DDR-005 표), 기각. (iv) 이름 기반 `signal(task_name, t_arr)` + 반환값 없는 `wait()`(이전 초안) — 처리 중 도달을 표현하지 못해 유실 또는 빈 Job이 생기고, 호출마다 이름 탐색이 필요하며, 다중 source 확장 시 시그니처가 바뀜, 기각.

**OI-15 결정**: P1에서 `ttsched_evt_notify`의 호출자는 같은 프로세스의 스레드를 기본으로 하고, SHM 쓰기 권한이 있는 같은 호스트의 다른 프로세스도 허용한다. P1의 `0666`은 호출자를 제한하지 않으며 그 영향은 위 신뢰 경계와 같다(P1 범위 확정으로 종결). 쓰기 권한을 소유 그룹으로 줄이는 것과 크로스 프로세스 정책은 P2.

**결과**: `libttsched.h` 소비자(timpani-n, sample-apps, 외부 TT 앱) 재컴파일 필요(DDR-011 §1.1과 동일한 유형의 ABI 단절). `timpani_rt_shm.h`/`/timpani_rt`는 폐기.

---

### D-04. CBS 보충 의미론: 활성화 기준 보충 윈도우 + 백업 보충 필수 — P1

**결정**: 서버 $s$는 잔여 예산 `remaining`과 현재 보충 윈도우의 끝 `replenish_at`을 가진다. 현행 격자 정렬 지연 보충(`replenish_at_ns += k·T_s`)을 버리고 다음 규칙을 쓴다.

1. **활성화 시**(WAITING→ACTIVE, D-02): `t_act + tol ≥ replenish_at`이면 `remaining = C_s`, `replenish_at = t_act + T_s`. 그렇지 않으면(직전 윈도우가 아직 끝나지 않음: MIT 위반 활성화, 분할된 Job 조각, 또는 이전 위반·초과의 여파) 예산과 윈도우를 바꾸지 않고 그 Job에 **예산 이월 표식**(`CARRYOVER`)을 남긴다.
2. **소진 시**: `remaining`이 0이 되면 Throttled가 되고 `BUDGET_EXCEED`를 1회 낸다(D-10).
3. **승격 시**: Throttled 서버는 `now ≥ replenish_at`이 되면 배정 CPU의 dispatch 경로에서 `remaining = C_s`, `replenish_at = now + T_s`로 갱신되고 L2 큐의 **뒤**로 들어간다(D-08).
4. 활성 상태이고 Throttled가 아닌 서버는 윈도우가 끝나도 예산을 다시 채우지 않는다. 다음 보충은 규칙 1 또는 3에서만 일어난다.
5. `tol`(`CBS_ACT_TOL_US`)은 깨움 지연의 변동 때문에 연속한 두 활성화의 간격이 MIT보다 조금 짧게 관측되는 경우를 흡수한다. 활성화 기준 MIT 검사(D-05)도 같은 값을 쓴다. P1에서 `tol`과 승인식의 $L_{wake}^{max}$(D-09)는 같은 빌드 상수(50 µs)이며, 한쪽을 바꾸면 다른 쪽도 같이 바꾼다($tol \ge$ 활성화 시각 지터의 상한이어야 한다).
6. 서버가 동작하는 동안 `remaining`, `replenish_at`, Throttled 상태는 **BPF만**, 배정 CPU의 런큐 잠금 아래에서 쓴다. 사용자 공간은 서버를 만들 때(태스크가 `SCHED_EXT`에 들어오기 전)만 초기값을 쓰고 그 뒤로는 읽기만 한다(D-07, D-17).

DDR-007 §4.5.3의 **Replenisher 스레드(Backup)를 필수 구성요소로 격상**한다(D-07). 배정 CPU가 유휴일 때 규칙 3이 제때 실행되게 하는 것이 그 역할이다.

**성질**
- **(P-a) 선언을 지켜 온 서버는 스로틀되지 않는다.** 모든 도달이 MIT를 지키고 Job당 차감 실행 시간(활성화부터 wait point에서 잠들 때까지, 대기 진입 경로 포함)이 $C_s$ 미만이며 $R \le D \le T_s$이면, 직전 윈도우는 `t_prev + T_s`에 끝나고 다음 활성화는 `t_act ≥ t_prev + T_s − tol`이므로 규칙 1이 매 Job에 온전한 $C_s$를 준다. 귀납적으로 서버는 스로틀되지 않는다.
- **(P-b) 윈도우당 소비 상한.** 한 보충 윈도우 안에서 서버의 소비는 $C_s$(+ 틱 해상도에 의한 overrun, SRS-008 TMG-004) 이하다. 윈도우의 시작 간격은 $T_s − tol$ 이상이므로 길이 $L$인 임의 구간의 소비는 $(\lceil L/(T_s − tol) \rceil + 1) \cdot C_s$ 이하다. 이 상한은 서버가 위반하든 아니든 성립한다.
- **(P-c) 다른 서버가 앞서 실행하는 양은 서버당 $C_j$ 이하.** Job $i$가 활성화된 시점에 서버 $j$가 큐에서 앞에 있거나 실행 중이면 $j$의 잔여 예산은 $C_j$ 이하다. $j$의 예산이 다시 채워지는 경우는 둘뿐이고(규칙 1의 활성화, 규칙 3의 승격) 둘 다 $j$를 $i$ 뒤로 보낸다(D-08: 새 활성화는 활성화 시각 순위, 승격은 승격 시각 순위). 수면 없이 이어지는 back-to-back Job들(D-02 한계 (c))은 스로틀되지 않는 한 예산을 새로 받지 않는다(규칙 4). 따라서 $j$가 $i$보다 앞서 쓰는 양은 $i$의 활성화 시점에 남아 있던 예산을 넘지 않는다. D-09의 $\sum_{j \ne i} C_j$ 항은 이 성질에 근거하며, $j$가 MIT를 위반하거나 예산을 초과해도 성립한다.

**위반 후 동작(위반의 비용은 위반한 태스크가 진다)**: MIT 위반 활성화가 잔여 예산을 다 쓰면 윈도우 끝까지 스로틀되었다가 **다음 윈도우의 예산**으로 실행된다(규칙 3). 즉 위반 Job은 미래의 예산 $C_s$를 빌려 쓰고, 서버의 윈도우는 도달 위상보다 뒤로 밀린다. 그 뒤의 Job은 간격이 MIT 이상이어도 윈도우가 끝나기 전에 활성화되므로 새 예산을 받지 못하고(규칙 1, `CARRYOVER` 표식) 남은 예산으로 실행되며, 모자라면 윈도우 끝까지 기다린다.

```text
T_s = 18 ms, C_s = 2 ms. 매 Job이 예산을 전부(2 ms) 쓰는 경우:
t= 0.0  J1 활성화 → 보충, 윈도우 [0, 18).  J1 실행 0~2
t=15.0  J2 도달(간격 15 ms: MIT 위반). 윈도우가 끝나지 않음 → 보충 없음, 잔여 0 → 18.0까지 스로틀
t=18.0  승격: 예산 2 ms, 윈도우 [18, 36).  J2 실행 18~20
t=33.0  J3 도달(J2로부터 18 ms). 33 < 36 → 보충 없음(CARRYOVER), 잔여 0 → 36.0까지 스로틀 → 응답 시간이 약 3 ms 늘어남
```

빌린 예산은 이후 Job들의 여유로 갚는다. Job당 실제 실행 시간이 $c' < C_s$이면 Job마다 $C_s − c'$만큼 잔여 예산이 늘어 약 $\lceil c' / (C_s − c') \rceil$개의 Job 뒤에 스로틀 없이 끝나고, 그 다음 활성화에서 윈도우가 다시 도달 위상에 맞는다(P-a 상태로 복귀). 도달 간격이 한 번 $T_s$ + 밀린 시간 이상 벌어져도 즉시 복귀한다. Job이 항상 예산을 남김없이 쓰면 복귀하지 못하므로, $C_s$는 실제 실행 시간보다 여유 있게 선언한다(HLD-008 §8). 이 구간에서 마감을 놓친 Job은 `DMISS`에 `THROTTLED`와 `CARRYOVER` 표식이 붙어 실행 결함과 구분된다(D-10). 예산 초과(실행 시간 > $C_s$)로 스로틀된 경우도 같다. 어느 경우에도 다른 서버와 L1에 대한 보장((P-b), (P-c))은 유지된다.

**격자 정렬 보충을 기각한 이유(반례)**: $T_s$ = 18 ms, $C_s$ = 2 ms, $D$ = 5 ms인 서버와, 같은 CPU에 [16, 18) ms와 [36, 38) ms를 점유하는 TT 블록이 있다고 하자. 격자 보충 시각은 0, 18, 36 ms다.

```text
t=16.0  Event A 도달 → TT 블록 때문에 18.0까지 대기
t=18.0  격자 보충(remaining = 2 ms). A 실행 18.0~20.0 → [18, 36) 주기의 예산을 모두 사용
t=34.0  Event B 도달(A로부터 18 ms: MIT 준수). remaining = 0 → Throttled
t=36.0  격자 보충. 그러나 TT 블록 [36, 38)
t=38.0  B 실행 38.0~40.0 → 응답 시간 6 ms > D = 5 ms (DMISS)
```

이 구성은 승인식(D-09)을 통과한다(2 + 2 + 0.05 = 4.05 ≤ 5 ms). 즉 격자 정렬에서는 선언을 지키는 태스크가 스로틀되어 승인식이 보장한 마감을 놓친다. 활성화 기준 윈도우에서는 A의 윈도우가 [16, 34)이므로 B는 34.0에 온전한 예산을 받아 36.0에 끝난다(응답 시간 2 ms).

**대안**
| 대안 | 판정 |
|:--|:--|
| 주기 격자 정렬 지연 보충(현행 `cbs_lazy_replenish`) | 위 반례로 기각. BPF 변경은 가장 작지만 건전하지 않다 |
| 소비 조각마다 보충 시각을 기록하는 완전한 sporadic server | 서버당 보충 목록이 필요해 BPF가 복잡해진다. 단일 스레드 서버에는 과하다. 기각 |
| 마감 기반 CBS(예산 소진 시 서버 마감 연기, EDF 순서) | D-08 EDF와 함께 P2에서 검토 |
| 승격한 윈도우를 승격 시각이 아니라 그 Job의 활성화 시각에 맞추기(`replenish_at = t_act + T_s`) | 위반 직후의 Job부터 도달 위상에 다시 맞아 복귀가 빠르다. 그러나 위반을 반복하는 서버가 $T_s$마다 $2C_s$를 얻는다(윈도우당 상한 (P-b)가 깨짐). 다른 서버의 마감 보장((P-c))은 유지되므로 P2에서 측정 후 재검토(F-9) |

**결과**: `replenish_at`이 격자가 아니라 서버별 활성화에 따라 움직인다. Replenisher는 서버별 시각을 BPF에서 읽어 따라가야 한다(D-07). DDR-007 §4.5의 "주기 격자" 서술은 정합 갱신 대상이다(F-2). REQ-008 OI-4는 본 결정으로 종결한다.

---

### D-05. MIT 위반 처리: 감지·계수 + "잔여 예산 내 흡수" — P1

**결정**: MIT는 Event source의 **도달** 제약이므로(REQ-008 §2.3) 위반 계수는 도달을 관측할 수 있는 어댑터를 우선한다.
- **도달 기준(어댑터 M, 우선)**: `ttsched_evt_notify`가 `t_arr − last_arr_ns < MIT`일 때 SHM 슬롯의 `mit_viol_count`를 증가시킨다(D-03). 처리 중 도달하여 wakeup을 만들지 않는 Event도 계수된다.
- **활성화 기준(어댑터 K, 근사)**: BPF가 활성화 시 `t_act + tol < last_act_ns + MIT`이면(`tol`은 D-04 규칙 5와 같은 값: 깨움 지연의 변동으로 MIT를 지키는 도달이 위반으로 관측되지 않게 한다) `MIT_VIOLATION` 이벤트를 발생시키고 `TaskMeta.mit_viol_count`를 증가시킨다. 마커를 쓰지 않는 source의 유일한 원천이다.
- TIMPANI-N은 source별로 **한 원천만** 사용해 보고한다(D-10). 슬롯의 `event_seq`가 바인딩 이후 증가한 source는 **마커 활성**으로 보고 그때부터 M을 쓴다. M의 기준값은 source의 슬롯을 **활성화하기 직전**의 `mit_viol_count`로 잡으므로 첫 통지 이후의 도달 기준 위반은 빠짐없이 계수된다. 마커 활성 source에 대한 BPF `MIT_VIOLATION` 이벤트는 폴트로 전달하지 않는다. 수집 주기마다 폴트 ringbuf를 먼저 꺼내 두고, 그다음 SHM 슬롯을 읽고, 그 뒤에 꺼내 둔 이벤트를 처리한다: 통지로 생긴 활성화의 K 이벤트는 그 통지의 `event_seq` 증가보다 늦게 나오므로, 처리 시점에는 그 source가 이미 마커 활성으로 표시되어 있다. 그래서 처음부터 통지를 쓰는 source(검증 대상인 경우)에서는 빠지거나 두 번 세는 위반이 없다. 표준 깨움만 쓰다가 도중에 통지를 시작한 source는, 전환을 알아챈 수집 주기에 함께 꺼낸 K 이벤트(전환 직전 최대 100 ms)가 버려져 그만큼 계수에서 빠질 수 있다. 그 이전에 처리한 K 집계는 그대로 둔다.
- **집행 정책(공통)**: 윈도우가 끝나기 전의 활성화는 **새 예산을 받지 않는다**(D-04 규칙 1). 잔여 예산이 있으면 실행하고, 다 쓰면 윈도우 끝까지 Throttled가 되었다가 다음 윈도우의 예산으로 실행된다(규칙 3). 그 뒤의 동작은 D-04 "위반 후 동작"과 같다. 집행은 MIT 위반 여부가 아니라 윈도우 상태만 본다 — 그래서 어댑터와 무관하게 같다.
- **DMISS의 MIT 연관 표식(REQ-008 PRT-007)**: TIMPANI-N은 해당 source에서 마지막으로 관측된 MIT 위반 시각(K: BPF 이벤트 시각, M: 슬롯의 `last_viol_ns`)이 마감을 놓친 Job의 `t_act − T_s` 이후이면 표식을 붙인다. BPF가 활성화 기준으로 Job에 세우는 `flag.mit_violation`은 Job 레코드에 남는다. 별도로, 새 예산을 받지 못하고 시작한 Job에는 BPF가 `CARRYOVER` 표식을 세우고 TIMPANI-N은 이를 `DMISS` 보고에 그대로 싣는다(PRT-007 (b)).

**대안**: (i) 보충 시각까지 지연(enqueue를 THROTTLED로 강제) — 정상 잔여 예산을 못 쓰게 해 지연만 증가, 기각. (ii) 폐기(drop) — 커널 스케줄러가 job을 "버릴" 수 없음(태스크는 이미 깨어 있음), 의미 없음, 기각.

---

### D-06. 배정 CPU 고정: CPU별 L2 DSQ — P1

**결정**: 전역 `DSQ_CBS(101)`/`DSQ_THROTTLED(102)`를 **CPU별 DSQ**로 바꾼다: `DSQ_CBS_BASE + cpu`, `DSQ_THROTTLED_BASE + cpu`(예: `0x100 + cpu`, `0x200 + cpu`; `init()`에서 isolated 마스크의 CPU마다 생성). `ops.enqueue`는 `meta->assigned_cpu`의 L2 DSQ에만 삽입하고, `ops.dispatch(cpu)`는 자기 CPU의 L2 DSQ만 소비한다. `ops.select_cpu`는 항상 `assigned_cpu`를 반환한다. `sched_setaffinity`는 사용하지 않는다(현행 원칙 유지; cpuset으로 허용된 CPU 내에 있어야 함).

**대안**: (i) `select_cpu`만으로 유도(현행) — 전역 DSQ consume으로 타 CPU 실행 가능, 기각. (ii) `sched_setaffinity`로 하드 고정 — 앱 투명성 저해·cgroup cpuset과 충돌 가능, 기각.

**결과**: `promote_throttled_if_replenished`도 CPU별로 순회한다. 스캔 상한 `PROMOTE_BUDGET`(16)은 CPU당 L2 태스크 수 상한 `MAX_L2_PER_CPU`(16, D-09)와 같아 윈도우가 끝난 서버가 스캔에서 빠지지 않는다. `CbsConfig`에 배정 CPU 필드가 필요(D-11).

---

### D-07. 백업 보충(Replenisher) 스레드와 CPU 재스케줄 유발 수단 — P1

**결정**: timpani-n에 `Replenisher` 스레드(SCHED_FIFO, Non-Isolated CPU. 우선순위는 Timer Master − 1을 후보로 두고 PoC에서 확정)를 추가한다. Replenisher는 **서버 상태를 쓰지 않는다.** 하는 일은 Throttled 서버의 윈도우가 끝난 시점에 배정 CPU가 dispatch 경로를 다시 타게 하는 것뿐이고, 예산 갱신과 승격은 BPF가 그 CPU의 런큐 잠금 아래에서 한다(D-04 규칙 3, 6). 이렇게 하면 사용자 공간의 쓰기가 다른 CPU에서 진행 중인 예산 차감을 덮어쓰는 갱신 유실이 생기지 않는다.

**동작**: 서버별 "다음 확인 시각"을 min-heap으로 관리하고 `CLOCK_MONOTONIC` 절대 시각까지 대기한다. 깨어나면
1. BPF `BPF_PROG_TYPE_SYSCALL` 프로그램 `timpani_replenish_kick`을 `bpf_prog_test_run_opts()`로 실행한다(인자: 서버의 `cbs_map` 키와 배정 CPU). 프로그램은 `cbs_map`을 **읽어** 서버가 Throttled이고 `now ≥ replenish_at`이며 그 CPU의 TT 슬롯이 활성이 아닐 때만 `scx_bpf_kick_cpu(cpu, 0)`을 호출하고, 서버의 `(throttled, remaining, replenish_at)`을 돌려준다. TT 슬롯이 활성이면 kick하지 않는다. 여기서 "활성"은 현행 `tt_active_on_cpu`의 판정, 곧 그 CPU에 실행 가능한(runnable) TT 태스크가 있다는 뜻이다(시간표상의 슬롯 구간이 아니다). 그 동안에는 dispatch가 L2를 내보내지 않는다. TT 태스크가 잠들면 CPU가 다음 태스크를 고르면서 dispatch가 호출되고, 그때 승격이 일어난다.
2. 다음 확인 시각: kick 프로그램을 실행하지 못했거나(일시적 오류), 서버가 아직 Throttled이고 윈도우가 이미 끝났으면(승격이 아직 일어나지 않음) `now + 1 ms`(`REPLENISH_RETRY`), 돌려받은 `replenish_at`이 미래이면 그 시각, 그 밖에는 `now + T_s`.

`replenish_at`은 항상 "갱신 시점 + $T_s$"로만 바뀌므로(D-04 규칙 1, 3), 열려 있는 윈도우의 끝보다 늦게 확인하는 일은 없다. 윈도우가 끝났는데 승격되지 않은 서버는 승격될 때까지 1 ms마다 다시 확인한다. 불필요한 실행은 무해하다(프로그램은 조건이 맞을 때만 kick하고 상태를 바꾸지 않는다).

**재스케줄 유발 수단(우선순위)**
1. 위 `timpani_replenish_kick`. `SCX_KICK_IDLE`이 아니라 플래그 0을 쓴다: 대상 CPU가 유휴로 들어가는 도중이면 `SCX_KICK_IDLE`은 kick을 생략할 수 있다. 플래그 0은 실행 중인 태스크를 선점하지 않고 재스케줄만 요청한다. sched_ext의 kfunc 집합이 SYSCALL 프로그램 타입에서 호출 가능하다는 전제이며 **P1 PoC로 확인**한다(OI-9). PoC는 (a) 호출 가능 여부, (b) 유휴 CPU에서 kick → 승격 지연(TMG-003), (c) L2 실행 중인 CPU에 대한 kick의 비용과, kick 직후 TT 슬롯이 시작되는 경계 조건에서의 TT 지터 영향, (d) 예산 차감과 동시에 실행해도 `remaining`이 유실되지 않음을 확인한다.
2. 대체: CPU별 보조 "kicker" 스레드(SCHED_FIFO, 해당 Isolated CPU 고정)를 futex로 깨워 즉시 재대기시킴 → 해당 CPU에서 `schedule()` → `ops.dispatch` 재진입 → 승격. Isolated CPU에 비-SCX RT 스레드가 짧게 실행되는 부작용(수 µs)이 있다.

**대안**: `bpf_timer` — DDR-007 §4.5.1에서 PREEMPT_RT 사유로 기각된 결정 유지.

---

### D-08. 동일 CPU 다중 L2 서버 순서: P1 FIFO, P2 EDF

**결정**: P1은 CPU별 L2 DSQ를 **활성화 시각 순서(FIFO)**로 소비한다. 구현은 DSQ를 우선순위 큐로 쓰고 키를 다음과 같이 준다(`scx_bpf_dsq_insert_vtime`).
- 새 Job, 그리고 선점·슬라이스 만료 후 재삽입되는 Job: `vtime = t_act`(자신의 활성화 순위를 유지한다).
- 스로틀 후 승격되는 Job: `vtime = 승격 시각`(큐의 뒤로 간다. 예산을 초과한 서버가 먼저 활성화되었다는 이유로 다른 서버의 Job을 다시 앞지르지 못한다).

P2에서 키를 절대 마감으로 바꿔 **EDF**로 전환하고, 승인식을 윈도우 수요·공급 함수식으로 교체한다.

**근거**: DSQ의 단순 삽입 순서는 활성화 순서와 다르다(TT 선점 후 재삽입된 Job이 나중에 활성화된 Job 뒤로 밀린다). 활성화 시각을 키로 써야 D-09의 "먼저 활성화된 다른 서버만 앞선다"는 전제가 성립한다. 이 순서와 D-04의 성질 (P-c)가 합쳐져 다른 서버의 간섭 상한이 서버당 $C_j$가 된다. EDF는 수용률을 높이지만 승인식·BPF 변경이 크다.

---

### D-09. 승인 충분조건(P1): 이용률 + FIFO 블로킹 합

**결정**: TIMPANI-O는 CPU $k$에 대해 다음을 모두 검증한다.

```text
(1) 이용률:     U_tt(k) ≤ U_bound  AND  U_tt(k) + Σ_j C_j/T_j + U_overhead ≤ U_bound
(2) 최소 Gap:   min_gap(k) ≥ CBS_MIN_EXEC_US            (경고 → 거부로 승격)
(3) 마감(FIFO): ∀ L2 task i on k:  C_i + B_tt(k, D_i) + L_wake_max + Σ_{j≠i} C_j ≤ D_i
     B_tt(k, L)  = CPU k에서 길이 L인 임의 윈도우 내 TT 점유 시간의 최댓값(인접·연속 슬롯 병합 후, hyperperiod 경계 wrap 포함).
                   윈도우가 여러 TT 블록을 덮으면 그 합이 된다
     L_wake_max  = 설정 상수(기본 50 µs, REQ-008 TIM-001 후보값)
(4) 파라미터:   0 < C_i ≤ D_i ≤ T_i(MIT),  T_i > 0,  task_id ≤ 15바이트이고 노드 안에서 유일
(5) 개수:       CPU k의 L2 태스크 수 ≤ MAX_L2_PER_CPU (16)
```

조건 (3)은 새로 추가되는 태스크뿐 아니라 CPU $k$에 이미 배치된 **모든** L2 태스크에 대해 다시 검사한다(새 태스크의 $C$가 기존 태스크의 간섭 항에 더해지기 때문이다).

조건 (3)의 근거: Job $i$의 응답 시간은 (a) 다른 서버가 $i$보다 앞서 실행하는 양(서버당 $C_j$ 이하 — D-08의 활성화 순서와 D-04 성질 (P-c)), (b) 응답 윈도우 안의 TT 점유($B_{tt}(k, D_i)$ 이하), (c) 깨움 지연($L_{wake}^{max}$ 이하), (d) 자신의 실행($C_i$ 이하, 선언을 지키면 스로틀되지 않음 — D-04 성질 (P-a))의 합을 넘지 않는다. $B_{tt}$는 어디서나 "길이 $L$ 윈도우 내 TT 점유의 최댓값" 하나로 정의하며 구현(`TtBlocking::occupancy_within_us`)도 같은 값을 계산한다. P2는 $sbf_k(L)$/$dbf$ 정밀식(EDF)으로 바꾼다.

**선행 수정**: CPU별 vs 전역 hyperperiod 불일치, CBS 전용 노드 `hyperperiod_us = 0`, TT $U_{bound}$ 미검사(REQ-008 §10-16)를 함께 수정한다.

**CPU 선택**: 조건 (1)~(3)을 만족하는 후보 중 **worst-fit**(잔여 이용률 최대) 유지(DDR-007 §3.3).

---

### D-10. 폴트 분류·속도 제한·전달 — P1

**결정**
- BPF `FaultEvent.fault_type`: `DMISS=0`, `BUDGET_EXCEED=1`(유지), **`MIT_VIOLATION=2`** 추가, `WAKEUP_LATENCY_EXCEED=3`, `WATCHDOG=4` 예약. `FaultEvent`에 `job_seq`, `activation_ns`($t_{act}$), `flags`(mit_violation / throttled / early / carryover) 추가. `carryover`는 새 예산을 받지 못하고 시작한 Job을 뜻한다(D-04). `early`는 완료 전에 낸 조기 `DMISS`를 뜻한다(D-02, Job당 최대 1회).
- `BUDGET_EXCEED`는 `remaining_us`가 양수→0으로 **전이하는 시점에만** 발생(에피소드당 1회). 현행 "매 stopping" 발행 제거. FaultMonitor도 에피소드 키(`replenish_at`; 에피소드는 job 경계와 무관하므로 `job_seq`는 키에 포함하지 않음)로 중복을 흡수(이중 방어).
- `MIT_VIOLATION`은 source당 $T_s$ 윈도우에 1건으로 집계해 보고(위반 횟수 포함). 원천은 D-05에 따라 도달 기준(SHM `mit_viol_count` 증분, 어댑터 M) 또는 활성화 기준(BPF 이벤트, 어댑터 K) 중 하나이며 같은 위반을 두 원천에서 중복 계수하지 않는다(D-05의 전환 규칙).
- `DMISS`는 DDR-012 규칙(count > `current_limit` 시 보고) 유지. MIT 연관 표식은 D-05의 규칙으로 붙인다.
- TIMPANI-O `handle_fault_info`는 유형을 **같은 의미의 유형**으로 Pullpiri `FaultType`에 매핑한다(UNKNOWN 축약 제거). P1에 쓰는 유형(DMISS, BUDGET_EXCEED, MIT_VIOLATION)은 두 proto에서 번호도 같다.
- BPF `FaultEvent.fault_type` 번호(DMISS=0, BUDGET_EXCEED=1, MIT_VIOLATION=2, WAKEUP_LATENCY_EXCEED=3, WATCHDOG=4)와 proto `FaultType` 번호(DMISS=1, BUDGET_EXCEED=2, WATCHDOG=3, MIT_VIOLATION=4, WAKEUP_LATENCY_EXCEED=5)는 **다르다**. TIMPANI-N `NodeClient`가 명시적 변환표로 매핑하며 정수 캐스팅을 금지한다(LLD-008 §5.5).
- FaultInfo에 `task_id`, `node_id`, `timestamp_ns` 채움(현행 미설정).

---

### D-11. proto 변경 범위(P1, 필드 추가만)

**결정** (번호는 LLD-008에서 최종 확정; 기존 번호 불변)

| 파일 | 변경 | Phase |
|:--|:--|:-:|
| `schedinfo.proto` `TaskInfo` | `uint32 min_inter_arrival_us = 11` 추가. Sporadic에서 0이면 전환 기간 동안 `period`를 MIT로 해석(경고) | P1 |
| `schedinfo.proto` `FaultType` | `BUDGET_EXCEED = 2`, `MIT_VIOLATION = 4` 추가, `WATCHDOG = 3` 예약(사용은 P2). 같은 유형은 `node_control`과 번호가 같다. `WAKEUP_LATENCY_EXCEED`는 P2에 추가 | P1 |
| `schedinfo.proto` `FaultInfo` | `uint64 timestamp_ns`, `uint32 count`, `uint32 flags` 추가 | P1 |
| `node_control.proto` `CbsConfig` | `uint32 min_inter_arrival_us = 9`, `optional uint32 cpu = 10` 추가(`optional`로 CPU 0과 미지정을 구분. TIMPANI-N은 필드가 없거나 Isolated CPU가 아닌 엔트리를 적용 오류로 처리하며, 파티션 cpuset으로 대체하지 않는다) | P1 |
| `node_control.proto` `LayerConfig` | L2 엔트리를 `model = EVENT_DRIVEN`, `layer_index = 2` 레이어로 분리 발행(메시지 변경 없음, 발행 규칙 변경) | P1 |
| `node_control.proto` `FaultType` | `MIT_VIOLATION = 4` 추가, `WAKEUP_LATENCY_EXCEED = 5` 예약. 기존 `WATCHDOG = 3`은 P2까지 사용하지 않는다 | P1 |
| `node_control.proto` `FaultInfo` | `uint32 count`, `uint32 flags`(MIT_RELATED, THROTTLED, EARLY, MARKER, CARRYOVER), `uint32 job_seq` 추가 | P1 |
| `node_control.proto` `HierarchicalScheduleTable.table_id` | 발행마다 단조 증가 문자열(`"table_<gen>"`) — 메시지 변경 없음 | P1 |
| `schedinfo.proto` `TaskInfo.temporal_class` (태스크 단위), `Response.reason/message` | — | P2 |
| `node_control.proto` `NodeStatus` L2 통계 | — | P2 |

`CbsConfig` 명칭은 유지한다(OI-13 종결: 신규 메시지는 승인 범위를 넓힘).

---

### D-12. 지연 식별(Late binding): TaskRegistry 부활 + 주기 재스캔 — P1

**결정**: 사장된 `TaskRegistry`를 식별 모듈로 복원한다. 테이블 적용 시 미해결 L2/TT 태스크 이름을 보관하고, 500 ms 주기(설정 가능)로 `/proc/*/comm` 및 `/proc/*/task/*/comm`을 재스캔하여 발견 즉시 BPF 등록·`SCHED_EXT` 적용·SHM 슬롯 바인딩을 수행한다(`T_bind` ≤ 1 s). TID 소멸(`/proc/<pid>/task/<tid>` 부재 또는 BPF `exit_task` 콜백)을 감지하면 상태를 정리하고 미해결 목록으로 되돌린다.

**대안**: (i) `tp_btf/task_newtask` + `sched_process_comm`(comm 변경) 트레이스포인트로 즉시 바인딩 — 지연 ≪ 1 s이나 BPF/ringbuf 경로 추가, P2. (ii) cgroup 이벤트(`inotify` on `cgroup.procs`) + Pullpiri 메타데이터 — REQ-NFR-004의 최종 형태, P2.

---

### D-13. TT floor(이중 모드)·Watchdog: P1 미채택

**결정**: DDR-007 §0의 결정(이중 모드 미채택)을 유지한다. 이벤트 침묵 감시(`WATCHDOG`)는 P2에서 **사용자 공간 EventMonitor**(FaultMonitor 확장, `FaultPolicy.watchdog_period_us` 또는 `max_inter_arrival_us` 신규 필드)로 구현한다. DDR-002 §7은 "DDR-007/008에 의해 대체됨"으로 정합 갱신한다.

---

### D-14. DDR-005 사용자 앱 인터페이스 정합

**결정**: DDR-005 "ttsched_wait_next_period()는 L1/L2 모두 필수"를 **"L1 필수, L2 선택(`ttsched_evt_wait` 권장)"**으로 개정한다. L2 식별 제약(`pthread_setname_np` = `task_id`)은 유지한다.

---

### D-15. 구현 대상: C++ 경로(timpani-o/timpani-n C++ + BPF) — P1

**결정**: P1은 `timpani-o/`(C++), `timpani-n/`(C++ + BPF), `sample-apps/src/libttsched`를 대상으로 구현한다. `timpani_rust/`는 CBS·TemporalClass·NodeStream이 부재하고 timpani-n Rust는 런타임이 없으므로(REQ-008 §10 기반 분석) P1 대상이 아니다(단, Rust `schedinfo.proto` 정의 파일에는 신규 필드만 동기화하며 Rust 코드 변경은 없다). Rust 포트에는 본 DDR의 모델(상태 머신, 승인식, proto)을 그대로 이식하는 후속 작업을 남긴다.

**결과**: `doc/docs/structure.md`의 "Rust만 활발히 개발" 서술과 상충 → 해당 문서 갱신 필요. C++ 경로는 CI 미포함이므로 로컬 GTest·통합 스크립트로 검증(SRS-008 NFR-007).

---

### D-16. Job 트레이스 채널: 전용 BPF ringbuf — P1

**결정**: `fault_ringbuf`와 별도의 `job_trace_rb`(BPF_MAP_TYPE_RINGBUF, 256 KiB)에 완료 시 job 레코드를 발행한다. 필드와 순서는 LLD-008 §4.1의 `struct JobRecord`가 정본이다(`workload_id_hash, task_id_hash, job_seq, cpu, t_act, t_start, t_done, budget_used_us, flags`). 레코드는 **커널이 관측한 Job 구간**(어댑터 K)이며 REQ-008 §2.4의 P1 적합 조건에서 모델의 Job과 일치한다. 전역 `trace_enabled` 변수(rodata 아님, 런타임 토글)로 게이트한다. 예외: 태스크 종료 레코드(`ops.exit_task`)는 TaskRegistry 정리 신호이므로 게이트 없이 항상 발행한다(종료는 드물어 비용 무시 가능). timpani-n의 `JobTraceCollector`가 소비하여 (a) 태스크별 카운터·히스토그램 갱신, (b) 검증 프레임워크용 파일/소켓 출력(gpdata 형식 확장)을 수행한다. 어댑터 M의 Event source 통계(SHM의 누적 카운터 `event_seq/consumed_seq/mit_viol_count`와 최근 시각 `last_arr_ns`)는 `EventSourceMonitor`가 주기 스냅샷으로 같은 채널에 출력한다. Job 레코드에는 `n_events`와 Event별 정보가 없다(비동기 수집에서는 Job과 Event를 실시간으로 맞출 때 경합이 불가피하다). Event가 유실 없이 소비되었는지는 source의 누적 도달 수와 누적 소비 수(`consumed_seq`)의 차로 확인하고, Event↔Job 대응과 $t_{rel}$ 기준 응답 시간은 공통 단조 시계를 쓰는 두 레코드와 생성기 로그의 **오프라인 대조**로 산출한다. 대조 규칙: REQ-008 §2.4의 P1 적합 조건(MIT 준수, $R \le D$, mid-job blocking 없음)이 성립하는 구간에서는 I1에 의해 k번째 Job 레코드가 생성기 로그의 k번째 도달과 짝이고 $t_{rel}$은 그 도달의 $t_{arr}$이다. 조건이 깨진 구간(`MIT_VIOLATION`/`CARRYOVER`/`DMISS` 표식, 또는 `pending > 0` 스냅샷으로 식별)은 Job 단위 대응을 정의하지 않고 source 단위 계수(도달 수 = 소비 수)만 확인한다. P1의 실시간 판정(DMISS, 깨움 지연)은 $t_{act}$ 기준이다. Timer Master 지터 샘플도 동일 채널 형식으로 출력한다.

**대안**: `fault_ringbuf` 재사용 — 폴트와 트레이스의 대역폭·우선순위 혼합, 기각. 커널 tracepoint(`bpf_trace_printk`/`trace_pipe`) — 비결정적·파싱 비용, 기각.

---

### D-17. 런타임 갱신(P1): 전체 테이블 재발행 유지, L2 재등록 허용

**결정**: P1은 현행 전체 테이블 발행·전체 재등록을 유지한다. 단, (a) L2 엔트리가 동일하고 태스크가 이미 바인딩되어 있으면 `cbs_map`과 `task_meta_map`에 **쓰지 않는다**(사용자 공간이 값을 통째로 다시 쓰면 그 사이의 BPF 갱신 — 예산, Job 상태, 카운터 — 을 덮어쓴다). 파라미터가 바뀐 엔트리는 **제거 후 재등록**한다: 태스크를 `SCHED_OTHER`로 되돌려 큐에서 빼고, 맵 엔트리를 지우고, 새 엔트리를 쓴 뒤 `SCHED_EXT`로 넣는다. 새로 등록할 때도 맵 엔트리를 먼저 쓰고 `SCHED_EXT` 전환을 나중에 한다(전환 직후의 깨움이 서버 없이 처리되지 않게). 재등록 중에 진행 중이던 Job의 관측은 끊길 수 있다(P1 허용). 같은 `task_id`의 SHM 슬롯과 그 누적 값(`event_seq`, `consumed_seq`, `mit_viol_count`)은 유지한다(D-03). BPF 쪽 태스크 상태(`job_seq`, 활성화 기준 MIT 횟수, 마지막 활성화 시각)는 등록할 때마다 0에서 다시 시작한다: `job_seq`는 한 번의 등록 구간 안에서만 유일하고, 레코드의 순서는 `t_act`로 정한다, (b) TT 슬롯 집합이 동일하면 Timer Master 재시작을 생략(가능한 범위에서, SHOULD). TT 무교란·epoch 유지·증분 갱신은 P2(DDR-011 개정과 함께).

---

## 3. 결과 (Consequences)

### 3.1 긍정
- Event 정의·Job 경계·폴트 의미론이 명확해져 검증 프레임워크가 합부를 판정할 수 있다.
- L2가 배정 CPU에 고정되어 DDR-007의 CPU별 Gap 분석이 유효해진다.
- 선언을 지키는 태스크는 스로틀되지 않고(D-04), 보충 지연이 바운드되어 "이벤트 없는 CPU에서의 기아"가 사라진다.
- 코어가 미들웨어 중립이 되어 FCW(SOME/IP) 외 DDS/CAN 기반 샘플도 동일 계약으로 지원된다.
- proto는 필드 추가만으로 호환을 유지한다.

### 3.2 부정 / 비용
- `libttsched` SHM ABI 단절(재컴파일). `libtimpani_rt`·`/timpani_rt` 폐기로 PR #50/#52 코드 일부 제거.
- BPF 변경(상태 머신, CPU별 DSQ, 폴트 전이) 및 Replenisher 스레드 추가 → timpani-n 검증 범위 확대, PoC(OI-9) 필요.
- P1 FIFO + 보수적 승인식은 다중 L2 구성에서 수용률이 낮을 수 있다(P2 EDF로 개선).
- mid-job blocking을 하는 앱은 마커를 쓰더라도 P1에서 Job이 분할 측정된다(A-06, 문서화된 한계).
- 활성화 기준 윈도우로 바꾸면서 BPF 보충 경로와 DDR-007 §4.5 서술이 바뀐다.
- MIT를 위반하거나 예산을 초과한 서버는 윈도우가 도달 위상보다 뒤로 밀리고, 이후 Job들의 여유로 갚을 때까지 그 태스크 자신의 응답 시간이 늘어난다(D-04 "위반 후 동작"). 여유 없이 선언한 서버는 복귀하지 못한다.
- L2 `task_id`마다 SHM 슬롯을 데몬 수명 동안 고정 배정하므로, 이름이 자주 바뀌는 워크로드는 슬롯을 소진할 수 있다(64개 중 TT가 쓰지 않는 수만큼).

### 3.3 후속 결정 필요 (P2 또는 OI)
| # | 항목 | 문서 |
|:--|:--|:--|
| F-1 | OI-9: 백업 보충의 재스케줄 유발 수단 PoC(BPF SYSCALL + `scx_bpf_kick_cpu` 가용성) | 본 DDR Accepted 조건 |
| F-2 | DDR-007 §4.5(주기 격자 보충 서술, Replenisher의 맵 갱신)를 D-04/D-07에 맞춰 정합 갱신 | DDR-007 |
| F-3 | D-02 P2 규칙(시스템 콜 분류), D-08 EDF, D-09 정밀식 | DDR-008 개정 |
| F-4 | DDR-002 §7, DDR-005 User App Interface, DDR-011 증분 갱신, DDR-012 비동기 폴트 전달 정합 갱신 | 각 DDR |
| F-5 | `doc/docs/structure.md` Rust 전용 개발 서술 갱신 | docs |
| F-6 | TIM 후보값 확정(RPi5 측정) | 테스트 전략 |
| F-7 | 태스크당 다중 Event source 명시 선언(REQ-008 OI-16), 미들웨어 경계 어댑터 X | DDR-008 개정 |
| F-9 | 위반 후 윈도우 복귀 정책: 승격 윈도우를 활성화 시각에 맞추는 대안(D-04 대안 표)의 측정·재검토 | DDR-008 개정 |
| F-8 | OI-17: 마커 wait point의 커널 식별. 후보 = 라이브러리의 `sched_yield()` + BPF `ops.yield`(마커 사용 플래그가 선 태스크만). PoC 결과에 따라 P1 채택 여부 결정 | DDR-008 개정 |

---

## 4. 요구사항 대응표 (Decision → Requirement)

| 결정 | 충족 REQ-008 | 충족 SRS-008 |
|:--|:--|:--|
| D-01 | MOD-002/006/007/008, IF-005 | FR-G-001/002, FR-L-002/006 |
| D-02 | MOD-002/003, DET-001/002/005 | FR-B-001~005, FR-B-020/021 |
| D-03 | DET-003/004, MOD-007, IF-005 | FR-L-001~005/007/008, IF-L-001~003 |
| D-04 | PRT-001/007, CBS-006, ORD-003 | FR-B-013/014/015/017 |
| D-05 | PRT-004/007 | FR-B-006/017, FR-L-007, FR-N-014 |
| D-06 | CBS-005 | FR-B-010, TMG-010 |
| D-07 | PRT-003, TIM-003 | FR-N-010/011, TMG-003 |
| D-08 | ORD-002 | FR-B-016 |
| D-09 | CBS-001/004, ORD-001/003, MOD-004, A-03 | FR-O-003/010~015 |
| D-10 | PRT-005/006/007, IF-004 | FR-B-022/023, FR-N-012~015, FR-O-020 |
| D-11 | IF-001/003/004, NFR-005 | IF-PO-001/004/005/006, IF-ON-001~004/006 |
| D-12 | DET-006, TIM-006 | FR-N-003/004/007 |
| D-13 | (범위 외 명시) | — |
| D-14 | DET-003 | FR-L-003 |
| D-15 | NFR-003 | NFR-006/007 |
| D-16 | OBS-001/002, TIM-008 | FR-N-020~024/026, IF-V-001~004 |
| D-17 | RTU-001/004 | FR-O-005, FR-N-005/006 |

---

## 5. 영향 받는 컴포넌트

| 컴포넌트 | 변경 요약 (상세: LLD-008) |
|:--|:--|
| `timpani-o/proto/schedinfo.proto`, `node_control.proto` | D-11 필드 추가 |
| `timpani-o/src/schedinfo_service.cpp` | MIT 필드 읽기·전환 해석, 파라미터 검증, 거부 시 복원 |
| `timpani-o/src/global_scheduler.{h,cpp}` | `mit_us` 분리, $B_{tt}$ 계산, D-09 승인식, min gap 거부, hyperperiod 불일치 수정, EVENT_DRIVEN 레이어 발행, CPU 명시 |
| `timpani-o/src/orchestrator_service.cpp`, `fault_client.cpp` | 폴트 유형 보존 매핑, 신규 필드 전달 |
| `timpani-n/src/bpf/maps.h`, `timpani.bpf.c` | TaskMeta 확장(job 상태), CPU별 DSQ, 상태 머신, MIT 검사, 폴트 전이 발행, `job_trace_rb`, `trace_enabled` |
| `timpani-n/src/main.cpp`, `bpf_loader.{h,cpp}` | EVENT_DRIVEN 레이어 처리, CBS 등록에 MIT/CPU, 상태 보존 재등록 |
| `timpani-n/src/task_registry.{h,cpp}` | D-12 부활(미해결 목록, 재스캔, 소멸 감지) |
| `timpani-n/src/replenisher.{h,cpp}` (신규) | D-07 |
| `timpani-n/src/job_trace.{h,cpp}` (신규) | D-16 수집·통계(Job 레코드, source 통계 출력) |
| `timpani-n/src/event_source_monitor.{h,cpp}` (신규) | D-03/D-05: SHM EVT 슬롯 누적 카운터 폴링 → 도달 통계, 도달 기준 MIT 증분 |
| `timpani-n/src/fault_monitor.{h,cpp}` | 에피소드 집계, MIT 윈도우 집계(원천 선택: 도달 기준 우선), 필드 채움 |
| `timpani-n/src/timer_master.{h,cpp}` | SHM v2, L2 Event source 슬롯 할당·`mit_us` 게시·누적 카운터 보존, 지터 샘플 트레이스 출력 |
| `sample-apps/src/libttsched.{h,c}` | SHM v2, `ttsched_evt_open/notify/wait`(도달 통지, 배치 소비, 도달 기준 MIT 계수) |
| `examples/someip/*` | `libtimpani_rt` 축소/폐기, 어댑터화(REQ-008 §12) |
| `scripts/mock_pullpiri.yaml`, `timpani-o/scripts/grpcurl/*.json` | MIT 필드 사용 |
| 문서 | DDR-002 §7, DDR-005, DDR-011, DDR-012, `structure.md` 정합 |

---

## 6. 기존 DDR과의 정합

| DDR | 영향 |
|:--|:--|
| DDR-001 | 없음(L2 = Sporadic+Safety → CBS 그대로). `TaskSpec.min_inter_arrival_us` 설계를 proto에 실현 |
| DDR-002 | §7 이중 모드 → "미채택(DDR-007/008)"로 갱신 |
| DDR-003 | `TaskSpec` MIT 필드·FaultType 확장 반영 |
| DDR-004 | §8.C CBS 예산 계산에 D-09 승인식 추가 |
| DDR-005 | User App Interface: L2 `ttsched_wait_next_period` 필수 → 선택(D-14); TaskMeta 확장; DSQ 레이아웃(CPU별) 갱신 |
| DDR-006 | RT 스레드 표에 Replenisher 추가(DDR-007 §10 예고 사항 실현) |
| DDR-007 | §4.5 보충 의미론을 활성화 기준 윈도우로(D-04), §4.5.3 Backup 필수화(상태는 BPF만 갱신, D-07), §4.2 DSQ 레이아웃 CPU별, §7 Fault 표에 MIT_VIOLATION |
| DDR-011 | P1 영향 없음; P2 증분 갱신 시 개정 |
| DDR-012 | 에피소드 집계·유형 보존; 비동기 전달은 P2 OI |
