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
- 관측 어댑터 K(커널): BPF의 **태스크별 job 상태 머신(WAITING/ACTIVE)**이 Activation/Start/Completion을 관측한다. P1 규칙은 "자발적 수면 = 완료, 그 후 첫 wakeup = 활성화"이며 집행은 항상 이 관측을 쓴다(D-02). 관측 어댑터 M(마커): `libttsched`의 `ttsched_evt_open/notify/wait`(futex·SHM 기반)가 Arrival(`event_seq`, $t_{arr}$, 도달 기준 MIT)과 배치 소비(`n_events`)를 제공한다(D-03). 시스템 콜 분류 기반 정밀 규칙은 P2.
- CBS는 현행 "주기 격자 정렬 지연 보충"을 유지하되 **백업 Replenisher 스레드**를 필수화하여 보충 지연을 바운드한다(D-04, D-07). MIT 위반은 도달 기준(M) 우선·활성화 기준(K) 근사로 계수하고 "잔여 예산 내 흡수"(D-05).
- L2 태스크는 **CPU별 DSQ**로 배정 CPU에 고정하고(D-06), 동일 CPU 서버 순서는 P1 **FIFO**, P2 EDF(D-08).
- 승인은 이용률 + **FIFO 전제 블로킹 합 충분조건**($C_i + B_{tt} + L_{wake} + \sum_{j\ne i} C_j \le D_i$)(D-09).
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
1. **모델을 먼저 고정한다.** REQ-008 §2의 공통 모델을 TIMPANI의 유일한 기준으로 삼는다: Event source(선언된 발생원, 속성 MIT) → Event(`(source, event_seq, t_arr)`) → Job(`(task, job_seq, n_events ≥ 1, t_act, t_start, t_done)`), 생명주기 사건 Arrival/Activation/Start/Completion, 규칙 R1(대기 중 도달 → 활성화) / R2(처리 중 도달 → 미결) / R3(완료 시 미결 있으면 즉시 다음 Job) / R4(Job은 활성화 시점의 미결 Event를 모두 소비). 정상 조건에서 Job:Event = 1:1(I1).
2. **3층으로 구현한다.** (①) 모델: 애플리케이션 계약, 폴트 의미, 관측 출력(Job 레코드, Event source 통계)은 모델 용어로만 정의한다. (②) 관측 어댑터: **K** = BPF 커널 관측(D-02), **M** = `libttsched` 마커(D-03). P2에 미들웨어 경계(X), 선행 태스크 완료(D)를 어댑터로 추가한다. (③) 집행: CBS 예산·CPU 고정·순서·DMISS 판정(D-04~D-10)은 P1에서 어댑터 K가 관측한 사건만 사용한다. 따라서 어떤 어댑터 조합에서도 격리·판정 동작은 같고, M은 도달 정보와 결정적 wait point를 더할 뿐이다.
3. **전송 계층 중립.** 코어(O/N/BPF/`libttsched`)의 proto·API·자료구조는 미들웨어 식별자(서비스/인스턴스/이벤트/토픽 ID)와 미들웨어 헤더를 포함하지 않는다. 미들웨어 수신 → Event 도달 통지의 연결은 **애플리케이션(또는 앱 측 어댑터)**이 표준 대기 메커니즘(K만 사용) 또는 마커 API(M)로 수행한다.
4. **P1 제한.** Event source는 L2 태스크당 1개이며 암묵 선언한다(식별자 = `(workload_id, task_id)`, MIT = `TaskInfo.min_inter_arrival_us`). proto에 Event source 메시지를 추가하지 않는다. 태스크당 다중 source의 명시 선언은 P2(REQ-008 OI-16)이며, 마커 API는 처음부터 **source 핸들**을 인자로 받아 그 확장에서 시그니처가 바뀌지 않게 한다.

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
WAITING --(wakeup: ops.enqueue with SCX_ENQ_WAKEUP, 또는 ops.runnable)--> ACTIVE
          · t_act = now, job_seq++, MIT 검사(last_act_ns), activation 기록 1회
ACTIVE  --(wakeup/선점 후 재개)--> ACTIVE (t_act 불변)
ACTIVE  --(ops.quiescent(SCX_DEQ_SLEEP) AND 블로킹이 wait point로 분류)--> WAITING
          · t_done = now, 응답시간 판정(DMISS), job 레코드 발행
ACTIVE  --(ops.stopping(runnable=true): 선점/슬라이스 만료)--> ACTIVE
          · ops.stopping(runnable=false)는 예산 차감만 수행한다(완료 판정은 quiescent에서)
```

**wait point 분류 규칙**
- **P1**: (a) 마커 wait(D-03)는 wait point. (b) 마커 없는 태스크의 **모든** 자발적 수면(`ops.quiescent`, `SCX_DEQ_SLEEP`)은 wait point. → 구현상 P1 BPF는 (a)/(b)를 구분할 필요가 없다(둘 다 완료). 알려진 한계(mid-job blocking 앱의 job 분할)는 REQ-008 A-06으로 문서화하고 마커 사용을 권고한다.
- **P2**: (b)를 정밀화. 보조 BPF 프로그램(`tp_btf/sys_enter`, 등록 TID 필터)이 블로킹 직전 시스템 콜을 분류하여 `TaskMeta.wait_kind`에 기록: 이벤트 대기 계열(`epoll_wait/epoll_pwait2/poll/ppoll/select/pselect6/recvmsg/recvfrom/recvmmsg/read*`), 마커 futex(`FUTEX_WAIT_BITSET` + 매직 비트셋, D-03), 기타(뮤텍스 futex 등 = mid-job). mid-job으로 분류된 블로킹이 `midjob_block_max_us`(기본 $D$)를 초과하면 다음 wakeup에서 사후적으로 "완료@block_ns + 새 활성화"로 재분류한다.

**모델 대비 관측 한계(REQ-008 §2.4 K)**: (a) mid-job blocking이 Job을 분할한다(P1, A-06). (b) 처리 중 도달(R2)은 보이지 않는다 — 도달 계수는 어댑터 M이 담당한다. (c) 미결 Event로 인한 back-to-back Job(R3)은 수면이 없어 `quiescent`가 호출되지 않으므로 하나의 구간으로 병합된다. 병합 구간은 선행 Job의 $t_{act}$부터 후행 Job의 $t_{done}$까지이므로 DMISS 판정은 실제보다 엄격한 쪽(보수적)이며, I1에 의해 이 상황은 MIT 위반 또는 선행 Job의 마감 초과에서만 발생한다. 정밀 관측은 P2(REQ-008 OI-17; 후보: back-to-back 복귀 시 라이브러리가 `sched_yield()`를 호출하고 BPF `ops.yield`에서 Job 경계를 기록, 또는 P2 시스템 콜 분류).

**현행 대비 변경**: `select_cpu()`에서 매 wakeup마다 `activation_ns`를 덮어쓰는 코드(`timpani.bpf.c:256`)를 제거하고, 상태 머신으로 대체. CBS DMISS 판정을 매 `stopping`에서 **완료 시점**으로 이동(조기 판정은 1회 MAY).

**대안**: (i) 매 wakeup = 활성화(현행) — mid-job 오염, 기각. (ii) 사용자 공간 EventMonitor가 SHM 마커만으로 판정 — BPF 집행과 괴리, 마커 없는 앱 지원 불가, 기각. (iii) uprobe/USDT로 라이브러리 함수에 훅 — 컨테이너별 attach 운영 부담, 1–2 µs/호출, P2 후보로 보류.

---

### D-03. 관측 어댑터 M: `libttsched` 마커 API(futex/SHM 확장) — P1

**결정**: 기존 `libttsched`(SHM `/timpani_ttsched`, 태스크 슬롯 카운터 + futex)를 L2 Event source로 확장하여, 모델의 Arrival과 R2~R4를 애플리케이션 쪽에서 관측·구현하는 어댑터 M을 제공한다.
- **SHM v2 레이아웃**: `TIMPANI_MAX_TASKS` 32→64; 헤더에 `version` 추가, `magic` 값 갱신(ABI 단절 명시). L2 태스크도 슬롯을 할당받으며(현행 TT 전용 해제) 슬롯 하나가 Event source 하나의 상태를 담는다: `kind`(TT/EVT), `counter`(= `event_seq`, futex 워드), `consumed_seq`, `last_arr_ns`, `mit_us`(TIMPANI-N 게시), `mit_viol_count`. 모두 누적 값이라 TIMPANI-N이 주기적으로 읽어도 변화가 유실되지 않는다.
- **`ttsched_evt_t ttsched_evt_open(const char *task_id)`**: Event source 핸들 획득. 프로세스 내 고정 크기 테이블(힙 없음)에 이름과 슬롯 인덱스를 캐시하고, generation이 바뀌면 notify 시점에 재해결한다. source가 아직 등록되지 않았어도 핸들은 발급된다.
- **`int ttsched_evt_notify(ttsched_evt_t src, uint64_t t_arr_ns)`** — **Arrival**: `t_arr_ns`(0이면 `CLOCK_MONOTONIC` now)와 직전 `last_arr_ns`의 차가 `mit_us` 미만이면 `mit_viol_count++`(도달 기준 MIT, D-05); `last_arr_ns` 갱신; `counter`(`event_seq`) `+1`; `FUTEX_WAKE(1)`. 호출자는 동일 호스트의 임의 스레드. 힙 할당 없음, 시스템 콜 1회.
- **`int ttsched_evt_wait(void)`** — **wait point**: `counter == consumed_seq`이면 `FUTEX_WAIT_BITSET(val3 = TTSCHED_EVT_BITSET)`로 블로킹하고(R1), 다르면 블로킹 없이 진행한다(R3). 복귀 시 `n = counter − consumed_seq`를 반환하고 `consumed_seq = counter`로 갱신한다(R4). 따라서 처리 중 도달한 Event(R2)는 `counter`에 누적되어 유실되지 않고 다음 Job의 `n`으로 전달된다. `consumed_seq`를 SHM에 두므로 워커 재초기화·테이블 갱신을 넘어서도 유실·중복이 없다. −1은 재초기화 필요(`ttsched_wait_next_period`와 동일 규약). 매직 비트셋은 P2 시스템 콜 분류(D-02)에서 마커 wait를 식별하는 수단이며, P1 BPF 동작에는 영향 없다.
- 애플리케이션 계약: `n = ttsched_evt_wait()` 후 자신의 큐에서 **정확히 n건**을 처리한다(notify 전에 큐에 넣는다). 정상 조건에서 n = 1이다(I1).
- 도달 정보의 용도(P1): (1) 도달 기준 MIT 위반 계수 → TIMPANI-N이 보고(D-05, D-10), (2) Event source 통계(도달·소비·미결) → 트레이스 채널(D-16), (3) $t_{arr}$ 기록. DMISS·깨움 지연 판정은 어댑터 K의 $t_{act}$ 기준이다(REQ-008 A-04). P2 미들웨어 경계 관측(X)은 같은 슬롯에 도달을 기록하는 또 하나의 notifier가 된다.
- 신뢰 경계: 도달 계수는 앱이 제공한 값이다. 앱이 통지를 누락·조작해도 CBS 집행은 커널 관측만 사용하므로 격리는 깨지지 않으며, 영향은 해당 source의 MIT 통계 정확도에 한정된다.

**대안**: (i) 앱이 pinned BPF 맵을 직접 갱신 — 권한(`CAP_BPF`/pinned path 노출) 문제·컨테이너 마운트 의존, 기각. (ii) 별도 라이브러리 `libttevent` — ABI/SHM 이중화, 기각(오픈 이슈 OI-14는 "libttsched 확장"으로 종결). (iii) eventfd 기반 — fd 전달 필요, futex 대비 지연 큼(DDR-005 표), 기각. (iv) 이름 기반 `signal(task_name, t_arr)` + 반환값 없는 `wait()`(이전 초안) — 처리 중 도달을 표현하지 못해 유실 또는 빈 Job이 생기고, 호출마다 이름 탐색이 필요하며, 다중 source 확장 시 시그니처가 바뀜, 기각.

**OI-15 결정**: P1에서 `ttsched_evt_notify` 호출자는 동일 프로세스 또는 SHM(`0666`) 쓰기 권한을 가진 동일 호스트 프로세스로 한정한다(종결). 권한 축소·크로스 프로세스 정책은 P2.

**결과**: `libttsched.h` 소비자(timpani-n, sample-apps, 외부 TT 앱) 재컴파일 필요(DDR-011 §1.1과 동일한 유형의 ABI 단절). `timpani_rt_shm.h`/`/timpani_rt`는 폐기.

---

### D-04. CBS 보충 의미론: 주기 격자 정렬 지연 보충 유지 + 백업 보충 필수 — P1

**결정**: 현행 `cbs_lazy_replenish`(보충 시각 격자 `replenish_at_ns += k·T_s`, 잔여 = $C_s$로 재설정)를 **P1 의미론**으로 확정한다(사실상 "지연 보충 주기 서버"). 활성화 기준 보충(sporadic-server식)으로의 전환은 P2 OI로 남긴다(REQ-008 OI-4는 P1에 대해 본 결정으로 종결, 후속은 F-2). 단, DDR-007 §4.5.3의 **Replenisher 스레드(Backup)를 필수 구성요소로 격상**한다(D-07).

**근거**: (1) BPF 변경 최소화(P1 단순성). (2) D-08 FIFO 하에서 타 L2에 대한 간섭 상한은 보충 의미론과 무관하게 "서버당 큐잉된 job 1개 = $C_j$"이다(단일 스레드 서버는 동시에 1개 job만 runnable). (3) 임의 $T_s$ 윈도우에서 서버의 소비 상한은 격자 정렬 시 최대 $2C_s$(주기 경계 양쪽)이나, 이는 L1에는 영향이 없고(TT-first) 타 L2에는 FIFO로 차단된다. 따라서 승인식(D-09)이 $C_j$ 항만 쓰는 것이 정당하다.

**대안**: 활성화 기준 보충(remaining 재설정을 `t_act ≥ last_replenish + T_s`일 때만) — 버스트 하에서 윈도우당 $C_s$로 더 엄격하나, `replenish_at` 갱신이 활성화 경로로 이동하고 백업 스레드의 힙 재정렬이 잦아짐. P2에서 측정 후 결정(OI-4).

---

### D-05. MIT 위반 처리: 감지·계수 + "잔여 예산 내 흡수" — P1

**결정**: MIT는 Event source의 **도달** 제약이므로(REQ-008 §2.3) 위반 계수는 도달을 관측할 수 있는 어댑터를 우선한다.
- **도달 기준(어댑터 M, 우선)**: `ttsched_evt_notify`가 `t_arr − last_arr_ns < MIT`일 때 SHM 슬롯의 `mit_viol_count`를 증가시킨다(D-03). 처리 중 도달하여 wakeup을 만들지 않는 Event도 계수된다.
- **활성화 기준(어댑터 K, 근사)**: BPF가 활성화 시 `t_act − last_act_ns < MIT`이면 `MIT_VIOLATION` 이벤트를 발생시키고 `TaskMeta.mit_viol_count`를 증가시킨다. 마커를 쓰지 않는 source의 유일한 원천이다.
- TIMPANI-N은 source별로 **한 원천만** 사용해 보고한다(D-10): 슬롯의 `event_seq`가 바인딩 이후 증가한 source는 M, 그렇지 않으면 K.
- **집행 정책(공통)**: 위반 도달은 **새 예산을 받지 않으며**(격자 보충만 적용), 잔여 예산이 있으면 실행, 없으면 Throttled. BPF는 활성화 기준으로 위반이 관측된 Job에 `flag.mit_violation`을 세워 완료 시 `DMISS`에 연관 표식으로 전달하고, TIMPANI-N은 같은 $T_s$ 윈도우에 도달 기준 위반이 있었으면 표식을 보강한다(PRT-007).

**대안**: (i) 보충 시각까지 지연(enqueue를 THROTTLED로 강제) — 정상 잔여 예산을 못 쓰게 해 지연만 증가, 기각. (ii) 폐기(drop) — 커널 스케줄러가 job을 "버릴" 수 없음(태스크는 이미 깨어 있음), 의미 없음, 기각.

---

### D-06. 배정 CPU 고정: CPU별 L2 DSQ — P1

**결정**: 전역 `DSQ_CBS(101)`/`DSQ_THROTTLED(102)`를 **CPU별 DSQ**로 바꾼다: `DSQ_CBS_BASE + cpu`, `DSQ_THROTTLED_BASE + cpu`(예: `0x100 + cpu`, `0x200 + cpu`; `init()`에서 isolated 마스크의 CPU마다 생성). `ops.enqueue`는 `meta->assigned_cpu`의 L2 DSQ에만 삽입하고, `ops.dispatch(cpu)`는 자기 CPU의 L2 DSQ만 소비한다. `ops.select_cpu`는 항상 `assigned_cpu`를 반환한다. `sched_setaffinity`는 사용하지 않는다(현행 원칙 유지; cpuset으로 허용된 CPU 내에 있어야 함).

**대안**: (i) `select_cpu`만으로 유도(현행) — 전역 DSQ consume으로 타 CPU 실행 가능, 기각. (ii) `sched_setaffinity`로 하드 고정 — 앱 투명성 저해·cgroup cpuset과 충돌 가능, 기각.

**결과**: `promote_throttled_if_replenished`도 CPU별로 순회(`PROMOTE_BUDGET` 유지). `CbsConfig`에 배정 CPU 필드가 필요(D-11).

---

### D-07. 백업 보충(Replenisher) 스레드와 CPU 재스케줄 유발 수단 — P1

**결정**: timpani-n에 `Replenisher` 스레드(SCHED_FIFO, 우선순위 = Timer Master − 1, Non-Isolated 또는 Timer Master와 동일 CPU)를 추가한다. 모든 L2 서버의 `replenish_at_ns`를 min-heap으로 관리하고, `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`으로 기상하여 (1) `cbs_map`의 해당 엔트리에 `cbs_lazy_replenish`와 동일한 멱등 규칙을 적용(사용자 공간 구현), (2) 배정 CPU에 재스케줄을 유발, (3) 다음 보충 시각으로 재삽입한다.

**재스케줄 유발 수단(우선순위)**
1. **BPF `BPF_PROG_TYPE_SYSCALL` 프로그램 `timpani_replenish_kick`**을 `bpf_prog_test_run_opts()`로 실행: 커널 내에서 맵 갱신 + `scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE)`을 원자적으로 수행. sched_ext의 `scx_kfunc_set_any`가 SYSCALL 프로그램 타입에 등록되어 있다는 전제이며 **P1 PoC로 확인**한다(OI-9).
2. 대체: CPU별 보조 "kicker" 스레드(SCHED_FIFO, 해당 Isolated CPU 고정)를 futex로 깨워 즉시 재대기시킴 → 해당 CPU에서 `schedule()` → `ops.dispatch` 재진입 → 승격. Isolated CPU에 비-SCX RT 스레드가 짧게 실행되는 부작용(수 µs)이 있다.

**대안**: `bpf_timer` — DDR-007 §4.5.1에서 PREEMPT_RT 사유로 기각된 결정 유지.

---

### D-08. 동일 CPU 다중 L2 서버 순서: P1 FIFO, P2 EDF

**결정**: P1은 CPU별 L2 DSQ의 **FIFO**(활성화 순서) 소비를 명문화한다. P2에서 `scx_bpf_dsq_insert_vtime(p, dsq, slice, vtime = abs_deadline, flags)`로 **절대 마감 오름차순(EDF)**으로 전환하고, 승인식을 윈도우 수요·공급 함수식으로 교체한다.

**근거**: FIFO는 구현 변경이 없고(현행 consume), 단일 스레드 서버 가정 하에 "먼저 큐잉된 타 서버 job 각 1개"라는 단순·보수적 간섭 상한을 준다(D-09). EDF는 수용률을 높이지만 승인식·BPF 변경이 크다.

---

### D-09. 승인 충분조건(P1): 이용률 + FIFO 블로킹 합

**결정**: TIMPANI-O는 CPU $k$에 대해 다음을 모두 검증한다.

```text
(1) 이용률:     U_tt(k) ≤ U_bound  AND  U_tt(k) + Σ_j C_j/T_j + U_overhead ≤ U_bound
(2) 최소 Gap:   min_gap(k) ≥ CBS_MIN_EXEC_US            (경고 → 거부로 승격)
(3) 마감(FIFO): ∀ L2 task i on k:  C_i + B_tt(k, D_i) + L_wake_max + Σ_{j≠i} C_j ≤ D_i
     B_tt(k, L)  = CPU k에서 길이 L인 임의 윈도우 내 TT 점유 시간의 최댓값(인접·연속 슬롯 병합 후, hyperperiod 경계 wrap 포함).
                   L이 최장 연속 TT 블록 이상이면 최대 연속 TT 점유 길이와 같고, D_i가 TT 주기를 넘으면 여러 TT 점유의 합이 된다
     L_wake_max  = 설정 상수(기본 50 µs, REQ-008 TIM-001 후보값)
(4) 파라미터:   0 < C_i ≤ D_i ≤ T_i(MIT),  T_i > 0
```

조건 (3)의 보수성: FIFO 하에서 job $i$는 (a) 활성화 시점에 이미 큐잉된 타 서버 job(최대 각 1개, $\sum_{j\ne i} C_j$), (b) 실행 중 최대 한 번의 연속 TT 점유($B_{tt}$)에 의한 선점, (c) 깨움 지연만큼 지연된다. TT 점유가 응답 윈도우 내 두 번 이상 나타날 수 있는 경우($D_i$가 TT 주기보다 긴 경우)는 P1에서 $B_{tt}(k, D_i)$ 대신 보수적으로 "윈도우 $D_i$ 내 TT 점유 총합"을 쓰도록 구현(FR-O-012의 P1 범위 확장 허용). P2는 $sbf_k(L)$/$dbf$ 정밀식(EDF).

**선행 수정**: CPU별 vs 전역 hyperperiod 불일치, CBS 전용 노드 `hyperperiod_us = 0`, TT $U_{bound}$ 미검사(REQ-008 §10-16)를 함께 수정한다.

**CPU 선택**: 조건 (1)~(3)을 만족하는 후보 중 **worst-fit**(잔여 이용률 최대) 유지(DDR-007 §3.3).

---

### D-10. 폴트 분류·속도 제한·전달 — P1

**결정**
- BPF `FaultEvent.fault_type`: `DMISS=0`, `BUDGET_EXCEED=1`(유지), **`MIT_VIOLATION=2`** 추가, `WAKEUP_LATENCY_EXCEED=3`, `WATCHDOG=4` 예약. `FaultEvent`에 `job_seq`, `flags`(mit_violation / throttled / early) 추가.
- `BUDGET_EXCEED`는 `remaining_us`가 양수→0으로 **전이하는 시점에만** 발생(에피소드당 1회). 현행 "매 stopping" 발행 제거. FaultMonitor도 에피소드 키(`replenish_at`; 에피소드는 job 경계와 무관하므로 `job_seq`는 키에 포함하지 않음)로 중복을 흡수(이중 방어).
- `MIT_VIOLATION`은 source당 $T_s$ 윈도우에 1건으로 집계해 보고(위반 횟수 포함). 원천은 D-05에 따라 도달 기준(SHM `mit_viol_count` 증분, 어댑터 M) 또는 활성화 기준(BPF 이벤트, 어댑터 K) 중 하나이며 합산하지 않는다.
- `DMISS`는 DDR-012 규칙(count > `current_limit` 시 보고) 유지, 표식(MIT 연관) 동반.
- TIMPANI-O `handle_fault_info`는 유형을 **동일 번호**로 Pullpiri `FaultType`에 매핑(UNKNOWN 축약 제거).
- BPF `FaultEvent.fault_type` 번호(DMISS=0, BUDGET_EXCEED=1, MIT_VIOLATION=2, WAKEUP_LATENCY_EXCEED=3, WATCHDOG=4)와 proto `FaultType` 번호(DMISS=1, BUDGET_EXCEED=2, WATCHDOG=3, MIT_VIOLATION=4, WAKEUP_LATENCY_EXCEED=5)는 **다르다**. TIMPANI-N `NodeClient`가 명시적 변환표로 매핑하며 정수 캐스팅을 금지한다(LLD-008 §5.5).
- FaultInfo에 `task_id`, `node_id`, `timestamp_ns` 채움(현행 미설정).

---

### D-11. proto 변경 범위(P1, 필드 추가만)

**결정** (번호는 LLD-008에서 최종 확정; 기존 번호 불변)

| 파일 | 변경 | Phase |
|:--|:--|:-:|
| `schedinfo.proto` `TaskInfo` | `uint32 min_inter_arrival_us = 11` 추가. Sporadic에서 0이면 전환 기간 동안 `period`를 MIT로 해석(경고) | P1 |
| `schedinfo.proto` `FaultType` | `BUDGET_EXCEED = 2`, `WATCHDOG = 3`, `MIT_VIOLATION = 4` 추가(`node_control`과 동일 번호) | P1 |
| `schedinfo.proto` `FaultInfo` | `uint64 timestamp_ns`, `uint32 count`, `uint32 flags` 추가 | P1 |
| `node_control.proto` `CbsConfig` | `uint32 min_inter_arrival_us = 9`, `uint32 cpu = 10` 추가 | P1 |
| `node_control.proto` `LayerConfig` | L2 엔트리를 `model = EVENT_DRIVEN`, `layer_index = 2` 레이어로 분리 발행(메시지 변경 없음, 발행 규칙 변경) | P1 |
| `node_control.proto` `FaultType` | `MIT_VIOLATION = 4`, `WAKEUP_LATENCY_EXCEED = 5` 예약 | P1 |
| `node_control.proto` `FaultInfo` | `uint32 count`, `uint32 flags`, `uint32 job_seq` 추가 | P1 |
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

**결정**: `fault_ringbuf`와 별도의 `job_trace_rb`(BPF_MAP_TYPE_RINGBUF, 256 KiB)에 완료 시 job 레코드 `{wl_hash, task_hash, job_seq, t_act, t_start, t_done, cpu, budget_used_us, flags}`를 발행한다. 전역 `trace_enabled` 변수(rodata 아님, 런타임 토글)로 게이트한다. 예외: 태스크 종료 레코드(`ops.exit_task`)는 TaskRegistry 정리 신호이므로 게이트 없이 항상 발행한다(종료는 드물어 비용 무시 가능). timpani-n의 `JobTraceCollector`가 소비하여 (a) 태스크별 카운터·히스토그램 갱신, (b) 검증 프레임워크용 파일/소켓 출력(gpdata 형식 확장)을 수행한다. 어댑터 M의 Event source 통계(SHM 누적 카운터 `event_seq/consumed_seq/mit_viol_count/last_arr_ns`)는 `EventSourceMonitor`가 주기 스냅샷으로 같은 채널에 출력한다. Job 레코드에 Event별 정보를 실시간으로 병합하지 않는다: 비동기 수집에서는 경합이 불가피하므로, Event↔Job 대응과 $t_{rel}$ 기준 응답 시간은 공통 단조 시계를 쓰는 두 레코드와 생성기 로그의 **오프라인 대조**로 산출한다. Timer Master 지터 샘플도 동일 채널 형식으로 출력한다.

**대안**: `fault_ringbuf` 재사용 — 폴트와 트레이스의 대역폭·우선순위 혼합, 기각. 커널 tracepoint(`bpf_trace_printk`/`trace_pipe`) — 비결정적·파싱 비용, 기각.

---

### D-17. 런타임 갱신(P1): 전체 테이블 재발행 유지, L2 재등록 허용

**결정**: P1은 현행 전체 테이블 발행·전체 재등록을 유지한다. 단, (a) L2 엔트리가 동일하면 `cbs_map`의 `remaining/replenish_at`를 보존(리셋 금지), (b) TT 슬롯 집합이 동일하면 Timer Master 재시작을 생략(가능한 범위에서, SHOULD). TT 무교란·epoch 유지·증분 갱신은 P2(DDR-011 개정과 함께).

---

## 3. 결과 (Consequences)

### 3.1 긍정
- Event 정의·Job 경계·폴트 의미론이 명확해져 검증 프레임워크가 합부를 판정할 수 있다.
- L2가 배정 CPU에 고정되어 DDR-007의 CPU별 Gap 분석이 유효해진다.
- 보충 지연이 바운드되어 "이벤트 없는 CPU에서의 기아"가 사라진다.
- 코어가 미들웨어 중립이 되어 FCW(SOME/IP) 외 DDS/CAN 기반 샘플도 동일 계약으로 지원된다.
- proto는 필드 추가만으로 호환을 유지한다.

### 3.2 부정 / 비용
- `libttsched` SHM ABI 단절(재컴파일). `libtimpani_rt`·`/timpani_rt` 폐기로 PR #50/#52 코드 일부 제거.
- BPF 변경(상태 머신, CPU별 DSQ, 폴트 전이) 및 Replenisher 스레드 추가 → timpani-n 검증 범위 확대, PoC(OI-9) 필요.
- P1 FIFO + 보수적 승인식은 다중 L2 구성에서 수용률이 낮을 수 있다(P2 EDF로 개선).
- 마커 없는 mid-job blocking 앱은 P1에서 job 분할 측정(문서화된 한계).

### 3.3 후속 결정 필요 (P2 또는 OI)
| # | 항목 | 문서 |
|:--|:--|:--|
| F-1 | OI-9: 백업 보충의 재스케줄 유발 수단 PoC(BPF SYSCALL + `scx_bpf_kick_cpu` 가용성) | 본 DDR Accepted 조건 |
| F-2 | OI-4: 활성화 기준 보충으로의 전환 여부(측정 기반) | DDR-008 개정 |
| F-3 | D-02 P2 규칙(시스템 콜 분류), D-08 EDF, D-09 정밀식 | DDR-008 개정 |
| F-4 | DDR-002 §7, DDR-005 User App Interface, DDR-011 증분 갱신, DDR-012 비동기 폴트 전달 정합 갱신 | 각 DDR |
| F-5 | `doc/docs/structure.md` Rust 전용 개발 서술 갱신 | docs |
| F-6 | TIM 후보값 확정(RPi5 측정) | 테스트 전략 |
| F-7 | 태스크당 다중 Event source 명시 선언(REQ-008 OI-16), back-to-back Job 경계의 커널 관측(OI-17), 미들웨어 경계 어댑터 X | DDR-008 개정 |

---

## 4. 요구사항 대응표 (Decision → Requirement)

| 결정 | 충족 REQ-008 | 충족 SRS-008 |
|:--|:--|:--|
| D-01 | MOD-002/006/007/008, IF-005 | FR-G-001/002, FR-L-002/006 |
| D-02 | MOD-002/003, DET-001/002/005 | FR-B-001~005, FR-B-020/021 |
| D-03 | DET-003/004, MOD-007, IF-005 | FR-L-001~005/007/008, IF-L-001~003 |
| D-04 | PRT-001, CBS-006 | FR-B-013/014/015 |
| D-05 | PRT-004/007 | FR-B-006/017, FR-L-007, FR-N-014 |
| D-06 | CBS-005 | FR-B-010, TMG-010 |
| D-07 | PRT-003, TIM-003 | FR-N-010/011, TMG-003 |
| D-08 | ORD-002/003 | FR-B-016 |
| D-09 | CBS-001/004, ORD-001, MOD-004 | FR-O-003/010~015 |
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
| `scripts/mock_pullpiri.yaml`, `grpcurl/*.json` | MIT 필드 사용 |
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
| DDR-007 | §4.5.3 Backup 필수화, §4.2 DSQ 레이아웃 CPU별, §7 Fault 표에 MIT_VIOLATION |
| DDR-011 | P1 영향 없음; P2 증분 갱신 시 개정 |
| DDR-012 | 에피소드 집계·유형 보존; 비동기 전달은 P2 OI |
