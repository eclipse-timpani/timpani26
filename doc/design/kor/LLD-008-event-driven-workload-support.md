<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-008: Event-Driven Workload Support — 상세 설계 (Low-Level Design)

**날짜:** 2026-11-10
**상태:** Draft (v4 — 공통 Event/Job 모델, 독립 검토·검증 반영, FCW 수정 스케치 확장, Phase 1 범위) — DDR-008(Proposal)의 D-01~D-17 확정을 전제로 함
**작성:** Human + AI
**상위 문서:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), [DDR-008](DDR-008-event-driven-workload-support.md), [HLD-008](HLD-008-event-driven-workload-support.md)
**기준 코드:** `timpani26` HEAD `4201573` (`timpani-o/`, `timpani-n/`, `sample-apps/`, `examples/someip/`)

> 본 문서는 HLD-008의 컴포넌트를 **파일·구조체·함수 수준**으로 전개한다. 코드 블록은 구현 지침용 스케치이며, 식별자·필드 번호는 본 문서를 단일 기준(single source)으로 삼는다. Phase 2 항목은 `[P2]`로 표시하고 훅만 남긴다.
> 코딩 규칙: 기존 저장소 스타일(C++17, `TLOG_*`, SPDX 헤더, 4-space)과 `.github/copilot-instructions.md`(RT 경로에 gRPC/할당 금지, proto 변경 승인) 준수.

---

## 요약 (Korean Summary)

- **proto**: `schedinfo.TaskInfo.min_inter_arrival_us=11`, `FaultType` 3종 추가, `FaultInfo` 3필드 추가; `node_control.CbsConfig.min_inter_arrival_us=9, optional cpu=10`, `FaultType` 2종 추가, `FaultInfo` 3필드 추가. 발행 규칙: L2는 `EVENT_DRIVEN` 레이어(`layer_index=2`).
- **timpani-o**: `ClassifiedTask.mit_us` 분리, `validate_l2_task()`, `compute_tt_blocking()`, `admit_l2_fifo()`(이용률+min-gap 거부+CPU의 모든 L2 태스크에 대한 FIFO 블로킹 합), 전역 hyperperiod 일관화, 2-레이어 `build_table()`, 거부 시 복원, 폴트 유형 보존 매핑.
- **BPF**: `TaskMeta` 확장(job 상태·시퀀스·플래그), CPU별 DSQ(`0x100+cpu`/`0x200+cpu`), 신규 `runnable/quiescent/exit_task` 콜백, **활성화 기준 보충 윈도우**(`cbs_on_activation`/`cbs_try_promote`), 활성화 순서 큐, MIT 검사, `BUDGET_EXCEED` 전이 발행, ns 누산 차감, `job_trace_rb`, `timpani_replenish_kick`(SYSCALL).
- **timpani-n**: `TaskRegistry` 재설계(미해결 목록·재스캔·종료 감지), 신규 `Replenisher`(재스케줄 유발만, `cbs_map` 쓰기 없음), 신규 `JobTraceCollector`, 신규 `EventSourceMonitor`, `FaultMonitor` 집계 확장, `TimerMaster` SHM v2, `main.cpp` 테이블 적용 분기.
- **공통 모델 대응**: REQ-008 §2의 Event source·Event·Job과 생명주기 사건을 코드 요소에 1:1로 대응시킨다(§1.1). 관측 어댑터 K = BPF job FSM, M = `libttsched` + SHM EVT 슬롯 + `EventSourceMonitor`.
- **libttsched v2**: SHM v2 레이아웃(64 슬롯, 슬롯 = Event source 상태 `event_seq/consumed_seq/last_arr_ns/last_viol_ns/mit_us/mit_viol_count`, 대기용 futex 워드 `counter`는 따로 둠, `task_id`별 고정 배정, 슬롯별 `kind`로 유효성 판정), `ttsched_evt_open()`, `ttsched_evt_notify()`(도달 통지·도달 기준 MIT), `ttsched_evt_wait()`(배치 소비 수 반환).
- **샘플**: `sample-apps/src/evttask.c` 신규(전송 계층 중립 L2 생성기), FCW 어댑터 축소.
- 테스트: timpani-o GTest 추가, **timpani-n GTest 타깃 신설**, BPF 상태 머신은 공용 헤더 `job_fsm.h`로 호스트 테스트.

---

## 1. 목적 및 대응 요구사항

| 영역 | SRS-008 ID | 본 문서 절 |
|:--|:--|:--|
| 프로토콜 | IF-PO-001/004/005/006, IF-ON-001~004/006 | §2 |
| TIMPANI-O | FR-O-001/003/005/010~016/020 | §3 |
| BPF | FR-B-001~006/010~017/020~023 | §4 |
| TIMPANI-N | FR-N-002~007/010~015/020~022/024 | §5 |
| 공통 모델·3층 | FR-G-001/002 | §1.1 |
| 라이브러리 | FR-L-001~008, IF-L-001~003 | §6 |
| 샘플 앱 | FR-G-001, FR-L-006 | §7 |
| 타이밍 | TMG-001~013 | §8 |
| 검증 인터페이스 | IF-V-001~004, FR-N-026 | §5.4, §10 |
| 비기능 | NFR-001(용량), NFR-002(BPF 반복 상한), NFR-003(TT 무회귀), NFR-005(proto 호환) | §4.2, §4.5, §8, §2.3 |
| 테스트 | NFR-006/007, AC-008-* | §10 |

### 1.1 공통 모델 ↔ 구현 요소 대응

| 모델 요소 (REQ-008 §2) | 구현 요소 | 어댑터 | 절 |
|:--|:--|:-:|:--|
| Event source (식별자, MIT, 소비 태스크) | `CbsConfig{task_id, min_inter_arrival_us}` → SHM EVT 슬롯(`name`, `mit_us`) / `TaskMeta.mit_us` | — | §2.2, §5.6, §4.1 |
| Event `(source, event_seq, t_arr)` | 슬롯 `event_seq`의 1 증가, `last_arr_ns` | M | §6 |
| **Arrival** | `ttsched_evt_notify()` | M | §6.2 |
| 미소비(pending) Event (R2) | `event_seq − consumed_seq` | M | §6.2, §5.4 |
| **Activation** (R1) | `ops.runnable`에서 `JOB_WAITING → JOB_ACTIVE`, `activation_ns`, `job_seq++` | K | §4.3, §4.4 |
| **Start** | `ops.running`에서 `t_start_ns` 최초 기록 | K | §4.4 |
| **Completion** | `ops.quiescent(SCX_DEQ_SLEEP)`에서 `JOB_ACTIVE → JOB_WAITING` | K | §4.3, §4.4 |
| back-to-back Job (R3) | `ttsched_evt_wait()`가 수면 없이 복귀. K는 병합 관측(P1), 병합 구간은 예산을 새로 받지 않음 | M / K | §6.2, §4.5 |
| `n_events` (R4) | `ttsched_evt_wait()` 반환값 | M | §6.2 |
| MIT 위반 | 도달 기준: 슬롯 `mit_viol_count`(notify) / 활성화 기준: `FAULT_MIT_VIOLATION`(BPF) | M / K | §6.2, §4.3, §5.5 |
| Job 레코드 | `JobRecord`(`job_trace_rb`) — 커널이 관측한 Job 구간. `n_events`는 없음(source 누적 카운터로 확인) | K | §4.1, §5.4 |
| Event source 통계 | `EventSourceSnapshot`(100 ms 폴링) | M | §5.4 |
| 보충 윈도우(D-04) | `CbsState.replenish_at_ns`(윈도우 끝), `cbs_on_activation()`, `cbs_try_promote()` | K | §4.3, §4.4 |
| 집행(CBS·CPU 고정·순서·DMISS) | `cbs_map`, CPU별 DSQ(`queue_key_ns` 순), `job_fsm_on_quiescent` — K의 사건만 사용 | K | §4 |

---

## 2. 프로토콜 변경 (`timpani-o/proto`)

### 2.1 `schedinfo.proto` (Pullpiri ↔ O)

```proto
message TaskInfo {
  // ... 1..10 unchanged ...
  // L2 (TEMPORAL_SPORADIC) only: minimum inter-arrival time in us.
  // 0 → transition rule: `period` is interpreted as MIT (warning logged).
  uint32 min_inter_arrival_us = 11;
  // [P2] TemporalClass temporal_class = 12;  // per-task override, 0 = inherit
}

enum FaultType {
  UNKNOWN = 0;
  DMISS = 1;
  BUDGET_EXCEED = 2;   // informational: CBS budget exhausted (episode)
  WATCHDOG = 3;        // reserved (P2)
  MIT_VIOLATION = 4;   // arrival interval < min_inter_arrival_us (aggregated per T_s)
}

message FaultInfo {
  // ... 1..5 unchanged ...
  uint64 timestamp_ns = 6;   // CLOCK_REALTIME at detection (node)
  uint32 count = 7;          // aggregated occurrences in this report (MIT window / episode)
  uint32 flags = 8;          // FaultFlags bitmask (see below)
}

// Bitmask carried in FaultInfo.flags (both protos use identical values)
//   0x1 MIT_RELATED  — an MIT violation was observed on the Event source at or after (t_act − T_s) of the DMISS job (PRT-007)
//   0x2 THROTTLED    — the DMISS job was throttled during its response window
//   0x4 EARLY        — detected before completion (runnable past deadline)
//   0x8 MARKER       — the task's Event source is marker-active (arrival info comes from libttsched)
//   0x40 CARRYOVER  — the job started without a fresh budget (its server window had not ended: after-effect of an earlier
//                     MIT violation or budget overrun, PRT-007 (b))
// BPF JobRecord.flags additionally uses 0x10 DMISS and 0x20 TASK_EXIT; these two never appear in FaultInfo.flags.
```

`Response`는 변경하지 않는다(`[P2] reason/message`).

### 2.2 `node_control.proto` (O ↔ N)

```proto
enum FaultType {
  UNKNOWN = 0; DMISS = 1; BUDGET_EXCEED = 2; WATCHDOG = 3;
  MIT_VIOLATION = 4;
  WAKEUP_LATENCY_EXCEED = 5;   // reserved (P2)
}

message FaultInfo {
  // ... 1..9 unchanged ...
  uint32 count = 10;
  uint32 flags = 11;     // same bitmask as schedinfo.FaultInfo.flags
  uint32 job_seq = 12;
}

message CbsConfig {
  // ... 1..8 unchanged (budget_us = C_s, period_us = T_s, deadline_us = D) ...
  uint32 min_inter_arrival_us = 9;  // MIT; 0 → use period_us (transition)
  optional uint32 cpu = 10;         // assigned isolated CPU. `optional` (proto3 field presence) so that CPU 0 differs from "not set"
}
```

**발행 규칙**(메시지 변경 없음): `PartitionConfig.layers = [ {layer_index=1, model=TIME_TRIGGERED, tt_slots=[...], cbs_entries=[]}, {layer_index=2, model=EVENT_DRIVEN, tt_slots=[], cbs_entries=[...]} ]`. 비어 있는 레이어는 생략 가능. `table_id = "table_" + generation`.

### 2.3 하위 호환 규칙
- 모든 변경은 필드 추가. 구 Pullpiri(필드 1~10)·구 timpani-n(필드 1~8)이 보낸/받은 메시지는 그대로 역직렬화된다.
- O는 항상 `cpu`를 채우며 CPU 0도 명시 값이다. timpani-n은 `has_cpu()`가 false이거나 Isolated CPU가 아닌 엔트리를 적용 오류로 처리한다(파티션 cpuset으로 대체하지 않는다: 대체하면 승인 때의 CPU별 검사가 무효가 된다). O와 N은 함께 배포한다. proto3 `optional`은 protoc 3.15 이상이 필요하다.
- `scripts/mock_pullpiri.py`, `scripts/mock_pullpiri.yaml`, `timpani-o/scripts/grpcurl/cbs.json`에 `min_inter_arrival_us` 추가.

---

## 3. TIMPANI-O 상세

### 3.1 `global_scheduler.h` — 자료구조

```cpp
struct ClassifiedTask {
    std::string workload_id;
    std::string task_id;
    Mechanism   mechanism;
    uint32_t    period_us;      // L1 only
    uint32_t    mit_us;         // L2 only (NEW) — replaces the dual use of period_us
    uint32_t    wcet_us;
    uint32_t    deadline_us;
    uint32_t    max_dmiss;
    int         assigned_cpu;
};

struct CbsAllocation {
    // ... existing ...
    uint32_t mit_us;            // NEW → CbsConfig.min_inter_arrival_us
};

enum class InfeasibleReason {
    NonHarmonicPeriod, TtSlotConflict, NoCbsBandwidth, SafetyCbsExceeded,
    NoCbsCpu, UtilizationExceeded,
    NodeUnknown,         // NEW: node_id not registered (FR-O-004 minimum set)
    ParamInvalid,        // NEW: 0<C≤D≤MIT violated, task_id longer than 15 bytes, or task_id not unique on the node
    GapTooSmall,         // NEW: min_gap < CBS_MIN_EXEC_US (was warning)
    DeadlineInfeasible,  // NEW: FIFO blocking condition failed for some L2 task on the CPU
};

struct TtBlocking {          // NEW (per CPU)
    std::vector<std::pair<uint32_t,uint32_t>> merged_us;   // merged TT occupancy [start,end) sorted, one hyperperiod
    uint32_t hp_us = 0;
    uint32_t max_contig_us = 0;                            // longest merged block (wrap-aware); diagnostic only — B_tt(k,L) can be larger when the window covers several blocks
    uint32_t total_us = 0;                                 // sum of merged block lengths in one hyperperiod
    uint32_t occupancy_within_us(uint32_t L) const;        // B_tt(k, L): max TT time in any window of length L (wrap-aware)
    // [P2] sbf_k(L) gap-supply function
};

// Tunables (header constants, overridable by -D at build)
static constexpr double   U_BOUND        = 0.80;
static constexpr double   U_OVERHEAD     = 0.02;
static constexpr uint32_t CBS_MIN_EXEC_US = 100;
static constexpr uint32_t L_WAKE_MAX_US  = 50;     // NEW: REQ-008 TIM-001 candidate
static constexpr uint32_t DEFAULT_HP_US  = 10000;  // CBS-only node hyperperiod
static constexpr size_t   MAX_L2_PER_CPU = 16;     // NEW: equals BPF PROMOTE_BUDGET (§4.4), so a due throttled server is never skipped
static constexpr size_t   MAX_TASK_ID_LEN = 15;    // NEW: Linux comm limit (TASK_COMM_LEN − 1)
```

내부 사유와 SRS-008 FR-O-004 사유 코드의 대응(P1은 로그, P2는 `Response.reason`):

| SRS 사유 코드 | `InfeasibleReason` |
|:--|:--|
| `PARAM_INVALID` | `ParamInvalid` |
| `UTIL_EXCEEDED` | `UtilizationExceeded`, `NoCbsBandwidth`, `SafetyCbsExceeded` |
| `DEADLINE_INFEASIBLE` | `DeadlineInfeasible`, `GapTooSmall` |
| `NO_CPU` | `NoCbsCpu`(CPU당 L2 개수 상한 초과 포함), `TtSlotConflict` |
| `NODE_UNKNOWN` | `NodeUnknown` |
| `NON_HARMONIC` | `NonHarmonicPeriod` |

### 3.2 `schedinfo_service.cpp` — 변환·검증·복원

```cpp
// ConvertToClassifiedTasks(): per task
if (mech == Mechanism::CBS) {
    t.mit_us = ti.min_inter_arrival_us();
    if (t.mit_us == 0 && ti.period() > 0) {            // transition rule (FR-O-001)
        t.mit_us = static_cast<uint32_t>(ti.period());
        TLOG_WARN("task ", ti.name(), ": min_inter_arrival_us=0, using period as MIT (deprecated)");
    }
    t.deadline_us = ti.deadline() > 0 ? ti.deadline() : t.mit_us;
    if (auto err = validate_l2_task(t)) return reject(*err);   // FR-O-003
}

std::optional<InfeasibleError> validate_l2_task(const ClassifiedTask& t) {
    if (t.task_id.empty() || t.task_id.size() > MAX_TASK_ID_LEN)
                                           return err(ParamInvalid, "task_id must be 1..15 bytes (Linux comm limit)");   // REQ-008 A-03
    if (t.mit_us == 0)                     return err(ParamInvalid, "MIT must be > 0");
    if (t.wcet_us == 0)                    return err(ParamInvalid, "WCET must be > 0");
    if (t.wcet_us > t.deadline_us)         return err(ParamInvalid, "WCET > deadline");
    if (t.deadline_us > t.mit_us)          return err(ParamInvalid, "deadline > MIT (arbitrary deadline unsupported in P1)");
    return std::nullopt;
}

// RegenerateAllSchedules(): per node, before scheduling — task_id must be unique on the node (it is the SHM slot / comm key)
//   std::set<std::string> seen; for each task on the node: if (!seen.insert(t.task_id).second) → ParamInvalid "duplicate task_id on node"

// AddSchedInfo(): restore-on-reject (FR-O-005)
auto previous = workload_tasks_.find(id) != end ? std::optional{workload_tasks_[id]} : std::nullopt;
workload_tasks_[id] = tasks;
if (!RegenerateAllSchedules(err)) {
    if (previous) workload_tasks_[id] = *previous; else workload_tasks_.erase(id);
    RegenerateAllSchedules(ignored);         // tables return to last-good state
    TLOG_ERROR("reject ", id, " reason=", to_string(err.reason), " cpu=", err.cpu, " task=", err.task_id, ": ", err.details);   // FR-O-004 P1: structured log; [P2] Response.reason
    return status -1;
}
```

### 3.3 `global_scheduler.cpp` — 파이프라인 변경

```text
generate_schedule(node, tasks):
  1. split TT / CBS (unchanged)
  2. global_hp = TT 있음 ? LCM(all TT periods on node) : DEFAULT_HP_US     // FR-O-015
  3. assign_cpus(): TT round-robin (unchanged); check U_tt(k) ≤ U_BOUND (NEW)
     CBS worst-fit by U_s = wcet/mit over residual (unchanged formula, mit_us)
  4. per CPU k:
       place_tt_slots(k, global_hp)          // repeats = global_hp / period  (was per-CPU hp)
       gaps_k   = compute_gaps(slots_k, global_hp)
       btt_k    = compute_tt_blocking(slots_k, global_hp)              // NEW
       admit_l2_fifo(k, cbs_tasks_k, u_tt_k, gaps_k, btt_k)            // replaces allocate_cbs_budgets
  5. build_table(): layer 1 TT, layer 2 EVENT_DRIVEN; table_id = "table_<gen>"
```

```cpp
TtBlocking GlobalScheduler::compute_tt_blocking(std::vector<TtSlotPlacement> slots, uint32_t hp_us) {
    TtBlocking b; b.hp_us = hp_us;
    if (slots.empty()) return b;
    std::sort(slots.begin(), slots.end(), by_offset);
    for (auto& s : slots) {                                   // merge adjacent/overlapping [offset, offset+duration)
        uint32_t st = s.offset_us, en = s.offset_us + s.duration_us;
        if (!b.merged_us.empty() && st <= b.merged_us.back().second) b.merged_us.back().second = std::max(b.merged_us.back().second, en);
        else b.merged_us.push_back({st, en});
    }
    for (auto& m : b.merged_us) { b.max_contig_us = std::max(b.max_contig_us, m.second - m.first); b.total_us += m.second - m.first; }
    if (b.merged_us.size() > 1 && b.merged_us.back().second == hp_us && b.merged_us.front().first == 0)   // wrap
        b.max_contig_us = std::max(b.max_contig_us,
            (b.merged_us.back().second - b.merged_us.back().first) + (b.merged_us.front().second - b.merged_us.front().first));
    return b;
}

// B_tt(k, L): TT occupancy is a periodic step function, so the maximum over windows of fixed length L is attained
// when the window starts at a block start or ends at a block end. Evaluate both candidate sets over two hyperperiods.
uint32_t TtBlocking::occupancy_within_us(uint32_t L) const {
    if (merged_us.empty() || L == 0) return 0;
    if (L >= hp_us) return total_us * (L / hp_us) + occupancy_within_us(L % hp_us);                 // whole hyperperiods + remainder window
    auto occ = [&](uint64_t ws) {                            // occupancy of [ws, ws+L) over unrolled blocks
        uint64_t we = ws + L, sum = 0;
        for (int rep = 0; rep < 2; ++rep) for (auto& m : merged_us) {
            uint64_t s = m.first + (uint64_t)rep * hp_us, e = m.second + (uint64_t)rep * hp_us;
            if (e > ws && s < we) sum += std::min(e, we) - std::max(s, ws);
        }
        return sum;
    };
    uint64_t best = 0;
    for (auto& m : merged_us) {
        best = std::max(best, occ(m.first));                                        // window starts at block start
        best = std::max(best, occ(m.second >= L ? m.second - L : m.second + hp_us - L));   // window ends at block end
    }
    return static_cast<uint32_t>(best);
}

bool GlobalScheduler::admit_l2_fifo(int cpu, const std::vector<ClassifiedTask>& cbs_tasks, double u_tt,
                                    const std::vector<GapInterval>& gaps, const TtBlocking& btt,
                                    PerCpuSchedule& sched, InfeasibleError& error) {
    if (cbs_tasks.empty()) return true;                                     // TT-only CPU: nothing to admit (its gaps and U_tt are not L2 constraints)
    if (cbs_tasks.size() > MAX_L2_PER_CPU) return fail(NoCbsCpu);           // FR-O-013 / FR-B-015
    // (1) utilization (existing logic, uses mit_us)
    double u_avail = U_BOUND - u_tt - U_OVERHEAD;
    if (u_avail <= 0.0) return fail(NoCbsBandwidth);
    // (2) min gap → reject (was warning)                                     FR-O-014
    if (!gaps.empty() && min_len(gaps) < CBS_MIN_EXEC_US) return fail(GapTooSmall);
    // utilization admission in decreasing U_s order (existing)
    ...
    // (3) FIFO blocking condition for EVERY L2 task on this CPU — tasks already placed are re-checked,
    //     because a new task adds its C to their interference term                FR-O-013 (P1)
    uint32_t sum_c = 0; for (auto& t : cbs_tasks) sum_c += t.wcet_us;
    for (auto& t : cbs_tasks) {
        uint32_t b_tt = btt.occupancy_within_us(t.deadline_us);            // B_tt(k, D_i), D-09 (3)
        uint64_t bound = (uint64_t)t.wcet_us + b_tt + L_WAKE_MAX_US + (sum_c - t.wcet_us);
        if (bound > t.deadline_us) {
            error = InfeasibleError(InfeasibleReason::DeadlineInfeasible, cpu, t.task_id,
                "C+B_tt(D)+Lw+ΣC_j=" + std::to_string(bound) + "us > D=" + std::to_string(t.deadline_us) + "us");
            return false;
        }
    }
    // allocations: budget=wcet, period=mit, mit=mit, deadline, cpu
    return true;
}
```

`assign_cpus()`의 CBS worst-fit 단계에서도 후보 CPU마다 조건 (2)/(3)과 개수 상한(`MAX_L2_PER_CPU`)을 사전 검사하여 통과하는 CPU만 후보로 둔다(FR-O-014). 모든 CPU가 실패하면 `NoCbsCpu`. 조건 (3)의 근거(다른 서버가 앞서 실행하는 양 ≤ $C_j$)는 BPF의 활성화 순서 큐와 보충 윈도우 규칙이다(§4.3, DDR-008 D-04 (P-c)).

```cpp
// build_table(): two layers
LayerConfig* tt = part->add_layers(); tt->set_layer_index(1); tt->set_model(TIME_TRIGGERED);  // tt_slots only
LayerConfig* ed = part->add_layers(); ed->set_layer_index(2); ed->set_model(EVENT_DRIVEN);    // cbs_entries only
for (auto& a : sched.cbs_allocations) {
    auto* c = ed->add_cbs_entries();
    c->set_budget_us(a.budget_us); c->set_period_us(a.period_us); c->set_deadline_us(a.deadline_us);
    c->set_min_inter_arrival_us(a.mit_us); c->set_cpu(a.cpu); c->set_current_limit(a.max_dmiss);
    c->set_workload_id_hash(fnv1a(a.workload_id)); c->set_task_id_hash(fnv1a(a.task_id));
}
table.set_table_id("table_" + std::to_string(++table_generation_));
```

### 3.4 `orchestrator_service.cpp` — 폴트 유형 보존

```cpp
static schedinfo::v1::FaultType map_fault_type(node::v1::FaultType t) {
    switch (t) {
    case node::v1::DMISS:          return schedinfo::v1::DMISS;
    case node::v1::BUDGET_EXCEED:  return schedinfo::v1::BUDGET_EXCEED;
    case node::v1::WATCHDOG:       return schedinfo::v1::WATCHDOG;
    case node::v1::MIT_VIOLATION:  return schedinfo::v1::MIT_VIOLATION;
    default:                       return schedinfo::v1::UNKNOWN;
    }
}
// FaultServiceClient::NotifyFault(...) gains timestamp_ns, count, flags → FaultInfo 6..8
```

### 3.5 timpani-o 테스트 (GTest, `timpani-o/tests`)

| 테스트 | 검증 |
|:--|:--|
| `SchedInfoServiceTest.MitFieldPreferredOverPeriod` | FR-O-001 |
| `SchedInfoServiceTest.LegacyPeriodAsMitWarns` | FR-O-001 전환 규칙 |
| `SchedInfoServiceTest.RejectWcetGtDeadline` / `RejectDeadlineGtMit` / `RejectZeroMit` / `RejectTaskIdLongerThan15` / `RejectDuplicateTaskIdOnNode` | FR-O-003 |
| `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | FR-O-005 |
| `GlobalSchedulerTest.TtBlockingMergesAdjacentSlots` / `TtBlockingWrapAround` / `TtBlockingWindowSpansTwoSlots` (D=15 ms, TT 2 ms @ 10 ms → B_tt=4 ms) | compute_tt_blocking, occupancy_within_us |
| `GlobalSchedulerTest.TtBlockingWindowLongerThanHyperperiod` (D=25 ms, hp=10 ms, TT 2 ms → 2·2 ms + B_tt(5 ms)) | `total_us` 분기 |
| `GlobalSchedulerTest.FifoAdmission_FcwConfigPasses` (C=2000, B_tt=2000, Lw=50, D=5000) | FR-O-013 |
| `GlobalSchedulerTest.FifoAdmission_SecondL2Rejected` (C=2000×2, B_tt=2000, D=5000 → 6050 > 5000) | FR-O-013 |
| `GlobalSchedulerTest.FifoAdmission_ExistingTaskRechecked` (기존 태스크의 조건이 새 태스크 때문에 깨지면 거부) / `MaxL2PerCpuEnforced` | FR-O-013 |
| `GlobalSchedulerTest.MinGapRejects` / `TtOnlyCpuWithSmallGapNotRejected` (L2가 없는 CPU의 50 µs gap) | FR-O-014 |
| `GlobalSchedulerTest.CbsEntryCpuZeroIsExplicit` (`has_cpu()` && `cpu()==0`) | IF-ON-003 |
| `GlobalSchedulerTest.EventDrivenLayerEmitted` / `TtLayerHasNoCbsEntries` | FR-O-016 |
| `GlobalSchedulerTest.CbsOnlyNodeHyperperiodNonZero` / `SlotsRepeatPerGlobalHyperperiod` | FR-O-015 |
| `GlobalSchedulerTest.TtUtilizationBoundEnforced` | FR-O-011 |
| `OrchestratorServiceTest.FaultTypePreserved_{BudgetExceed,MitViolation}` | FR-O-020 |
| `PublicationCoordinatorTest.TableIdMonotonic` | IF-ON-006 |

---

## 4. BPF 스케줄러 상세 (`timpani-n/src/bpf`)

### 4.1 `maps.h` — 구조체 (전체 교체본)

```c
#define SCHED_TYPE_TT 0
#define SCHED_TYPE_CBS 1
#define SCHED_TYPE_BEST_EFFORT 2

/* L2 job state (DDR-008 D-02) */
#define JOB_WAITING 0
#define JOB_ACTIVE  1

/* job_flags / FaultEvent.flags / JobRecord.flags. Bits 1, 2, 3 and 6 have the same meaning as proto FaultInfo.flags
 * (THROTTLED, EARLY, MARKER, CARRYOVER). Bit 0 differs: here it marks THIS job's activation-based MIT violation, while
 * FaultInfo bit 0 is MIT_RELATED, computed by FaultMonitor for a DMISS (PRT-007 rule, §5.5). Bits 4..5 exist only in JobRecord.flags. */
#define CBS_ACT_TOL_US  50u   /* activation tolerance of the replenishment window (= L_wake_max), DDR-008 D-04 rule 5 */
#define PROMOTE_BUDGET  16    /* promotion scan bound = MAX_L2_PER_CPU (timpani-o admission) */
#define JOB_FLAG_MIT_VIOLATION (1u << 0)
#define JOB_FLAG_THROTTLED     (1u << 1)
#define JOB_FLAG_EARLY_DMISS   (1u << 2)
#define JOB_FLAG_MARKER        (1u << 3)   /* set by JobTraceCollector (userspace) when the task's Event source is marker-active (EventSourceMonitor); never set in BPF */
#define JOB_FLAG_DMISS         (1u << 4)
#define JOB_FLAG_TASK_EXIT     (1u << 5)   /* JobRecord only: task exited */
#define JOB_FLAG_CARRYOVER     (1u << 6)   /* job started without a fresh budget (window not ended at activation); forwarded in FaultInfo.flags */

struct TaskMeta {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 assigned_cpu;
    __u32 slot_duration_us;
    __u8  scheduling_type;
    __u8  layer;
    __u8  tt_runnable;
    __u8  job_state;          /* L2 only */
    __u64 activation_ns;      /* TT: last wake (unchanged); L2: t_act of current job */
    __u64 cgroup_id;
    /* ---- 008 additions (L2) ---- */
    __u32 mit_us;
    __u32 job_flags;
    __u32 job_seq;
    __u32 mit_viol_count;
    __u64 t_start_ns;         /* first running after activation; 0 until set */
    __u64 last_act_ns;        /* previous activation (MIT check) */
    __u64 queue_key_ns;       /* L2 DSQ rank: t_act of the current job; promotion time after a throttle (D-08) */
    __u32 budget_used_us;     /* per job, reset on activation */
    __u32 _pad2;
};

struct CbsState {
    __u64 task_id_hash;
    __u32 budget_us;
    __u32 period_us;          /* T_s */
    __u32 remaining_us;
    __u32 deadline_us;
    __u64 exec_start_ns;
    __u64 replenish_at_ns;    /* 008: END of the current replenishment window (D-04); 0 = no window yet. Written by BPF only */
    /* ---- 008 additions ---- */
    __u32 exec_frac_ns;       /* sub-µs carry between stops (0..999) */
    __u32 throttle_episodes;  /* incremented on remaining 1→0 transition */
    __u32 throttled;          /* 1 while the task waits in dsq_throttled (set/cleared in enqueue, cleared at promotion) */
    __u32 _pad;
};

#define FAULT_DMISS 0
#define FAULT_BUDGET_EXCEED 1
#define FAULT_MIT_VIOLATION 2
#define FAULT_WAKEUP_LATENCY_EXCEED 3   /* reserved P2 */
#define FAULT_WATCHDOG 4                /* reserved P2 */

struct FaultEvent {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 cpu;
    __u8  fault_type;
    __u8  _pad[3];
    __u64 expected_deadline_ns;   /* DMISS: abs deadline; BUDGET_EXCEED: replenish_at; MIT: last_act+MIT */
    __u64 actual_completion_ns;   /* DMISS: t_done or now(early); MIT: t_act */
    __u32 job_seq;
    __u32 flags;
    __u64 activation_ns;          /* t_act of the job the event belongs to (DMISS, BUDGET_EXCEED, MIT) */
};

struct JobRecord {               /* job_trace_rb payload (DDR-008 D-16) */
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 job_seq;
    __u32 cpu;
    __u64 t_act_ns;
    __u64 t_start_ns;
    __u64 t_done_ns;
    __u32 budget_used_us;
    __u32 flags;
};

struct ReplenishKickArgs {       /* in/out context of timpani_replenish_kick (SYSCALL prog) */
    __u64 cbs_key;               /* in  */
    __u32 cpu;                   /* in  */
    __u32 remaining_us;          /* out: snapshot */
    __u64 replenish_at_ns;       /* out: snapshot (window end); 0 if the server is unknown */
    __u32 throttled;             /* out: 1 while the task waits in dsq_throttled */
    __u32 _pad;
};
```

> `TaskMeta`/`CbsState` 레이아웃 변경은 `timpani-n` 사용자 공간(같은 헤더)과 함께 빌드되므로 추가 호환 조치는 없다. TT 경로 필드 의미는 불변(NFR-003).
> 용량(NFR-001): `task_meta_map` 4096, `cbs_map` 1024, SHM 64 슬롯(TT와 L2 합계: L2 32개를 쓰면 TT는 32개까지) → 노드당 L2 ≥ 32 충족. CPU당 L2는 8 이상, 상한 `MAX_L2_PER_CPU`(16). CPU별 DSQ는 길이 제한이 없다.
> `cbs_map`의 `remaining_us`/`replenish_at_ns`/`exec_*`는 BPF만 쓴다. 사용자 공간은 엔트리를 새로 만들 때와 파라미터가 바뀔 때만 쓴다(§5.1 (C)).

### 4.2 DSQ 및 맵 추가 (`timpani.bpf.c`)

```c
#define DSQ_CBS_BASE       0x100ULL   /* per-CPU L2 ready queue: DSQ_CBS_BASE + cpu */
#define DSQ_THROTTLED_BASE 0x200ULL   /* per-CPU throttled holding queue */
#define DSQ_BE             103
#define MAX_ISOLATED_CPUS  64

static __always_inline __u64 dsq_cbs(__u32 cpu)       { return DSQ_CBS_BASE + cpu; }
static __always_inline __u64 dsq_throttled(__u32 cpu) { return DSQ_THROTTLED_BASE + cpu; }
static __always_inline bool  cpu_is_isolated(__u32 cpu) { return cpu < 64 && (isolated_cpu_mask & (1ULL << cpu)); }

struct { __uint(type, BPF_MAP_TYPE_RINGBUF); __uint(max_entries, 256 * 1024); } job_trace_rb SEC(".maps");

/* L2 DSQs are used as priority queues: insert only with scx_bpf_dispatch_vtime(); mixing FIFO and PRIQ insertion
 * on one DSQ is rejected by the kernel. Rank of dsq_cbs = TaskMeta.queue_key_ns (t_act; promotion time after a throttle).
 * Rank of dsq_throttled = CbsState.replenish_at_ns, so the promotion scan can stop at the first entry that is not due. */

volatile __u32 trace_enabled = 0;       /* .bss — toggled by JobTraceCollector::set_enabled() */
volatile __u64 fault_drop_count = 0;    /* .bss — ringbuf reserve failures (FR-B-023) */

SEC("struct_ops.s/init")
s32 BPF_PROG(init) {
    for (__u32 cpu = 0; cpu < MAX_ISOLATED_CPUS; cpu++) {
        if (!cpu_is_isolated(cpu)) continue;
        scx_bpf_create_dsq(dsq_cbs(cpu), -1);
        scx_bpf_create_dsq(dsq_throttled(cpu), -1);
    }
    scx_bpf_create_dsq(DSQ_BE, -1);
    return 0;
}
```

### 4.3 공용 헬퍼 — `job_fsm.h` (BPF·호스트 공용, 순수 함수)

상태 머신 규칙을 BPF 커널 코드와 GTest가 **같은 헤더**로 공유한다. 헤더는 맵 접근·kfunc 없이 `TaskMeta`/`CbsState` 포인터와 `now`만 받는다.

```c
/* timpani-n/src/bpf/job_fsm.h — no BPF helpers; compiled into both scx_timpani and timpani-n tests */
struct FsmEvent { __u8 fault_type; __u8 emit_fault; __u8 emit_record; __u8 _pad; __u64 expected_ns; __u64 actual_ns; };

/* returns 1 if this wakeup is an Activation (WAITING → ACTIVE), 0 otherwise */
static __always_inline int job_fsm_on_runnable(struct TaskMeta *m, __u64 now, struct FsmEvent *ev)
{
    ev->emit_fault = 0; ev->emit_record = 0;
    if (m->job_state != JOB_WAITING) return 0;               /* not from a wait point: ignore (FR-B-002) */
    m->job_state = JOB_ACTIVE;
    m->activation_ns = now; m->queue_key_ns = now; m->t_start_ns = 0; m->budget_used_us = 0; m->job_flags = 0; m->job_seq++;
    if (m->last_act_ns && m->mit_us) {
        __u64 mit_ns = (__u64)m->mit_us * 1000ULL, tol_ns = (__u64)CBS_ACT_TOL_US * 1000ULL;
        if (now + tol_ns < m->last_act_ns + mit_ns) {         /* activation-based MIT check with the same tolerance as the window rule
                                                                 (FR-B-006): wake jitter must not turn a conforming arrival into a violation */
            m->job_flags |= JOB_FLAG_MIT_VIOLATION; m->mit_viol_count++;
            ev->emit_fault = 1; ev->fault_type = FAULT_MIT_VIOLATION;
            ev->expected_ns = m->last_act_ns + mit_ns; ev->actual_ns = now;
        }
    }
    m->last_act_ns = now;
    return 1;
}

/* Replenishment window, DDR-008 D-04 rule 1. Call at Activation only. Returns 1 if the budget was refilled. */
static __always_inline int cbs_on_activation(struct CbsState *c, __u64 t_act)
{
    if (t_act + (__u64)CBS_ACT_TOL_US * 1000ULL >= c->replenish_at_ns) {    /* previous window ended (or first activation: 0) */
        c->remaining_us = c->budget_us; c->exec_frac_ns = 0;
        c->replenish_at_ns = t_act + (__u64)c->period_us * 1000ULL;
        return 1;
    }
    return 0;      /* window not ended: MIT-violating activation, job fragment, or after-effect of an earlier violation/overrun.
                      Keep remaining and the window; the caller marks the job CARRYOVER */
}

/* Promotion of a throttled server, D-04 rule 3. Returns 1 if the server may run (budget refilled, new window from now). */
static __always_inline int cbs_try_promote(struct CbsState *c, __u64 now)
{
    if (c->remaining_us != 0) { c->throttled = 0; return 1; }   /* defensive: not throttled */
    if (now < c->replenish_at_ns) return 0;                  /* window not ended yet */
    c->remaining_us = c->budget_us; c->exec_frac_ns = 0; c->throttled = 0;
    c->replenish_at_ns = now + (__u64)c->period_us * 1000ULL;
    return 1;
}
/* No other code refills remaining_us (rule 4): not enqueue, not stopping, and not user space while the task is in SCHED_EXT.
 * The MIT flag is not consulted here: enforcement looks only at the window state (D-05). */

static __always_inline void job_fsm_on_running(struct TaskMeta *m, __u64 now)
{
    if (m->job_state == JOB_ACTIVE && m->t_start_ns == 0) m->t_start_ns = now;   /* FR-B-005 */
}

/* returns µs charged; updates CbsState; sets *throttled_now on 1→0 transition */
static __always_inline __u32 cbs_charge(struct CbsState *c, __u64 now, __u8 *throttled_now)
{
    *throttled_now = 0;
    if (c->exec_start_ns == 0) return 0;
    __u64 total = (now - c->exec_start_ns) + c->exec_frac_ns;
    __u32 us = (__u32)(total / 1000ULL);
    c->exec_frac_ns = (__u32)(total % 1000ULL);               /* FR-B-012: ns carry */
    c->exec_start_ns = 0;
    if (c->remaining_us == 0) return us;
    if (us >= c->remaining_us) { c->remaining_us = 0; c->throttle_episodes++; *throttled_now = 1; }
    else c->remaining_us -= us;
    return us;
}

static __always_inline void job_fsm_on_quiescent(struct TaskMeta *m, const struct CbsState *c, __u64 now, struct FsmEvent *ev)
{
    ev->emit_fault = 0; ev->emit_record = 0;
    if (m->job_state != JOB_ACTIVE) return;
    m->job_state = JOB_WAITING;                                /* P1 rule: any voluntary sleep = completion (marker tasks included) */
    ev->emit_record = 1; ev->actual_ns = now;
    if (c && c->deadline_us) {
        __u64 abs_dl = m->activation_ns + (__u64)c->deadline_us * 1000ULL;
        ev->expected_ns = abs_dl;
        if (now > abs_dl && !(m->job_flags & JOB_FLAG_EARLY_DMISS)) {   /* FR-B-020/021 no duplicate */
            m->job_flags |= JOB_FLAG_DMISS; ev->emit_fault = 1; ev->fault_type = FAULT_DMISS;
        }
    }
}
```

### 4.4 ops 구현

```c
SEC("struct_ops/select_cpu")
s32 BPF_PROG(select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (m && (m->scheduling_type == SCHED_TYPE_TT || m->scheduling_type == SCHED_TYPE_CBS)) {
        if (m->scheduling_type == SCHED_TYPE_TT) m->activation_ns = bpf_ktime_get_ns();   /* TT unchanged */
        if (cpu_is_isolated(m->assigned_cpu)) return (s32)m->assigned_cpu;               /* D-06 */
    }
    return prev_cpu;
}

SEC("struct_ops/runnable")
void BPF_PROG(runnable, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m || m->scheduling_type != SCHED_TYPE_CBS) return;
    struct FsmEvent ev; __u64 now = bpf_ktime_get_ns();
    if (!job_fsm_on_runnable(m, now, &ev)) return;                           /* not an Activation */
    __u64 key = cbs_key_from_meta(m);
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
    if (c && !cbs_on_activation(c, now)) m->job_flags |= JOB_FLAG_CARRYOVER; /* D-04 rule 1; no fresh budget → mark the job (PRT-007 (b)) */
    if (ev.emit_fault) emit_fault(m, ev.fault_type, ev.expected_ns, ev.actual_ns);
}

SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
    if (m->scheduling_type == SCHED_TYPE_TT) { /* unchanged: LOCAL_ON | acpu + PREEMPT kick */ return; }
    if (m->scheduling_type == SCHED_TYPE_CBS) {
        __u32 cpu = m->assigned_cpu;
        if (!cpu_is_isolated(cpu)) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
        __u64 key = cbs_key_from_meta(m);
        struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
        if (!c) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
        __u64 flags = cbs_enqueue_flags(enq_flags);
        /* No refill here (D-04 rule 4). The old cbs_lazy_replenish() call is removed. */
        if (c->remaining_us == 0) {                                          /* throttled: wait for the window end */
            m->job_flags |= JOB_FLAG_THROTTLED; c->throttled = 1;
            scx_bpf_dispatch_vtime(p, dsq_throttled(cpu), SCX_SLICE_DFL, c->replenish_at_ns, flags);
        } else {                                                             /* ready: activation order (D-08, FR-B-016) */
            c->throttled = 0;
            scx_bpf_dispatch_vtime(p, dsq_cbs(cpu), SCX_SLICE_DFL, m->queue_key_ns, flags);
        }
        return;
    }
    scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
}

static __always_inline void promote_throttled_if_replenished(__u32 cpu, __u64 now)
{
    /* bpf_for_each(scx_dsq, p, dsq_throttled(cpu), 0), at most PROMOTE_BUDGET iterations:
     *   c = CbsState of p;  if (!c || !cbs_try_promote(c, now)) break;     // ordered by replenish_at: first not-due entry ends the scan
     *   meta->queue_key_ns = now;                                           // D-08: a promoted job goes to the tail
     *   scx_bpf_dispatch_from_dsq_set_vtime(&it, now);
     *   scx_bpf_dispatch_vtime_from_dsq(&it, p, dsq_cbs(cpu), 0);           // no kick: we are on cpu
     * PROMOTE_BUDGET == MAX_L2_PER_CPU, so every due server on this CPU is promoted in one pass (FR-B-015). */
}

SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct *prev)
{
    __u64 now = bpf_ktime_get_ns();
    if (tt_active_on_cpu((__u32)cpu, now)) return;          /* TT-first (DDR-007) */
    if (!cpu_is_isolated((__u32)cpu)) { scx_bpf_consume(DSQ_BE); return; }   /* fix: DSQ_BE now consumable */
    promote_throttled_if_replenished((__u32)cpu, now);
    if (scx_bpf_consume(dsq_cbs((__u32)cpu))) return;
}

SEC("struct_ops/running")
void BPF_PROG(running, struct task_struct *p)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    if (m->scheduling_type != SCHED_TYPE_CBS) return;                        /* TT path unchanged */
    __u64 now = bpf_ktime_get_ns();
    job_fsm_on_running(m, now);
    __u64 key = cbs_key_from_meta(m);
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
    if (!c) return;
    c->exec_start_ns = now;                                                  /* start of this on-CPU stretch; cbs_charge() consumes and clears it in stopping */
    /* slice cap (unchanged): p->scx.slice = max(1 µs, min(remaining_us, µs until the next TT slot on this CPU)) */
}

SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    __u64 now = bpf_ktime_get_ns();
    if (m->scheduling_type == SCHED_TYPE_CBS) {
        __u64 key = cbs_key_from_meta(m);
        struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
        if (c) {
            __u8 throttled_now;
            m->budget_used_us += cbs_charge(c, now, &throttled_now);
            if (throttled_now) {                                             /* FR-B-013: once per episode */
                /* replenish_at_ns (window end) is left unchanged; dispatch promotes once now >= replenish_at_ns (D-04 rule 3).
                 * If the window has already ended, the next dispatch on this CPU promotes at once. */
                m->job_flags |= JOB_FLAG_THROTTLED;
                emit_fault(m, FAULT_BUDGET_EXCEED, c->replenish_at_ns, now);
            }
            /* early DMISS (MAY, once) */
            if (runnable && m->job_state == JOB_ACTIVE && c->deadline_us &&
                !(m->job_flags & (JOB_FLAG_EARLY_DMISS | JOB_FLAG_DMISS))) {
                __u64 abs_dl = m->activation_ns + (__u64)c->deadline_us * 1000ULL;
                if (now > abs_dl) { m->job_flags |= JOB_FLAG_EARLY_DMISS; emit_fault(m, FAULT_DMISS, abs_dl, now); }
            }
        }
        return;
    }
    /* TT path: unchanged (mark_tt_not_runnable + current_slot deadline check) */
}

SEC("struct_ops/quiescent")
void BPF_PROG(quiescent, struct task_struct *p, u64 deq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m || m->scheduling_type != SCHED_TYPE_CBS) return;
    if (!(deq_flags & SCX_DEQ_SLEEP)) return;                                /* only voluntary sleep (FR-B-003) */
    __u64 key = cbs_key_from_meta(m);
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
    struct FsmEvent ev; __u64 now = bpf_ktime_get_ns();
    job_fsm_on_quiescent(m, c, now, &ev);
    if (ev.emit_fault)  emit_fault(m, FAULT_DMISS, ev.expected_ns, now);
    if (ev.emit_record && trace_enabled) emit_job_record(m, now);
}

SEC("struct_ops/exit_task")
void BPF_PROG(exit_task, struct task_struct *p, struct scx_exit_task_args *args)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    emit_job_record_flags(m, bpf_ktime_get_ns(), JOB_FLAG_TASK_EXIT);     /* always, not gated: TaskRegistry cleanup signal (D-16 exception) */
}

/* userspace-triggered kick (DDR-008 D-07 option 1). Reads the server state, never writes it. */
SEC("syscall")
int timpani_replenish_kick(struct ReplenishKickArgs *args)
{
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &args->cbs_key);
    args->remaining_us = 0; args->replenish_at_ns = 0; args->throttled = 0;
    if (!c) return 0;
    __u64 now = bpf_ktime_get_ns();
    __u64 rep = c->replenish_at_ns; __u32 rem = c->remaining_us; __u32 thr = c->throttled;
    if (thr && now >= rep && !tt_active_on_cpu(args->cpu, now))   /* a task waits in dsq_throttled, its window has ended, and no TT slot is running */
        scx_bpf_kick_cpu((s32)args->cpu, 0);                 /* reschedule request → ops.dispatch → promote. 0, not SCX_KICK_IDLE:
                                                                a CPU that is just going idle must not miss the kick.
                                                                During a TT slot no kick is sent: dispatch would return at the TT gate,
                                                                and the dispatch at the end of the slot promotes anyway */
    args->remaining_us = rem; args->replenish_at_ns = rep; args->throttled = thr;   /* copied back to user space */
    return 0;
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {
    .flags = SCX_OPS_SWITCH_PARTIAL,
    .init = (void*)init, .init_task = (void*)init_task, .exit_task = (void*)exit_task,
    .select_cpu = (void*)select_cpu, .enqueue = (void*)enqueue, .dispatch = (void*)dispatch,
    .runnable = (void*)runnable, .running = (void*)running, .stopping = (void*)stopping,
    .quiescent = (void*)quiescent, .name = "timpani" };
```

`emit_fault()`는 `FaultEvent.activation_ns = m->activation_ns`, `flags = m->job_flags`를 채우며, `bpf_ringbuf_reserve` 실패 시 `fault_drop_count++`(FR-B-023). `emit_job_record()`는 `JobRecord{..., t_act=activation_ns, t_start=t_start_ns, t_done=now, budget_used_us, flags=job_flags}`를 제출한다.

### 4.5 검증기(verifier)·호환 메모
- `ops.runnable`/`ops.quiescent`/`ops.exit_task`는 sched_ext 6.12 struct_ops 멤버로 존재한다. `SCX_DEQ_SLEEP`은 vmlinux.h enum.
- `select_cpu`는 `nr_cpus_allowed == 1`인 태스크에 대해 호출되지 않을 수 있으므로 활성화 기록을 `runnable`로 옮긴 것이 정확성에도 필요하다.
- `promote_throttled_if_replenished` 반복 상한 `PROMOTE_BUDGET=16`(SRS NFR-002). TIMPANI-O가 CPU당 L2 태스크를 16개 이하로 승인하므로(§3.3) 윈도우가 끝난 서버가 스캔에서 빠지지 않는다.
- 예산 상태(`remaining_us`, `replenish_at_ns`)를 쓰는 곳은 `runnable`(활성화), `stopping`(차감), `dispatch`(승격)뿐이며 모두 태스크가 속한 배정 CPU의 런큐 잠금 아래에서 실행된다(L2 태스크는 배정 CPU의 런큐에만 있다는 D-06 전제). `timpani_replenish_kick`은 읽기만 한다.
- kfunc 이름은 커널 6.12 기준이다(`scx_bpf_dispatch*`, `scx_bpf_consume`, `scx_bpf_dispatch_from_dsq_set_vtime`, `scx_bpf_dispatch_vtime_from_dsq`). 6.13 이후 `scx_bpf_dsq_insert*`/`scx_bpf_dsq_move*`로 이름이 바뀌었으므로 `scx/compat.bpf.h`의 호환 매크로를 쓰고, 시그니처는 대상 커널의 `vmlinux.h`로 확인한다.
- `timpani_replenish_kick`의 `scx_bpf_kick_cpu` 호출 가능 여부와 컨텍스트 반환(`ctx_in`으로 다시 복사)은 커널에 의존 → **OI-9 PoC**(항목: DDR-008 D-07). 불가 시 §5.3 `KickBackend::KickerThreads`.
- `job_fsm.h`는 `__always_inline`·고정폭 타입만 사용하며 `bpf_helpers.h` 미포함 → 호스트 컴파일 가능.
- 어댑터 K의 관측 한계(REQ-008 §2.4): 미소비 Event가 있어 `ttsched_evt_wait()`가 수면 없이 복귀하면(R3) `quiescent`가 호출되지 않으므로 back-to-back Job은 하나의 구간으로 병합된다. `job_seq`와 `job` 레코드는 **커널이 관측한 구간**을 세며, 모델의 Job과는 REQ-008 §2.4의 P1 적합 조건(MIT 준수, $R \le D$, mid-job blocking 없음)에서 일치한다. Event 수와의 대조는 `EventSourceSnapshot`으로 한다. 병합 구간의 DMISS 판정은 마감 실패를 놓치지 않지만 후행 Job에 거짓 DMISS를 낼 수 있다. 병합 구간은 예산을 새로 받지 않는다(D-04 규칙 4). 정밀 관측은 P2(L-7).
- 예산 차감의 단위: `running`~`stopping` 사이의 on-CPU 시간을 ns로 누산해 µs로 차감한다. 차감은 활성화부터 wait point에서 잠들 때까지(대기 진입 시스템 콜 경로 포함)의 실행을 모두 포함하며, 차감액이 잔여 예산에 **도달하면**(`us >= remaining_us`) 스로틀한다. 따라서 선언 예산 $C_s$는 Job당 차감 실행 시간의 최댓값보다 커야 한다(같으면 대기 진입 직전에 스로틀될 수 있다, DDR-008 D-04 (P-a)).
- 위반 후 동작: 윈도우가 끝나기 전에 활성화된 Job은 `JOB_FLAG_CARRYOVER`를 갖고 남은 예산으로 실행된다. 서버가 도달 위상으로 돌아오는 과정은 DDR-008 D-04 "위반 후 동작"을 따른다.
- mid-job blocking(A-06 위반)은 마커 태스크에서도 Job을 분할한다. 분할된 조각의 활성화는 직전 윈도우 안이므로 예산은 새로 주지 않는다(`cbs_on_activation`). 마커 없는 source에서는 조각 사이의 간격이 활성화 기준 `MIT_VIOLATION`으로 잘못 관측될 수 있다.

---

## 5. TIMPANI-N 사용자 공간 상세

### 5.1 `main.cpp` — 테이블 적용 분기 (변경 요지)

```cpp
// (A) 레이어 모델 분기
for (const auto& layer : partition.layers()) {
    if (layer.model() == EVENT_DRIVEN || layer.cbs_entries_size() > 0) apply_event_layer(partition, layer);
    if (layer.model() == TIME_TRIGGERED || layer.tt_slots_size() > 0)  apply_tt_layer(partition, layer);   // existing body
}

// (B) apply_event_layer(): per CbsConfig e
if (!e.has_cpu() || !is_isolated_cpu(e.cpu())) {                                   // IF-ON-003: no partition-cpuset fallback
    apply_error("CbsConfig.cpu missing or not an isolated CPU: " + e.task_id()); continue;
}
uint32_t cpu = e.cpu();                                                            // CPU 0 is a valid explicit value
uint32_t mit = e.min_inter_arrival_us() ? e.min_inter_arrival_us() : e.period_us();
evt_tasks.push_back({e.task_id(), e.task_id_hash(), e.workload_id_hash(), mit});   // → TimerMaster SHM slot (kind=EVT, mit_us published for the arrival-based check)
servers.push_back({key(e), cpu, (uint64_t)e.period_us() * 1000ULL, 0});            // → Replenisher (next check = now)
fault_monitor.set_task_params(key(e), e.period_us(), e.current_limit());           // MIT window + threshold
L2Binding b = make_l2_binding(e, cpu, mit);                                        // {key, hashes, cpu, mit, budget, period, deadline}
if (auto t = registry.tracked(e.task_id())) {                                      // already bound by an earlier table
    if (t->binding == b) continue;                                                 // D-17(a): unchanged → write NOTHING. TaskMeta and CbsState are BPF-owned while the task runs
    unbind_l2_task(t->tid, t->binding);                                            // parameters changed → remove, then register again below (job in flight is lost, P1)
    registry.remove_expected(e.task_id());                                         // forget the old (tid, parameters) so a later exit scan cannot bring the old parameters back
}
auto r = registry.resolve_now(e.task_id());                                        // std::optional<TaskRegistry::Resolved>
if (!r) { registry.add_unresolved(e.task_id(), b); continue; }                     // FR-N-003 (not an apply error)
if (bind_l2_task(r->tid, b)) registry.track(e.task_id(), r->tid, b);

// (C) bind_l2_task(tid, b): at apply time or later from the TaskRegistry callback. The task is NOT in SCHED_EXT yet.
CbsState st{}; st.budget_us = b.budget_us; st.period_us = b.period_us; st.deadline_us = b.deadline_us;
st.remaining_us = b.budget_us; st.replenish_at_ns = 0; st.throttled = 0;           // no window yet: the first Activation opens it (D-04 rule 1)
bpf_loader.update_cbs_state(b.key, st);                                            // 1. the server first
// A bind starts the BPF-side task state from zero (job_seq, activation-based MIT count, last_act_ns): (task, job_seq) is unique
// within one binding only, and records are ordered by t_act. The SHM slot counters are not touched (§5.6).
TaskMeta meta{}; meta.scheduling_type = SCHED_TYPE_CBS; meta.assigned_cpu = b.cpu; meta.mit_us = b.mit; meta.job_state = JOB_WAITING; ...
bpf_loader.update_task_meta(tid, meta);                                            // 2. then the task meta
if (!apply_sched_ext(tid)) {                                                       // 3. class switch LAST: a wakeup right after it already finds its server and meta
    bpf_loader.delete_task_meta(tid); bpf_loader.delete_cbs_state(b.key); apply_error(...); return false;
}
return true;

// unbind_l2_task(tid, b): sched_setattr(tid, SCHED_OTHER) FIRST (the kernel takes the task out of the L2 DSQs),
// then delete task_meta_map[tid] and cbs_map[b.key]. Used for: entry removed from the table, STOP, changed parameters, task exit.
// These are the only user-space writes to cbs_map / task_meta_map: always while the task is outside SCHED_EXT.

// (D) after loops
EvtSlotTable evt_slots = timer_master.same_tt_table(slots, hp, epoch)
    ? timer_master.update_evt_slots(evt_tasks)                          // FR-N-006 / D-17(b): L2-only change → EVT slots only, no thread restart
    : timer_master.set_schedule_table(slots, evt_tasks, hp, epoch);
// evt_slots: key → {slot index, base_event_seq, base_mit_viol}. For a source whose slot is being (re)activated, TimerMaster reads the
// two counters right BEFORE it stores kind = EVT (§5.6), so nothing accepted after activation is hidden in the baseline.
replenisher.set_servers(servers);
registry.start_rescan_if_pending();
job_trace.set_task_names(hash → name);                     // for CSV output
event_sources.set_shm(timer_master.shm(), evt_slots);     // adapter M stats: retained sources keep their baselines; newly activated ones start from TimerMaster's values
node_client.send_table_applied(table.table_id(), apply_success, apply_error);
```

제거/STOP 경로(`RecoverySignal`, 테이블 소멸)는 기존 처리에 더해 `replenisher.remove(key)`, `fault_monitor.clear_task(key)`, `job_trace.clear_task(key)`, `event_sources.clear_task(key)`, `registry.remove_expected(name)`, `sched_setattr(tid, SCHED_OTHER)`을 수행한다(FR-N-005).

### 5.2 `task_registry.{h,cpp}` — 재설계

```cpp
class TaskRegistry {
public:
    struct Resolved { pid_t tid; bool is_thread; };
    using OnResolved = std::function<void(const std::string& name, Resolved r)>;
    using OnExited   = std::function<void(const std::string& name, pid_t tid)>;

    explicit TaskRegistry(std::chrono::milliseconds rescan_period = 500ms);
    std::optional<Resolved> resolve_now(const std::string& comm);          // process first, then thread (PR #51 logic moved here)
    using Binder = std::function<bool(pid_t tid, const L2Binding& b)>;     // = bind_l2_task (§5.1 (C)); set once at start-up
    void set_binder(Binder b);
    void add_unresolved(const std::string& comm, const L2Binding& b);      // not found yet: bind it when it appears
    void remove_expected(const std::string& comm);                         // entry left the table: forget it (unresolved or tracked)
    void track(const std::string& comm, pid_t tid, const L2Binding& b);    // bound task: watch for exit. Replaces an existing entry of the same name
    struct Tracked { pid_t tid; L2Binding binding; };
    std::optional<Tracked> tracked(const std::string& comm) const;         // table apply: is this task already bound, and with which parameters
    void notify_exit(uint64_t task_id_hash);                               // from JobTraceCollector (JOB_FLAG_TASK_EXIT record carries the hash, not a tid):
                                                                           // finds the tracked entry whose binding has this task_id_hash
    void start_rescan_if_pending();                                        // table-apply thread: wake the rescan thread if anything is unresolved or tracked
    void set_on_exited(OnExited cb);
    void start(); void stop();                                             // rescan thread (default policy, non-isolated)
private:
    void rescan_loop();      // while unresolved non-empty or tracked non-empty: scan /proc every period
    std::map<std::string, L2Binding> unresolved_;   // name → parameters to bind with
    std::map<std::string, Tracked>   tracked_;      // name → (tid, parameters). Exit detection walks the tids
    Binder binder_;
    std::mutex mu_; std::thread th_; std::atomic<bool> running_;
};
```

`rescan_loop()`: (1) `unresolved_` 각 이름에 대해 `resolve_now()`; 성공 시 `binder_(tid, binding)`을 실행하고 true면 `tracked_`로 이동, (2) `tracked_` 각 TID에 대해 `/proc/<tid>` 존재 확인; 없으면 `on_exited` 호출 후 (name, binding)을 `unresolved_`로 되돌린다(FR-N-004; `notify_exit()`도 같은 경로), (3) 두 집합이 모두 비면 다음 테이블까지 휴면. (name, binding)을 되돌릴 때 같은 이름이 이미 `unresolved_`에 있으면 덮어쓰지 않는다(테이블이 넣은 최신 파라미터가 우선이다). `set_binder(bind_l2_task)`는 시작 시퀀스에서 `start()`보다 먼저 한 번 호출한다. binder와 `on_exited` 콜백은 `mu_`를 **놓은 뒤** 호출한다(잠금 아래에서는 호출 대상의 스냅샷만 만든다). 콜백이 `track()`/`add_unresolved()`로 레지스트리에 다시 들어와도 교착하지 않는다. `T_bind` ≤ 주기 + 스캔 시간(≈ 수 ms) ≤ 1 s.

### 5.3 `replenisher.{h,cpp}` — 신규

```cpp
class Replenisher {
public:
    struct Server { uint64_t cbs_key; uint32_t cpu; uint64_t period_ns; uint64_t next_ns; };   // next_ns = next check time
    enum class KickBackend { BpfSyscallProg, KickerThreads };
    static constexpr uint64_t REPLENISH_RETRY_NS = 1'000'000;   // re-check interval for a server that is due but not promoted yet
    Replenisher(BpfLoader& bpf, KickBackend backend, int fifo_prio = 98);   // priority: default TM−1, to be confirmed by the PoC
    void set_servers(std::vector<Server> servers);   // table-apply thread (non-RT): builds the new heap OUTSIDE mu_, swaps it in under mu_
    void remove(uint64_t cbs_key);                   // under mu_: erase in place (no allocation), re-heapify; marks the in-flight server if it is this one
    void start();
    void stop();                                     // under mu_: running_ = false, broadcast cv_; then join
private:
    struct Snapshot { bool valid; bool throttled; uint32_t remaining_us; uint64_t replenish_at_ns; };
    void loop();                                     // SCHED_FIFO, non-isolated CPU
    Snapshot kick(const Server& s);                  // asks BPF to kick if due; returns the server snapshot. Never writes cbs_map
    pthread_mutex_t mu_;                             // PTHREAD_PRIO_INHERIT: the RT loop never waits behind a preempted normal thread
    pthread_cond_t  cv_;                             // CLOCK_MONOTONIC (pthread_condattr_setclock)
    std::vector<Server> heap_;                       // min-heap by next_ns (std::push_heap/pop_heap). Read and written under mu_ only
    uint64_t epoch_ = 0;                             // bumped by set_servers() only, guarded by mu_
    uint64_t inflight_key_ = 0;                      // the server the loop has popped and is kicking right now (guarded by mu_)
    bool inflight_valid_ = false, inflight_removed_ = false;
    bool running_ = false;                           // guarded by mu_
};

void Replenisher::set_servers(std::vector<Server> servers) {
    uint64_t now = mono_now();
    for (auto& s : servers) s.next_ns = now;                          // check every server once right away, then follow its window
    std::make_heap(servers.begin(), servers.end(), ByNext{});         // allocation and sorting happen here, outside mu_
    pthread_mutex_lock(&mu_);
    heap_.swap(servers); ++epoch_;                                    // short critical section: pointer swap only
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}                                                                     // the old storage is freed here, on the caller's thread

void Replenisher::remove(uint64_t key) {
    pthread_mutex_lock(&mu_);
    heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&](const Server& s){ return s.cbs_key == key; }), heap_.end());   // shrinks: no allocation
    std::make_heap(heap_.begin(), heap_.end(), ByNext{});
    if (inflight_valid_ && inflight_key_ == key) inflight_removed_ = true;   // the loop holds THIS server right now: it must not push it back.
                                                                      // Other servers are unaffected (no epoch bump here)
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}

void Replenisher::loop() {
    pthread_mutex_lock(&mu_);
    while (running_) {
        if (heap_.empty()) { pthread_cond_wait(&cv_, &mu_); continue; }            // no L2 server: sleep; never front() on an empty heap
        Server s = heap_.front(); uint64_t epoch = epoch_;
        if (mono_now() < s.next_ns) {                                              // sleep until the earliest check, the table changes, or stop()
            pthread_cond_timedwait(&cv_, &mu_, &to_ts(s.next_ns));                 // absolute CLOCK_MONOTONIC; no signal is used
            continue;                                                              // re-evaluate from the top: the heap may have been swapped
        }
        std::pop_heap(heap_.begin(), heap_.end(), ByNext{}); heap_.pop_back();
        inflight_key_ = s.cbs_key; inflight_valid_ = true; inflight_removed_ = false;
        pthread_mutex_unlock(&mu_);
        Snapshot st = kick(s);                                                     // BPF decides whether a kick is due (≤ 100 µs target, TMG-003)
        uint64_t now = mono_now();
        if (!st.valid)                                             s.next_ns = now + REPLENISH_RETRY_NS;   // the kick could not be run (transient error): try again soon, not after a whole period
        else if (st.throttled && st.replenish_at_ns <= now)        s.next_ns = now + REPLENISH_RETRY_NS;   // due but not promoted yet (TT slot, IPI in flight): look again soon
        else if (st.replenish_at_ns > now)                         s.next_ns = st.replenish_at_ns;         // follow the open window's end
        else                                                       s.next_ns = now + s.period_ns;          // no open window: look again in T_s
        pthread_mutex_lock(&mu_);
        if (epoch == epoch_ && !inflight_removed_) {                               // drop s only if the whole table was swapped or s itself was removed
            heap_.push_back(s); std::push_heap(heap_.begin(), heap_.end(), ByNext{});   // capacity is reserved
        }
        inflight_valid_ = false;
    }
    pthread_mutex_unlock(&mu_);
}

Replenisher::Snapshot Replenisher::kick(const Server& s) {
    if (backend_ == KickBackend::BpfSyscallProg) {
        ReplenishKickArgs a{}; a.cbs_key = s.cbs_key; a.cpu = s.cpu;
        if (bpf_.run_replenish_kick(&a) != 0) return {false, false, 0, 0};   // bpf_prog_test_run_opts(prog_fd, ctx_in=&a); the kernel copies ctx back
        return {true, a.throttled != 0, a.remaining_us, a.replenish_at_ns};
    }
    CbsState st;                                                      // fallback backend: read-only lookup, then poke the per-CPU kicker
    if (!bpf_.lookup_cbs_state(s.cbs_key, &st)) return {true, false, 0, 0};    // no such server yet (task not bound): nothing to do, same as the BPF program's answer
    if (st.throttled && mono_now() >= st.replenish_at_ns)
        kickers_[s.cpu]->poke();                                      // futex wake of a SCHED_FIFO 97 thread pinned to s.cpu → schedule() → ops.dispatch
    return {true, st.throttled != 0, st.remaining_us, st.replenish_at_ns};
}
```

- **상태를 쓰지 않는다**: Replenisher와 kick 프로그램은 `cbs_map`을 읽기만 한다. 예산 갱신과 승격은 BPF `dispatch`가 한다(§4.4). 이전 초안의 사용자 공간 보충(`apply_replenish_userspace`)과 kick 프로그램 안의 보충은 삭제했다 — 다른 CPU에서 진행 중인 예산 차감을 덮어쓰는 갱신 유실이 생기기 때문이다.
- **다음 확인 시각**: `replenish_at_ns`는 "갱신 시점 + $T_s$"로만 바뀐다. 확인 시점에 윈도우가 열려 있으면 그 끝에 다시 확인하고, 열린 윈도우가 없으면 `now + T_s`에 확인하므로 그 사이에 새로 열린 윈도우의 끝(≥ `now + T_s`)보다 늦지 않다. 윈도우가 끝났는데 아직 승격되지 않은 서버(TT 슬롯이 활성이어서 kick을 보내지 않은 경우, kick이 처리되기 전에 읽은 경우)는 승격될 때까지 1 ms마다 다시 확인한다. "TT 슬롯이 활성"은 `tt_active_on_cpu`의 판정, 곧 그 CPU에 실행 가능한 TT 태스크가 있다는 뜻이다. TT 태스크가 잠들면 CPU가 다음 태스크를 고르면서 dispatch가 호출되므로 kick 없이도 승격된다.
- **테이블 변경·종료 통지**: 조건 변수만 쓴다. 이전 초안의 `clock_nanosleep` + `SIGUSR2`는 처리기가 없으면 프로세스를 종료시키고 신호를 놓칠 수 있어 삭제했다. `epoch_`와 `running_`은 잠금 아래에서만 읽고 쓴다. `epoch_`는 테이블 전체를 교체할 때만 올린다. `remove()`는 루프가 꺼내 들고 있는 서버가 바로 그 서버일 때만 되돌려 넣지 않게 표시하므로, 다른 서버를 제거해도 진행 중이던 서버의 확인 일정은 사라지지 않는다. `stop()`은 잠금 아래에서 `running_ = false`로 바꾸고 `cv_`를 깨운 뒤 join한다.
- **할당**: 힙 저장소는 `set_servers()`가 잠금 밖에서 만들어 교체한다. `remove()`는 제자리에서 지우므로 할당하지 않는다. 루프는 `pop` 뒤에 `push`만 하므로 용량이 늘지 않는다.

`BpfLoader` 추가 API: `bool lookup_cbs_state(uint64_t, CbsState*)`(읽기 전용), `bool delete_cbs_state(uint64_t)`, `bool delete_task_meta(uint32_t)`, `int run_replenish_kick(ReplenishKickArgs*)`, `int get_job_trace_rb_fd()`, `bool set_trace_enabled(bool)`(bss 맵 쓰기), `bool lookup_task_meta(uint32_t, TaskMeta*)`.

### 5.4 `job_trace.{h,cpp}`, `event_source_monitor.{h,cpp}` — 신규 수집기

**(a) `JobTraceCollector` — 어댑터 K의 Job 레코드**

```cpp
class JobTraceCollector {
public:
    struct Hist { std::array<uint32_t, 1024> bins_10us{}; uint32_t overflow = 0; uint64_t count = 0; uint64_t sum_ns = 0; uint64_t max_ns = 0;
                  uint64_t percentile_ns(double p) const; };
    struct TaskStats { uint64_t activations, completions, dmiss, early_dmiss, budget_episodes, mit_violations, marker_jobs;
                       uint64_t arrivals, consumed, pending;                 // Event source counters from the latest EventSourceSnapshot (marker sources; else 0)
                       Hist response, wake; uint64_t last_budget_used_us, max_budget_used_us; };   // OBS-001
    JobTraceCollector(BpfLoader& bpf, const std::string& node_id);
    void set_task_names(std::map<uint64_t, std::string> hash_to_name);
    void set_marker_active(uint64_t key, bool active);        // from EventSourceMonitor → JOB_FLAG_MARKER on that task's records
    void set_enabled(bool);                                   // writes trace_enabled; opens/closes CSV sink
    void set_on_task_exit(std::function<void(uint64_t task_hash)>);
    void add_tt_jitter_sample(uint32_t cpu, uint32_t slot_idx, uint64_t target_ns, uint64_t actual_ns);   // from TimerMaster
    void add_source_snapshot(const EventSourceSnapshot& s);   // from EventSourceMonitor
    const TaskStats& stats(uint64_t key) const;
    void clear_task(uint64_t key);
    void start(); void stop();
private:
    static int rb_cb(void* ctx, void* data, size_t len);   // JobRecord → stats + CSV line
    // CSV: "job,<node>,<wl_hash>,<task>,<job_seq>,<cpu>,<t_act>,<t_start>,<t_done>,<budget_used_us>,<flags>"   (JobRecord order; <task> = name looked up from task_id_hash)
    //      "src,<node>,<wl_hash>,<task>,<event_seq>,<consumed_seq>,<pending>,<mit_violations>,<last_t_arr>,<t_sample>"
    //      "tt,<node>,<cpu>,<slot_idx>,<target_ns>,<actual_ns>,<jitter_ns>"
};
```

- 응답 시간 히스토그램: `t_done − t_act`, 깨움 지연: `t_start − t_act`(`t_start==0`이면 제외). 10 µs 빈 × 1024(=10.24 ms) + overflow.
- `job` 레코드는 커널이 관측한 Job 구간이다(§4.5). Job 레코드에는 `n_events`와 Event별 정보(`t_arr`, `event_seq`)를 병합하지 않는다. ringbuf 소비는 비동기이므로 레코드 처리 시점의 SHM 값은 이미 다음 Event의 것일 수 있다. Event가 유실 없이 소비되었는지는 `src` 레코드의 `event_seq`와 `consumed_seq`로 확인한다. Event↔Job 대응과 $t_{rel}$ 기준 응답 시간은 검증 프레임워크가 `job`·`src` 레코드와 생성기 로그를 `CLOCK_MONOTONIC` 기준으로 오프라인 대조해 산출한다(DDR-008 D-16). 이 문서의 응답 시간 히스토그램과 DMISS는 $t_{act}$ 기준이다.
- `JOB_FLAG_MARKER`: 해당 태스크의 Event source가 marker-active이면 레코드에 설정한다(IF-V-001 `marker_used`).
- `JOB_FLAG_TASK_EXIT` 레코드 → `on_task_exit` → `TaskRegistry::notify_exit`.
- 출력 파일: `<node_id>.jobtrace.csv`(기본 비활성; `-t` CLI 또는 `SIGUSR1`로 토글).

**(b) `EventSourceMonitor` — 어댑터 M의 Event source 통계**

```cpp
struct EventSourceSnapshot {            // model-level Event source statistics (IF-V-004)
    uint64_t key;                       // wl_hash ^ task_hash
    uint32_t event_seq;                 // arrivals notified (cumulative)
    uint32_t consumed_seq;              // events handed to jobs (cumulative)
    uint32_t pending;                   // event_seq − consumed_seq
    uint32_t mit_violations;            // arrival-based, cumulative since bind
    uint64_t last_arr_ns, t_sample_ns;
};

class EventSourceMonitor {
public:
    using OnMit          = std::function<void(uint64_t key, uint32_t delta, uint64_t last_arr_ns)>;   // → FaultMonitor (arrival-based MIT)
    using OnSnapshot     = std::function<void(const EventSourceSnapshot&)>;                           // → JobTraceCollector
    using OnMarkerActive = std::function<void(uint64_t key, bool active)>;                            // → FaultMonitor, JobTraceCollector
    void set_shm(volatile struct timpani_ttsched_shm* shm, const EvtSlotTable& slots);                   // at table apply: keeps Base of retained keys, adds Base for new keys, drops removed keys
    void clear_task(uint64_t key);
    uint64_t last_violation_ns(uint64_t key) const;   // atomic load of the slot's last_viol_ns (0 = none) → FaultMonitor (PRT-007)
    void poll();                        // called from the FaultMonitor poll thread every 100 ms (not an RT path)
private:
    struct Base { uint32_t event_seq; uint32_t mit_viol; bool marker_active; };
    std::map<uint64_t, Base> base_;
};
```

- **스레드와 잠금**: 이 절과 §5.5의 객체는 테이블 적용 스레드(gRPC 수신), FaultMonitor 폴링 스레드(ringbuf 콜백과 `event_sources.poll()`), JobTraceCollector 스레드가 함께 쓴다. 객체마다 뮤텍스 하나로 자신의 맵(`base_`, `l2_`, 이름표·통계)을 보호하고 맵 접근 동안만 잡는다. 콜백(`FaultCallback`, `OnMit`, `OnMarkerActive`, `OnSnapshot`, `on_task_exit`)은 잠금을 놓은 뒤 호출한다(TaskRegistry와 같은 규칙). SHM 매핑 포인터는 데몬 수명 동안 바뀌지 않으며 `set_shm`은 key→슬롯 표만 잠금 아래에서 교체한다.
- `poll()`은 각 EVT 슬롯의 누적 카운터를 atomic load로 읽는다. `consumed_seq`를 먼저, `event_seq`를 나중에 읽어 `pending`이 음수가 되지 않게 한다. 카운터는 누적 값이므로 폴링 주기 사이의 변화는 유실되지 않는다(FR-N-026). 차이는 `uint32_t` 모듈로 연산으로 구한다.
- 기준값(`event_seq`, `mit_viol_count`)은 TimerMaster가 그 source의 슬롯을 활성화하기(`kind = EVT`) **직전에** 읽어 `EvtSlotTable`로 넘겨준 값이다(§5.6). 활성화 뒤에 읽으면 그 사이에 수락된 통지의 위반이 기준값에 묻힌다. `event_seq`가 기준값에서 처음 증가하면 그 source를 **marker-active**로 표시하고 `OnMarkerActive(key, true)`를 호출한다. 이후 그 source의 MIT 보고 원천은 도달 기준이며(§5.5), 도달 기준 위반은 기준값부터 센다: 위반은 통지 없이는 생기지 않으므로 첫 통지 이후의 위반이 빠짐없이 들어간다. 중복도 없다: `poll_tick()`은 폴트 ringbuf를 먼저 꺼내 두고 그다음 `poll()`을 부른 뒤에 꺼내 둔 이벤트를 처리한다(§5.5). 통지로 생긴 활성화의 활성화 기준 이벤트는 그 통지의 `event_seq` 증가보다 늦게 나오므로, 처리할 때는 그 source가 이미 marker-active여서 버려진다. 따라서 처음부터 통지를 쓰는 source의 계수는 정확하다(AC-008-07). 표준 깨움만 쓰다가 도중에 통지를 시작한 source는 예외다: 전환을 알아챈 `poll_tick()`에서 함께 꺼낸 활성화 기준 이벤트(전환 직전 최대 100 ms)가 버려져 그만큼 빠질 수 있다. 전환은 슬롯 활성화당 한 번이다.
- `mit_viol_count`를 먼저, `last_viol_ns`를 나중에 읽는다(notify는 시각을 먼저 쓰고 횟수를 올린다). `mit_viol_count` 증분이 있으면 `OnMit(key, delta, last_arr_ns)`. 트레이스가 비활성이어도 이 경로는 동작한다.
- 테이블 재적용(`set_shm`) 시 유지되는 source의 기준값과 marker-active 상태는 그대로 둔다(슬롯 인덱스와 카운터는 TimerMaster가 보존, §5.6). 기준값은 슬롯이 활성화될 때만 잡는다. 그래서 두 폴링 사이에 테이블이 다시 적용되어도 증분이 사라지지 않는다.

### 5.5 `fault_monitor.{h,cpp}` — 집계 확장

```cpp
void set_task_params(Key key, uint32_t period_us, uint32_t current_limit);   // replaces update_current_limits for L2
struct MitWindow { uint64_t start_ns = 0; uint32_t count = 0; };
struct L2State {
    MitWindow mit;                          // open aggregation window (≤ 1 report per T_s)
    uint64_t  last_k_viol_ns = 0;           // t_act of the last activation-based violation (adapter K)
    uint64_t  last_episode_replenish_at = 0;// BUDGET_EXCEED de-duplication key
    bool      marker_active = false;        // MIT origin is arrival-based (adapter M)
    uint32_t  dmiss_count = 0; bool dmiss_reported = false;   // existing DDR-012 threshold state
};
std::map<Key, L2State> l2_;
void set_marker_active(Key key, bool active);                 // from EventSourceMonitor
void on_arrival_mit(Key key, uint32_t delta);                 // from EventSourceMonitor
void poll_tick();                                             // every 100 ms, in this order: (1) drain fault_ringbuf into a local buffer (the libbpf callback only copies),
                                                              // (2) event_sources.poll(), (3) process the buffered events (switch below), (4) flush elapsed MIT windows.
                                                              // (2) before (3) is what makes the MIT count exact across the marker-active switch (§5.4)
using FaultCallback = std::function<void(const FaultEvent&, uint32_t flags, uint32_t count, uint32_t dmiss_count)>;

// BPF fault_type → node_control.FaultType: explicit table, never an integer cast (numbers differ; DDR-008 D-10)
static node::v1::FaultType to_proto_fault(uint8_t bpf_type) {
    switch (bpf_type) {
    case FAULT_DMISS:                 return node::v1::DMISS;                  // 0 → 1
    case FAULT_BUDGET_EXCEED:         return node::v1::BUDGET_EXCEED;          // 1 → 2
    case FAULT_MIT_VIOLATION:         return node::v1::MIT_VIOLATION;          // 2 → 4
    case FAULT_WAKEUP_LATENCY_EXCEED: return node::v1::WAKEUP_LATENCY_EXCEED;  // 3 → 5 (P2)
    case FAULT_WATCHDOG:              return node::v1::WATCHDOG;               // 4 → 3 (P2)
    default:                          return node::v1::UNKNOWN;
    }
}

// One roll-over helper for both MIT origins: flush the elapsed window first, then add.
void add_mit(Key key, uint32_t n, uint64_t now) {
    MitWindow& w = l2_[key].mit;
    if (w.count && now - w.start_ns >= period_ns(key)) flush_mit(key);   // sends FaultInfo{MIT_VIOLATION, count}, then w = {}
    if (w.count == 0) w.start_ns = now;
    w.count += n;
}
void on_arrival_mit(Key key, uint32_t delta) { add_mit(key, delta, mono_now()); }

// ring_buf_callback(): complete switch — every case ends with break
switch (ev.fault_type) {
case FAULT_DMISS: {
    L2State& s = l2_[key]; s.dmiss_count++;
    uint64_t t_act     = ev.activation_ns;                                                 // carried in the event, not derived from a user-space copy of D
    uint64_t last_viol = s.marker_active ? event_sources_.last_violation_ns(key)           // adapter M: slot.last_viol_ns
                                         : s.last_k_viol_ns;                               // adapter K
    uint32_t flags = ev.flags & (FLAG_THROTTLED | FLAG_EARLY | FLAG_CARRYOVER);            // BPF bits that are forwarded; bit 0 is recomputed here
    if (last_viol && last_viol + period_ns(key) >= t_act) flags |= FLAG_MIT_RELATED;       // PRT-007: violation at or after t_act − T_s
    if (s.marker_active) flags |= FLAG_MARKER;
    if (s.dmiss_count > limit(key) && !s.dmiss_reported) { s.dmiss_reported = true; callback(ev, flags, 1, s.dmiss_count); }   // threshold rule unchanged
    break;
}
case FAULT_BUDGET_EXCEED: {
    L2State& s = l2_[key];
    if (s.last_episode_replenish_at != ev.expected_deadline_ns) {                          // one report per throttle episode
        s.last_episode_replenish_at = ev.expected_deadline_ns; callback(ev, ev.flags, 1, 0);
    }
    break;
}
case FAULT_MIT_VIOLATION: {
    L2State& s = l2_[key];
    if (s.marker_active) break;                    // arrival-based origin is authoritative for this source: never sum both (FR-N-014)
    s.last_k_viol_ns = ev.actual_completion_ns;    // t_act of the violating activation
    add_mit(key, 1, mono_now());
    break;
}
default: break;
}
```

보고 형식(`FaultInfo{MIT_VIOLATION, count}`, source당 $T_s$ 윈도우에 1건)은 원천과 무관하게 같다. `poll_tick()`은 경과한 윈도우를 비운다. `DMISS`의 `MIT_RELATED`는 집계 윈도우가 아니라 **마지막 위반 시각**으로 판정하므로, 윈도우가 넘어가 집계가 비워진 뒤에도 규칙(REQ-008 PRT-007: `t_act − T_s` 이후의 위반)이 그대로 적용된다. `dmiss_reported`/`limit()`은 기존 DDR-012 임계 규칙의 상태를 그대로 쓴다. `FLAG_*`는 §2.1의 `FaultInfo.flags` 비트다. `CARRYOVER`는 BPF가 세운 값을 그대로 전달한다(PRT-007 (b)). `l2_`는 뮤텍스로 보호하고 `callback`은 잠금을 놓은 뒤 호출한다.

`NodeClient::send_fault(...)`는 `type = to_proto_fault(ev.fault_type)`, `task_id`(hash→name), `node_id`, `timestamp_ns`(REALTIME), `count`, `flags`, `job_seq`를 채운다(FR-N-015).

### 5.6 `timer_master.{h,cpp}` — SHM v2 및 L2 슬롯

```cpp
struct EvtTaskEntry { std::string task_name; uint64_t task_id_hash; uint64_t workload_id_hash; uint32_t mit_us; };
struct EvtSlotInfo { int idx; uint32_t base_event_seq; uint32_t base_mit_viol; };   // base_*: read right before kind = EVT is stored
using EvtSlotTable = std::map<uint64_t, EvtSlotInfo>;                               // key (wl_hash ^ task_hash) → slot; returned by both calls below
EvtSlotTable set_schedule_table(const std::vector<SlotEntry>& slots, const std::vector<EvtTaskEntry>& evt_tasks,
                        uint64_t hyperperiod_us, uint64_t epoch_ns);
```
- `bool same_tt_table(slots, hp, epoch) const`: 현재 적용된 TT 슬롯 집합(cpu/offset/duration/task_hash)·hyperperiod·epoch와 동일하면 true. `EvtSlotTable update_evt_slots(evt_tasks)`: per-CPU 스레드를 유지한 채 EVT 슬롯만 갱신한다(아래 EVT 규약; `magic`은 건드리지 않는다). FR-N-006 / D-17(b).
- **시작**: SHM 객체 `/timpani_ttsched`가 이미 있으면(이전 데몬의 비정상 종료) 그것을 열어 종료 절차(`magic=0`, 모든 슬롯 `kind=NONE` → `counter++` → wake)를 수행하고 `shm_unlink`한 뒤, `O_CREAT|O_EXCL`로 새 객체를 만들고 `fchmod(fd, 0666)`으로 권한을 맞춘 뒤(`shm_open`의 모드는 umask에 깎인다) `version=2`와 새 `instance_id`(0이 아닌 값, 예: 생성 시각 `CLOCK_REALTIME` ns)를 쓴다. 남은 객체에 붙어 있던 앱은 −1을 받고 `ttsched_init()`으로 새 객체에 붙는다. 현행 생성자의 "있으면 그대로 연다"는 동작은 없앤다.
- **TT 슬롯**: 기존 핸드셰이크 그대로다 — `magic=0` → TT 슬롯 `counter++` + wake → 이름 기록 → `n_tasks`(release) → `generation++`(release) → `magic=TIMPANI_TTSCHED_MAGIC`(release). 유지되는 TT 태스크는 이전 인덱스를 쓰고, 새 TT 태스크는 EVT에 배정된 적이 없는 가장 낮은 빈 인덱스를 쓴다. 현행 코드의 "전 슬롯 `counter` 증가"는 **TT 슬롯에만** 적용한다.
- **EVT 슬롯 규약**(Event source 하나의 상태. `libttsched`는 `kind`와 이름으로만 유효성을 판정하며 `magic`/`generation`을 보지 않는다, §6.2). TimerMaster의 저장은 모두 원자 연산이다(이름은 8바이트 워드 2개로 저장).
  - **고정 배정**: L2 `task_id`에 한 번 배정한 슬롯은 데몬 수명 동안 그 이름의 것이다(`evt_slot_of_name_`). 다른 이름에 재사용하지 않고 0으로 지우지 않는다. 새 이름은 한 번도 쓰지 않은 슬롯(0으로 초기화된 상태)을 받는다. 남은 슬롯이 없으면 그 엔트리는 적용 오류다.
  - **새 배정**: `name`, `mit_us`를 쓰고, `event_seq`·`mit_viol_count`를 읽어 기준값으로 기록한 뒤, **마지막에** `kind = EVT`(release). 기준값은 `EvtSlotTable`로 `EventSourceMonitor`에 전달된다(재활성화도 같다).
  - **유지**: `mit_us`만 다시 쓴다. `event_seq`·`consumed_seq`·`last_arr_ns`·`last_viol_ns`·`mit_viol_count`·`counter`를 건드리지 않고 깨우지도 않는다. 그래서 TT 테이블을 다시 발행하는 동안에도 통지·대기가 끊기지 않고 미소비 Event가 보존된다.
  - **비활성화**(테이블에서 빠짐, STOP)와 **종료**: `kind = NONE`(release) → `counter++` → `FUTEX_WAKE(INT_MAX)`. 이름과 누적 값은 남긴다. 대기 중인 소비 태스크는 깨어나 `kind`를 보고 −1로 복귀한다. 잠들기 직전이었다면 `counter`가 바뀌어 `FUTEX_WAIT`가 즉시 실패하므로 깨움을 놓치지 않는다. `event_seq`는 바뀌지 않으므로 가짜 Event가 생기지 않는다.
  - **재활성화**(같은 `task_id`가 다시 등록됨): `mit_us`를 쓰고 `kind = EVT`. 누적 값이 이어지므로 `EventSourceMonitor`의 값이 거꾸로 가지 않고, 비활성 기간에 남아 있던 미소비 Event도 전달된다.
- TimerMaster는 EVT 슬롯의 `kind`/`name`/`mit_us`와 (비활성화·종료 때) `counter`만 쓴다. `shm()` 접근자와 key→슬롯 인덱스 표를 `EventSourceMonitor`에 제공한다.
- `cpu_thread_loop`: 지터 샘플을 `JobTraceCollector::add_tt_jitter_sample()`로 전달(락프리 SPSC 링, RT 안전). stdout 통계는 유지.

### 5.7 CLI/설정 추가
`-t` (job trace on), `-r <ms>` (rescan period, default 500), `-k syscall|thread` (kick backend, default syscall), `-T` (trace sink path). 모두 선택.

### 5.8 timpani-n 테스트 (GTest 타깃 신설: `timpani-n/tests/`)

| 테스트 | 대상 |
|:--|:--|
| `JobFsmTest.ActivationOnlyFromWaiting` / `MidJobWakeupKeepsActivation` / `SleepCompletesJob` / `PreemptDoesNotComplete` | `job_fsm.h` (FR-B-001~003) |
| `JobFsmTest.MitViolationFlagsAndCounts` / `FirstJobNoMitCheck` | FR-B-006 |
| `JobFsmTest.DmissOnceEvenIfEarly` | FR-B-020/021 |
| `CbsChargeTest.NanosecondCarry` / `ThrottleTransitionOnce` | FR-B-012/013 |
| `CbsWindowTest.ConformingActivationAlwaysRefills` (DDR-008 D-04 반례: T=18 ms, C=2 ms, 활성화 16.0/34.0 ms → 스로틀 없음) / `EarlyActivationKeepsRemainingAndMarksCarryover` / `ToleranceAbsorbsWakeJitter` (간격 17.95 ms: 보충, MIT 위반 아님) / `AfterViolationWindowShiftsThenRecoversWithSlack` (D-04 "위반 후 동작" 타임라인: 0/15/33 ms, 여유가 있으면 복귀) / `ExactBudgetJobIsThrottled` (차감액 = $C_s$ 경계) / `NoRefillWhileActiveAndNotThrottled` / `PromotionRefillsAndOpensWindowFromNow` / `ConsumptionPerWindowBounded` | `cbs_on_activation`, `cbs_try_promote` (FR-B-014/017) |
| `QueueOrderTest.RequeueKeepsActivationRank` / `PromotedJobGoesToTail` | `queue_key_ns` (FR-B-016) |
| `TaskRegistryTest.ResolvesLateThread` (fork+prctl 후 ≤1 s) / `DetectsExit` / `BinderMayReenterRegistry` | FR-N-003/004 |
| `ReplenisherTest.HeapOrder` / `SwapOnTableChangeWakesLoop` / `RearmsFromReturnedReplenishAt` / `RetriesWhileDueButNotPromoted` / `RemoveDuringKickDoesNotPushBack` / `RemoveOfOtherServerKeepsInflightServer` / `StopWakesEmptyLoop` / `NeverWritesCbsMap` / `EmptyHeapSleeps` (kick 백엔드 mock) | FR-N-010 |
| `ApplyTest.UnchangedEntryWritesNothing` (재적용 후 `job_seq`·`remaining_us` 불변) / `ChangedEntryIsUnboundThenRebound` (`job_seq`는 0에서 다시 시작, SHM 누적 값은 유지) / `ServerExistsBeforeSchedExt` / `MissingCpuIsApplyError` | FR-N-005, IF-ON-003, D-17 |
| `FaultMonitorTest.BudgetExceedOncePerEpisode` / `MitWindowAggregation` / `DmissThresholdUnchanged` | FR-N-012~014 |
| `JobTraceTest.HistogramPercentiles` / `SourceSnapshotUpdatesStats` / `TracksMaxBudgetUsed` / `TaskExitRecordNotifiesRegistry` | FR-N-020~022 |
| `ShmV2Test.EvtSlotsAllocatedStable` / `MagicVersionMismatchReturnsMinusOne` / `EvtCountersPreservedAcrossRepublish` / `TtRepublishDoesNotBumpEvtCounter` / `DeactivateBumpsWakeWordNotEventSeq` / `SlotPinnedToNameAcrossReAdd` (누적 값 이어짐) / `StaleObjectKilledAndRecreatedAtStartup` (새 `instance_id`, 모드 0666) | IF-L-001/002 |
| `FaultMonitorTest.MitCountExactAcrossMarkerSwitch` (첫 통지 직후의 버스트: 보고 count = 도달 기준 위반 수, K 이벤트 중복 없음) | FR-N-014 |
| `EvtApiTest.WaitReturnsBatchCount` (notify×3 중 대기 없음 → n=3) / `NoLostEventAcrossBusyJob` (처리 중 notify → 다음 wait 즉시 복귀, n=1) / `NoDuplicateAfterReinit` (`consumed_seq` 보존) | FR-L-001, MOD-007 (R2~R4) |
| `EvtApiTest.NotifyCountsArrivalMitViolation` / `NotifyBeforeSourceRegisteredReturnsMinusOne` / `HandleSurvivesRepublishAndDaemonRestart` / `NotifyAcceptedDuringTtRepublish` / `ConcurrentNotifiersLoseNoEvent` | FR-L-002/004/005/007/008 |
| `EvtApiTest.WaitReturnsMinusOneOnReadOnlyMapping` (10만 회 반복해도 매핑 수 불변) / `DeactivationNotReturnedAsEvent` / `WaitNotLostWhenSourceDeactivatedBeforeSleep` / `PendingEventsSurviveDeactivateReactivate` (앱 큐 유지) / `ResetReturnedOnceAfterDaemonRestart` (−2 한 번, 이후 정상) / `NotifyMinusOneMeansNotCounted` / `InitIsIdempotentNoMappingLeak` | FR-L-001(d)(e), FR-L-002, FR-L-005 |
| `EventSourceMonitorTest.SnapshotPendingNeverNegative` / `MarkerActiveOnFirstArrival` / `MitDeltaNotLostBetweenPolls` / `BaselineKeptAcrossReapply` | FR-N-026 |
| `FaultMonitorTest.MarkerSourceUsesArrivalMitOnly` (BPF MIT 이벤트 무시, 합산 없음) / `MarkerTransitionDoesNotDoubleCount` / `ArrivalMitRollsWindowOver` / `DmissMitRelatedByLastViolationTime` (윈도우가 넘어간 뒤에도 유지) / `DmissNotMitRelatedWhenViolationIsOlderThanTs` | FR-N-012/014, PRT-007 |

---

## 6. `libttsched` v2 (`sample-apps/src/libttsched.{h,c}`) — 관측 어댑터 M

> SRS OI-14 종결: 별도 라이브러리 없이 `libttsched`를 확장한다(DDR-008 D-03).
> 이 절의 API는 공통 모델의 **Arrival**(notify)과 **wait point / R2~R4**(wait)를 구현한다. Activation/Start/Completion의 관측과 집행은 BPF(§4)가 하며, 라이브러리는 폴트를 판정하지 않는다.

### 6.1 헤더

```c
#define TIMPANI_TTSCHED_MAGIC   0x54494D51U   /* "TIMQ" — v2; v1 "TIMP" readers get -1 (ABI break by design) */
#define TIMPANI_TTSCHED_VERSION 2U
#define TIMPANI_MAX_TASKS       64
#define TTSCHED_KIND_NONE 0U                  /* unused or inactive slot */
#define TTSCHED_KIND_TT   1U                  /* slot = TT activation point (notifier: TimerMaster) */
#define TTSCHED_KIND_EVT  2U                  /* slot = active L2 Event source (notifier: application adapter) */
#define TTSCHED_EVT_BITSET 0x00455654U        /* "EVT": FUTEX_WAIT_BITSET mask marking an L2 wait point (P2 classifier); distinct from both magics */
#define TTSCHED_EVT_MAX_SOURCES 16            /* per-process handle table (static, no heap) */
#define TTSCHED_EVT_RESET (-2)                /* ttsched_evt_wait(): the daemon was restarted, event counts restarted from 0 */

struct timpani_task_slot {            /* 64 bytes */
    char     name[16];                /* task_id (comm, ≤ 15 bytes + NUL, zero padded). For an L2 task the slot is pinned to this name for the SHM object's lifetime */
    uint32_t counter;                 /* futex word. TT: tick count (TimerMaster). EVT: wake sequence (notify and deactivation both bump it) — NOT the event count */
    uint32_t kind;                    /* TTSCHED_KIND_*. Written last on first allocation / re-activation and first on deactivation (release/acquire) */
    /* ---- EVT only: Event source state (read by timpani-n EventSourceMonitor) ---- */
    uint32_t consumed_seq;            /* cumulative: events already handed to jobs; written only by the consuming task */
    uint32_t mit_us;                  /* configuration: MIT published by timpani-n (0 = no arrival-based check) */
    uint64_t last_arr_ns;             /* latest value: t_arr of the most recent event (CLOCK_MONOTONIC); written by notify */
    uint32_t mit_viol_count;          /* cumulative: arrival-based MIT violations; written by notify */
    uint32_t event_seq;               /* cumulative: accepted arrivals; written by notify only */
    uint64_t last_viol_ns;            /* latest value: t_arr of the most recent MIT-violating arrival (0 = none); written by notify */
    uint64_t _reserved;
};

struct timpani_ttsched_shm {          /* 32 + 64*64 = 4128 bytes */
    uint32_t magic; uint32_t version; uint32_t n_tasks; uint32_t generation;
    uint64_t instance_id;             /* non-zero, different for every SHM object the daemon creates; constant for the object's lifetime */
    uint64_t _reserved;
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
};

typedef int ttsched_evt_t;                       /* Event source handle (>= 0) */

void ttsched_init(void);                         /* idempotent: unmaps the calling thread's previous mapping first, then maps RDWR if permitted,
                                                    else RDONLY. RDONLY is enough for TT wait; EVT wait needs RDWR */
int  ttsched_wait_next_period(void);             /* L1, unchanged */

/* Event source handle for the L2 task `task_id`. Returns >= 0, or -1 if the handle table is full.
 * Succeeds even if the source is not registered yet; it is resolved lazily and re-resolved on table updates. */
ttsched_evt_t ttsched_evt_open(const char *task_id);

/* Arrival: one event arrived at `src`. t_arr_ns == 0 → now. Wakes the consuming task if it is waiting.
 * Returns 0 if the event was accepted. Returns -1 if the source is not registered or the SHM cannot be written:
 * the event is NOT counted, and the caller must make sure the item it has just queued is not processed (remove it or mark it invalid). */
int  ttsched_evt_notify(ttsched_evt_t src, uint64_t t_arr_ns);

/* Wait point of the calling L2 task. Blocks until at least one event is pending, then returns
 * n >= 1 = number of events this job must consume (events that arrived since the previous return).
 * Returns at once if events are already pending. Process exactly n items (never "drain until empty").
 * Exactly one thread waits per source: the thread whose comm is the task_id.
 * -1 (after a 1 ms back-off): cannot wait now — daemon down, source inactive or not registered yet, or SHM not writable
 *   → KEEP the app queue, call ttsched_init() and retry. Counts continue within one daemon lifetime.
 * TTSCHED_EVT_RESET (-2), returned once: the calling thread is now attached to a different SHM object than before
 *   (the daemon was restarted, counts restarted from 0) → DROP everything in the app queue, then call again. */
int  ttsched_evt_wait(void);
```

### 6.2 구현 요지

```c
struct evt_src { char name[16]; int idx; /* atomic; -1 = unresolved */ int used; };
static struct evt_src g_src[TTSCHED_EVT_MAX_SOURCES];           /* process-wide; open() and remap under g_evt_lock (spinlock); the notify fast path is lock-free */
static volatile struct timpani_ttsched_shm *g_evt_shm;          /* process-wide O_RDWR mapping used by notify; atomic pointer */

/* A slot belongs to `name` iff kind == EVT and the name matches (two atomic 8-byte loads). Independent of magic / generation / n_tasks. */
static inline int evt_slot_is(volatile struct timpani_task_slot *sl, const char *name);
static int evt_find_slot(volatile struct timpani_ttsched_shm *shm, const char *name) {
    for (int i = 0; i < TIMPANI_MAX_TASKS; i++) if (evt_slot_is(&shm->tasks[i], name)) return i;
    return -1;
}
/* evt_shm_rw(): returns g_evt_shm, mapping it on first use (O_RDWR, version == 2), else NULL.
 * evt_shm_remap(): under g_evt_lock, at most once per 100 ms: fstat the named object; if it is a different object (st_ino) from
 *   the mapped one, map it, publish the new pointer and reset every g_src[i].idx to -1. The OLD mapping is never unmapped
 *   (4 KiB per daemon restart), so a notifier that still holds the old pointer stays safe. */

ttsched_evt_t ttsched_evt_open(const char *task_id) {
    /* under g_evt_lock: return the existing entry with the same name, else claim a free one: copy name (zero padded), idx = -1 */
}

int ttsched_evt_notify(ttsched_evt_t src, uint64_t t_arr_ns) {
    if (src < 0 || src >= TTSCHED_EVT_MAX_SOURCES || !g_src[src].used) return -1;
    struct evt_src *s = &g_src[src];
    volatile struct timpani_ttsched_shm *shm = evt_shm_rw();
    if (!shm) return -1;
    int idx = __atomic_load_n(&s->idx, __ATOMIC_RELAXED);               /* cache = one atomic word; the slot is re-validated on every call */
    if (idx < 0 || !evt_slot_is(&shm->tasks[idx], s->name)) {           /* unresolved, inactive, or the daemon was restarted */
        idx = evt_find_slot(shm, s->name);
        if (idx < 0 && (shm = evt_shm_remap()) != NULL) idx = evt_find_slot(shm, s->name);
        __atomic_store_n(&s->idx, idx, __ATOMIC_RELAXED);
        if (idx < 0) return -1;                                         /* source not registered / inactive: decided BEFORE any counting → NOT counted */
    }
    volatile struct timpani_task_slot *sl = &shm->tasks[idx];           /* pinned to this name for the object's lifetime: never another source's slot */
    if (t_arr_ns == 0) t_arr_ns = mono_now_ns();
    uint64_t prev = __atomic_exchange_n(&sl->last_arr_ns, t_arr_ns, __ATOMIC_RELAXED);
    uint32_t mit  = __atomic_load_n(&sl->mit_us, __ATOMIC_RELAXED);
    if (prev && mit && t_arr_ns - prev < (uint64_t)mit * 1000ULL) {     /* arrival-based MIT check (count only, no verdict) */
        __atomic_store_n(&sl->last_viol_ns, t_arr_ns, __ATOMIC_RELEASE);   /* timestamp first, then the count */
        __atomic_fetch_add(&sl->mit_viol_count, 1, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&sl->event_seq, 1, __ATOMIC_SEQ_CST);            /* Arrival: counted (never lost, even if the task is ACTIVE → R2) */
    __atomic_fetch_add(&sl->counter, 1, __ATOMIC_SEQ_CST);              /* wake word: makes a concurrent FUTEX_WAIT fail or return */
    syscall(SYS_futex, &sl->counter, FUTEX_WAKE, 1, NULL, NULL, 0);     /* wakes the task only if it is WAITING → R1 */
    return 0;                                                           /* accepted */
}

int ttsched_evt_wait(void) {
    /* g_shm, g_shm_writable, g_task_idx, g_my_name are thread-local (as in ttsched_wait_next_period) */
    if (!g_shm) ttsched_init();
    if (!g_shm || !g_shm_writable) { usleep(1000); return -1; }                 /* consumed_seq needs RDWR. No re-init here: the caller does it (no mapping leak) */
    if (g_task_idx < 0 || !evt_slot_is(&g_shm->tasks[g_task_idx], g_my_name)) {
        g_task_idx = evt_find_slot(g_shm, g_my_name);
        if (g_task_idx < 0) { usleep(1000); return -1; }                        /* not registered yet, inactive, or daemon restarted: caller re-inits and retries */
    }
    volatile struct timpani_task_slot *sl = &g_shm->tasks[g_task_idx];
    uint64_t inst = g_shm->instance_id;                                         /* identity of the SHM object (thread-local g_evt_instance: last one attached to; 0 = none) */
    if (g_evt_instance != inst) {
        int first = (g_evt_instance == 0); g_evt_instance = inst;
        if (!first) {                                                           /* daemon restarted: this object counts from 0 */
            __atomic_store_n(&sl->consumed_seq, __atomic_load_n(&sl->event_seq, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);   /* skip what was accepted before the app drops its queue */
            return TTSCHED_EVT_RESET;
        }
    }
    for (;;) {
        uint32_t w = __atomic_load_n(&sl->counter, __ATOMIC_ACQUIRE);           /* wake word FIRST: any later notify or deactivation changes it */
        if (!evt_slot_is(sl, g_my_name)) { g_task_idx = -1; usleep(1000); return -1; }   /* source deactivated / daemon stopping */
        uint32_t e    = __atomic_load_n(&sl->event_seq, __ATOMIC_ACQUIRE);      /* ONE snapshot per iteration: used for the compare, n and the store */
        uint32_t seen = __atomic_load_n(&sl->consumed_seq, __ATOMIC_RELAXED);   /* kept in SHM: survives re-init, table updates and deactivate/re-activate */
        if (e != seen) {
            __atomic_store_n(&sl->consumed_seq, e, __ATOMIC_RELEASE);           /* R4: this job consumes every event that arrived up to snapshot e */
            return (int)(e - seen);                                             /* n_events >= 1 (modulo-2^32 arithmetic) */
        }
        long r = syscall(SYS_futex, &sl->counter, FUTEX_WAIT_BITSET, w, NULL, NULL, TTSCHED_EVT_BITSET);   /* wait point: Completion seen by BPF */
        if (r == -1 && errno != EAGAIN && errno != EINTR) return -1;            /* EAGAIN / EINTR / wake → take a fresh snapshot */
    }
}
```

- **R1~R4 대응**: 대기 중 notify → futex wake → 새 Job(R1). 처리 중 notify → `event_seq`만 증가, 깨울 대기자 없음(R2). 다음 wait에서 `event_seq != consumed_seq` → 수면 없이 복귀(R3). 반환값이 그 Job의 `n_events`(R4). Job이 소비하는 Event의 경계는 스냅샷 `e`를 읽는 시점이다: 그 뒤에 도달한 Event는 다음 Job의 몫이다.
- **유실·중복 없음**: `event_seq`는 atomic 누적, `consumed_seq`는 소비 태스크만 쓴다. wait는 반복마다 `event_seq`를 **한 번** 읽어 비교·계산·저장에 모두 쓰므로, 두 번 읽는 사이에 도달한 Event가 소비된 것으로 처리되어 사라지는 일이 없다. 보장 대상은 notify가 0을 반환한(수락된) Event이고, 범위는 데몬이 살아 있는 동안이다. source가 비활성화되었다가 재활성화되어도 미소비 Event는 남는다(§5.6): 비활성인 동안 wait는 −1을 반환하고 앱은 큐를 그대로 두며, 재활성화 뒤의 wait가 남은 수를 n으로 돌려준다. 데몬이 종료·재시작하면 새 SHM 객체의 계수는 0에서 시작한다. wait는 `instance_id`가 달라진 객체에 처음 붙을 때 `TTSCHED_EVT_RESET`을 한 번 반환하고(그때까지 새 객체에 계수된 것은 소비한 것으로 넘긴다) 앱은 큐를 비운다. 재시작 경계에서 수락된 Event는 버려질 수 있다. 특히 −2를 돌려준 스냅샷과 앱의 큐 비우기 사이에 수락된 Event는 계수만 남고 항목은 버려질 수 있어, 그 직후 한 번은 n이 큐의 유효 항목 수보다 크거나(앱은 큐 길이만큼만 처리) 아직 통지되지 않은 항목이 한 건 먼저 처리될 수 있다. 이 어긋남은 그 한 번으로 끝나고 누적되지 않는다. 같은 이유로, 이전 소비 프로세스가 미소비 Event를 남기고 끝난 뒤 새 프로세스가 같은 `task_id`로 처음 붙으면 첫 wait가 그 수를 n으로 돌려준다(큐는 비어 있다).
- **깨움을 놓치지 않는 이유**: wait는 깨움 워드 `counter`를 **먼저** 읽고 나서 `kind`와 `event_seq`를 본다. 그 뒤의 notify나 비활성화는 `counter`를 바꾸므로 `FUTEX_WAIT`가 값 비교에서 즉시 실패(`EAGAIN`)하거나 깨어난다. 슬롯은 재배치되지 않으므로 대기 주소도 바뀌지 않는다. 타임아웃이 필요 없다.
- **−1의 의미가 정확한 이유**: notify는 수락 여부를 계수 **전에** 정하고 그 뒤로는 실패하지 않는다. 슬롯이 이름에 고정 배정되므로(§5.6) 검증과 증가 사이에 선점되어도 다른 source의 슬롯을 건드리지 않는다. 검증 직후 source가 비활성화되면 그 Event는 계수된 채 남고(0 반환) 재활성화 뒤에 전달된다.
- **재발행 중 동작**: EVT 경로는 `magic`/`generation`을 보지 않는다. TT 테이블을 다시 발행하는 `magic=0` 구간에도 유지되는 source의 notify는 수락되고 wait는 계속된다(§5.6). `ttsched_wait_next_period()`(TT)의 동작은 바뀌지 않는다.
- **동시성**: 핸들 캐시는 원자 정수 하나이고 슬롯 갱신은 모두 원자 연산이므로 같은 source에 대한 notify를 여러 스레드가 동시에 호출해도 데이터 경합이 없고 Event가 유실되지 않는다. 다만 `last_arr_ns` 교환 순서가 도달 순서와 다를 수 있어 `mit_viol_count`가 어긋날 수 있으므로, 계수 정확성이 필요하면 source당 notifier를 직렬화한다(HLD-008 §8.1 4항). 매핑 교체(`evt_shm_remap`)는 잠금 아래에서 하고 옛 매핑을 해제하지 않는다. **wait 호출자는 source당 하나**다(`consumed_seq`의 writer가 하나여야 한다). 같은 comm을 가진 스레드가 둘이면 계약 위반이다(REQ-008 A-03).
- **쓰기 매핑과 재초기화**: `ttsched_evt_wait`(`consumed_seq`)와 `ttsched_evt_notify`는 쓰기 매핑이 필요하다. 읽기 전용으로만 매핑할 수 있으면 둘 다 −1을 반환한다. wait는 매핑이 있는 상태에서 `ttsched_init()`을 스스로 다시 부르지 않고, `ttsched_init()`은 이전 매핑을 해제한 뒤 다시 연다. 그래서 −1 재시도가 반복되어도 매핑이 쌓이지 않는다. SHM 권한은 `0666`이다(TimerMaster가 `fchmod`로 맞춘다, §5.6. OI-15, DDR-008 D-03의 신뢰 경계).
- **비용**: notify = atomic 연산 8회 이내 + 시스템 콜 1회(≈ 1–2 µs). wait는 미소비 Event가 있으면 atomic 6회 이내, 없으면 블로킹.
- **P2 확장**: 태스크당 다중 source는 `ttsched_evt_open`의 source 지정 변형과 source별 슬롯으로 수용하며 notify/wait 시그니처는 그대로다. 마커 wait point의 커널 식별(L-7)은 이 함수의 대기 직전·수면 없는 복귀 지점에 훅을 둔다.

---

## 7. 샘플 앱

### 7.1 `sample-apps/src/evttask.c` (신규, 범용 이벤트 생성기 — 수용 기준의 기준 검증 수단)

```text
evttask --name cbs_task --source {timerfd|udp:<port>|pipe|self} --mit-us 18000 --wcet-us 1500
        [--burst N] [--overrun-us U] [--midjob-block-us B] [--marker on|off] [--count K] [--log FILE]

threads:
  worker (comm = --name): ttsched_init();
     loop: n = (--marker on) ? ttsched_evt_wait() : source_blocking_wait();     // adapter M : adapter K only (epoll_wait/read, n = 1)
           if (n == -1) { ttsched_init(); continue; }                           // cannot wait now: keep the queue
           if (n == TTSCHED_EVT_RESET) { drop_queue(); continue; }              // daemon restarted
           for i in 1..n: run_nsqrt_for(wcet_us ± overrun);                     // consume exactly n events (R4)
           if (midjob_block_us) nanosleep(midjob_block_us);                     // reproduces the A-06 violation
  source (not an L2 task): src = ttsched_evt_open(--name);
     generates events at MIT (or MIT/3 for --burst N):
       --marker on : enqueue item; if (ttsched_evt_notify(src, now) < 0) mark the item invalid   (for udp/pipe/timerfd the item still travels through that
                     channel into the app queue first, so the same binary exercises four delivery means with one TIMPANI config)
       --marker off: write(udp/pipe) or timerfd expiry is itself the wakeup
     logs (event_seq, t_arr) per event to --log for offline correlation with the job/src trace records
```
`cbstask.c`는 유지하되 README에서 `evttask`를 권장 생성기로 표기한다.

### 7.2 FCW 어댑터 축소 (`examples/someip/radar_based_fcw/fcw_consumer.cpp`)

수정 항목과 이유는 HLD-008 §8.4에 있다. 여기서는 수정 뒤의 코드 모양을 보인다.

| 현재 호출 | 수정 뒤 |
|:--|:--|
| `#include <timpani_rt.h>` | `#include <libttsched.h>` |
| `timpani_rt_init()` | 삭제. 워커가 `ttsched_init()`을 부른다 |
| `timpani_evt_register(wl, task, svc, inst, evt, 18000)` | `g_src = ttsched_evt_open("cbs_task")`. MIT는 워크로드 선언(`min_inter_arrival_us`) |
| `timpani_evt_mark_arrival(svc, inst, evt, seq)` | `ttsched_evt_notify(g_src, t_arr)` |
| `cbs_cv.wait(lock, ...)` | `n = ttsched_evt_wait()` |
| `timpani_cbs_bind_thread()` / `timpani_cbs_unbind_thread()` | 삭제 |
| `cbs_cv.notify_one()` / `notify_all()` | 삭제 |

```cpp
// Single-producer / single-consumer ring. The worker never takes a lock, so it cannot sleep on the queue mid-job (A-06).
// Neither side ever moves the other side's index backwards: a rejected item is marked invalid in place, not removed.
template <typename T, uint32_t N>                       // N: power of two
class SpscRing {
    struct Slot { T v; std::atomic<bool> valid; };
    Slot buf_[N]; std::atomic<uint32_t> head_{0}, tail_{0};
public:
    int push(const T& v) {                               // producer. Returns the slot index, or -1 if full
        uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == N) return -1;
        buf_[t & (N - 1)].v = v; buf_[t & (N - 1)].valid.store(true, std::memory_order_relaxed);
        tail_.store(t + 1, std::memory_order_release); return (int)(t & (N - 1));
    }
    void invalidate(int idx) { buf_[idx].valid.store(false, std::memory_order_release); }   // producer: TIMPANI did not accept this item
    bool pop(T& out, bool& valid) {                      // consumer
        uint32_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;
        out = buf_[h & (N - 1)].v; valid = buf_[h & (N - 1)].valid.load(std::memory_order_acquire);
        head_.store(h + 1, std::memory_order_release); return true;
    }
    void drop_invalid_head() {                           // consumer, while the source is inactive: free slots held by rejected items
        uint32_t h = head_.load(std::memory_order_relaxed);
        while (h != tail_.load(std::memory_order_acquire) && !buf_[h & (N - 1)].valid.load(std::memory_order_acquire)) ++h;
        head_.store(h, std::memory_order_release);       // stops at the first valid item: accepted items stay queued
    }
    void clear() { head_.store(tail_.load(std::memory_order_acquire), std::memory_order_release); }   // consumer, on TTSCHED_EVT_RESET only
};

static SpscRing<QueuedTarget, 64> alerts;                 // receive thread → worker
static SpscRing<FrameLog, 256>    frame_logs;             // worker → logger thread (plain thread, prints with std::cout)
static ttsched_evt_t              g_src;                  // Event source handle of the L2 task "cbs_task"
static std::atomic<uint64_t>      g_overflow{0}, g_rejected{0}, g_short{0}, g_log_dropped{0};
static std::atomic<bool>          g_logger_stop{false};

// vsomeip handler (middleware thread) — the only TIMPANI call on the receive side. No service/instance/event IDs.
// If vsomeip may call the handler from several threads, serialize the handler body with a mutex the worker never takes.
void on_obstacle_alert_received(const std::shared_ptr<vsomeip::message>& msg) {
    /* payload check and RadarTarget parsing: unchanged */
    uint64_t t_arr = clock_ns(CLOCK_MONOTONIC);
    /* --wakeup-delay-ms, if kept: delay HERE, after t_arr is taken and before notify (never in the worker) */
    int idx = alerts.push(QueuedTarget{*target, t_arr});  // 1. enqueue first
    if (idx < 0) { g_overflow++; return; }                // queue full: dropped by the app, not announced to TIMPANI
    if (ttsched_evt_notify(g_src, t_arr) < 0) {           // 2. Arrival
        alerts.invalidate(idx); g_rejected++;             // not accepted (workload not registered, daemon down): NOT counted → the worker skips it
    }
}

void cbs_worker_loop() {
    pthread_setname_np(pthread_self(), "cbs_task");       // task_id = comm (unchanged)
    ttsched_init();
    for (;;) {
        int n = ttsched_evt_wait();                       // wait point. n = events this job consumes (normally 1)
        if (n == TTSCHED_EVT_RESET) { alerts.clear(); continue; }               // daemon restarted: counts restarted from 0 → drop what is queued
        if (n < 0) { alerts.drop_invalid_head(); ttsched_init(); continue; }    // cannot wait now: KEEP accepted items, retry
        int done = 0;
        while (done < n) {                                // exactly n valid items; never "drain until empty"
            QueuedTarget item; bool valid;
            if (!alerts.pop(item, valid)) { g_short++; break; }   // fewer items than counted events: only right after a daemon restart or a consumer-process restart
            if (!valid) continue;                         // was never counted: discard without counting it
            /* overrun / -be injection (CPU burn) and the collision decision: unchanged */
            if (frame_logs.push(make_frame_log(item)) < 0) g_log_dropped++;   // no std::cout here: output may block. A full log ring drops the line and counts it
            ++done;
        }
    }
}

void logger_loop() {                                      // plain thread: the only place that prints per-frame lines
    FrameLog l; bool valid;
    for (;;) {
        if (frame_logs.pop(l, valid)) { /* print l */ continue; }
        if (g_logger_stop.load()) break;                  // stop only when the ring is empty
        /* sleep 10 ms */
    }
}

int main(int argc, char* argv[]) {
    /* option parsing: unchanged */
    g_src = ttsched_evt_open("cbs_task");                 // replaces timpani_rt_init() + timpani_evt_register(...)
    std::thread logger(logger_loop);
    std::thread(cbs_worker_loop).detach();                // start the worker BEFORE the receive path; it is never woken for shutdown (HLD-008 §8.1 item 9)
    /* vsomeip app init, request_service, register_message_handler, subscribe, app->start(): unchanged */
    g_logger_stop = true; logger.join();                  // app->start() has returned (the signal handler called app->stop())
    /* print g_overflow / g_rejected / g_short / g_log_dropped */
    return 0;                                             // the detached worker ends with the process
}
```

- **무효 표시를 쓰는 이유**: notify가 −1을 반환한 항목을 생산자가 큐에서 도로 빼려면 꼬리 인덱스를 되돌려야 하는데, 그 순간 소비자가 큐를 비우면(`clear`) 인덱스가 어긋난다. 무효 표시는 어느 쪽도 상대의 인덱스를 되돌리지 않으므로 이 경합이 없다. 워커는 계수된 Event 수(n)만큼의 **유효** 항목만 꺼내므로, notify 결과가 아직 나오지 않은 마지막 항목에는 닿지 않는다(그 항목 뒤에는 수락된 항목이 있을 수 없다: 생산자가 하나이므로). 예외는 데몬 재시작 직후의 한 번이다(§6.2 "유실·중복 없음"): 그때는 `g_short`가 한 번 오르거나 통지 전 항목 한 건이 먼저 처리될 수 있고, 그 뒤로 다시 맞는다.
- **−1과 −2**: −1 동안에는 수락된 항목을 남겨 두고 거부된(무효) 항목만 앞에서부터 치운다. 치우지 않으면 등록 전에 쌓인 무효 항목이 큐를 채워 이후의 경보가 들어가지 못한다. −2에서는 전부 버린다.
- **뮤텍스 큐를 그대로 쓰면**: 동작은 하지만 워커가 `cbs_mtx` 경합으로 잠드는 순간 그 Job이 둘로 나뉘어 측정된다.
- 생산자(`mock_radar_producer.cpp`): `#include <timpani_rt.h>`를 지우고, `timpani_rt_init()`과 `timpani_tt_register("radar_workload", "tt_task")`를 `pthread_setname_np(pthread_self(), "tt_task"); ttsched_init();`으로 바꾼다. 나머지는 그대로다.
- 제거: `libtimpani_rt`(`timpani_rt.h`, `timpani_rt_shm.h`, `libtimpani_rt.cpp`), `/timpani_rt` SHM, 라이브러리 안의 MIT 판정과 3 ms E2E 판정. 유지: `--overrun-ms`, `-be`, `--burst-ms`, `--tt-overrun-ms`. `--wakeup-delay-ms`는 수신 측으로 옮긴다.

---

## 8. 타이밍 분석 (Worst-case paths)

| 경로 | 구성 요소 | 예산(후보) | 대응 |
|:--|:--|:--|:--|
| 활성화 → 실행 개시 (Gap) | `FUTEX_WAKE` 또는 소켓 wakeup(≈1–3 µs) → `ops.runnable`+`enqueue`(≈1 µs, 맵 조회 2회) → `kick`/IPI(≈2–5 µs) → `ops.dispatch` consume → 컨텍스트 스위치(≈3–10 µs, PREEMPT_RT) | ≤ 50 µs p99.99 | TMG-001 |
| 활성화 → 실행 개시 (TT 중) | 위 + $B_{tt}$ (TT 연속 점유) | ≤ $B_{tt}$ + 50 µs | TMG-002 |
| 윈도우 끝(`replenish_at`) → 실행 가능 (스로틀된 서버, 유휴 CPU) | `pthread_cond_timedwait` 기상 지터(≈10–30 µs, TM 측정치 유사) → `bpf_prog_test_run`(≈2–5 µs) → `kick_cpu` IPI → `dispatch` promote(≤16 반복) | ≤ 100 µs p99.99 | TMG-003 |
| 예산 초과 | 슬라이스 만료 = 틱 경계(HZ=1000: ≤1 ms) + `stopping` 차감 | ≤ 1 ms (P1) | TMG-004 |
| DMISS 감지 | `quiescent`에서 완료 처리와 동시에(동기). 조기 감지(MAY)는 마감 경과 후 첫 `stopping` | 완료 시점. 조기 감지를 쓰면 마감 + 슬라이스(≤ $C_s$) | TMG-005 |
| DMISS 보고 | ringbuf → FaultMonitor 폴링(≤100 ms) → gRPC Write | ≤ +200 ms | TMG-006 |
| 감지 오버헤드 | `runnable`(≈300 ns) + `quiescent`(≈400 ns + ringbuf 제출 ≈300 ns) + `notify()` atomic 8회 이내 + 1 syscall(≈1 µs) | ≤ 2 µs/이벤트 | TMG-011 |

표에 없는 TMG 항목은 최악 경로가 아니라 시험으로 확인하는 항목이다: TMG-007($T_{bind}$, §5.2의 재스캔 주기 + 스캔 시간), TMG-009(계수 정확성, §10 행 01), TMG-010(CPU 고정, §10 행 06), TMG-012(폴트 보고 내용, §10 행 11), TMG-013(재적용, §10 행 09).

L1 영향(TMG-008): L2 변경 중 TT 경로에 추가된 코드는 `select_cpu`의 분기 1개와 `dispatch`의 `cpu_is_isolated` 검사뿐이다. kick 프로그램은 TT 슬롯이 활성인 CPU에는 kick을 보내지 않는다(§4.4). kick 직후 TT 슬롯이 시작되는 경계에서의 영향은 PoC에서 측정한다(DDR-008 D-07).

---

## 9. 오류 처리 및 실패 모드

| 상황 | 처리 |
|:--|:--|
| L2 태스크 미발견(테이블 적용 시) | 적용 실패가 아님. 미해결 목록 → 재스캔. `TableApplied.success=true`, 로그 WARN |
| `cbs_map`/`task_meta_map` 갱신 실패 | 해당 엔트리만 실패 기록(`apply_error`), 나머지 계속 |
| ringbuf 공간 부족 | `fault_drop_count++`; JobTraceCollector가 주기적으로 읽어 WARN |
| `timpani_replenish_kick` 로드/실행 실패 | 시작 시 감지 → `KickBackend::KickerThreads`로 자동 전환, 로그 ERROR |
| SHM v1 소비자(구 libttsched) | `magic` 불일치로 `-1` 반복 반환 → 앱 로그로 식별 가능. **NFR-003(TT 무회귀)의 명시적 예외**: v1로 빌드된 TT 앱은 다시 빌드해야 한다. 배포 순서: TT/L2 앱을 `libttsched` v2로 빌드 → timpani-n(v2)과 함께 교체 |
| MIT=0 (전환 규칙도 불가) | O에서 `ParamInvalid` 거부 |
| 태스크 종료 | `exit_task` 레코드 또는 재스캔 → 상태 정리, 미해결로 복귀 |
| source 등록 전·비활성, SHM 객체 없음(데몬 기동 전·정상 종료 후), SHM 쓰기 불가 상태의 `ttsched_evt_notify` | −1 반환, Event는 계수되지 않음(판정이 계수보다 먼저다). 앱은 방금 넣은 항목을 무효로 표시한다(§7.2). 등록 후에는 핸들 재획득 없이 동작. 유실 없음 보장은 수락된(0 반환) 통지에 대한 것이다 |
| 데몬이 비정상 종료한 직후(SHM 객체가 남아 있고 슬롯이 `kind = EVT`인 채) | 다음 데몬이 뜰 때까지 notify는 0을 반환하고 계수되며, wait는 −1 없이 계속 동작한다(태스크는 TIMPANI의 관리 없이 실행된다). 새 데몬이 남은 객체에 종료 절차를 수행하면 wait는 −1 → `ttsched_init()` → −2 순으로 복귀하고, 그 사이에 계수된 Event는 버려진다(보장 범위 밖) |
| TT 테이블 재발행(`magic=0` 구간) 중 `ttsched_evt_notify`/`ttsched_evt_wait` | 유지되는 source는 영향 없음: 통지는 수락되고 대기는 계속된다(§5.6 EVT 규약) |
| source 비활성화 또는 데몬 종료 중 `ttsched_evt_wait` | −1 복귀(깨움 워드만 바뀌고 `event_seq`는 그대로이므로 가짜 Event 없음). 앱은 큐를 그대로 두고 `ttsched_init()` 후 재시도한다. source가 다시 등록되면 누적 값이 이어지고 남아 있던 미소비 Event가 전달된다 |
| 데몬 종료(정상·비정상) 후 재시작 | 새 데몬이 남은 SHM 객체에 종료 절차를 수행하고 새 객체를 만든다(§5.6). 앱은 −1을 받고 `ttsched_init()`으로 새 객체에 붙으며, 소비 태스크는 그 객체에서의 첫 wait에서 `TTSCHED_EVT_RESET`(−2)을 한 번 받아 큐를 비운다. 새 객체의 계수는 0에서 시작한다 |
| L2 `task_id` 종류가 SHM 슬롯 수를 넘음 | 그 엔트리는 적용 오류(슬롯 고정 배정, §5.6). 데몬 재시작으로 해소 |
| `CbsConfig.cpu` 없음 또는 Isolated CPU 아님 | 그 엔트리는 적용 오류(§5.1 (B)) |
| 테이블 재적용 시 파라미터가 바뀐 L2 엔트리 | 제거 후 재등록(§5.1 (C)). 진행 중이던 Job의 레코드는 남지 않을 수 있다 |
| SHM을 읽기 전용으로만 매핑할 수 있는 L2 워커 | `ttsched_evt_wait`가 −1(1 ms 대기 후). 표준 블로킹(어댑터 K)으로는 동작 가능 |
| 처리 중 Event 도달 | 미소비 상태로 누적(R2), 다음 wait가 즉시 복귀(R3). `src` 레코드의 `pending`과 `n_events > 1`로 관측. 유실 없음 |
| 동일 이름 다중 매칭 | 테이블 안의 `task_id` 중복은 O가 거부한다(§3.2). 같은 comm을 가진 스레드가 노드에 여럿이면 첫 매칭 사용(현행) + WARN |
| L2 태스크의 mid-job blocking(A-06 위반) | Job이 분할 관측된다(§4.5). 예산 격리는 유지되나 응답 시간·MIT(마커 없는 source) 판정이 유효하지 않다. `evttask --midjob-block-us`로 재현 |

---

## 10. 테스트 훅 및 수용 기준 매핑

| AC-008 | 수단 | 훅 |
|:--|:--|:--|
| 01 | (a) `evttask --marker on` ×100,000 이벤트. (b) `evttask --marker off --source timerfd\|udp\|pipe` ×100,000(표준 블로킹) | (a) `src` 레코드: 최종 `event_seq` = 생성기 도달 수(누락·중복 0), 최종 `consumed_seq` = `event_seq`(소비 누락 0). MIT·$R \le D$·A-06을 지키는 구간에서 `job` 레코드 수 = 도달 수(I1). (b) 같은 조건에서 `job` 레코드 수 = 생성기 이벤트 수(생성기 로그와 오프라인 대조) |
| 02 | TT 테이블 고정 + `evttask --burst` A/B 1 h | `jobtrace.csv` tt 레코드 지터 p99.99, L1 DMISS = 0(FaultMonitor 로그) |
| 03 | `evttask --overrun-us`; `fcw_consumer -be` | `cbs_map` 샘플링(bpftool), `BUDGET_EXCEED` 수, Replenisher 로그 vs `remaining_us` 복원 시각 |
| 04 | (a) 실행 시간 초과: `evttask --overrun-us`(D 초과), FCW 재현 `fcw_consumer --overrun-ms 10`. (b) 실행 시간은 D 미만이지만 지연된 경우: `evttask --burst N`으로 스로틀을 유발하거나 D보다 긴 TT 블록과 겹치게 도달시킴 | Job마다 `DMISS` 1회(중복 0), (b)에서 `flags`에 `THROTTLED`/`CARRYOVER`, FaultMonitor 로그 타임스탬프와 O 수신 로그(≤ TMG-006) |
| 05 | `evttask --source udp` 외부 생성기(타 노드/CPU) | job 레코드 `t_start−t_act` |
| 06 | 1 h 정격 + 폭주 | `perf sched`/`sched_switch` 트레이스에서 cbs_task CPU 집합 |
| 07 | `evttask --burst N --marker on` (MIT/3); FCW 재현: `mock_radar_producer --burst-ms 6` | `MIT_VIOLATION count` = 생성기 위반 수(도달 기준), `src` 레코드의 최종 `consumed_seq` = `event_seq` = 도달 수, DMISS `flags & 0x1`, 다른 L2 태스크의 DMISS = 0 |
| 08 | GTest `FifoAdmission_*`(기존 태스크 재검사 포함), `RejectRestoresPreviousWorkload` | — |
| 09 | `mock_pullpiri.py` add/remove 10회 | 데몬 PID 불변, `bpftool map dump` 잔존 0 |
| 10 | `evttask`를 테이블 적용 후 기동 | `Registered CBS task` 로그 시각 − 기동 시각 |
| 11 | `mock_pullpiri.py` 수신 로그 | 유형/카운트 |
| 12 | `grep -rniE 'someip|vsomeip|service_id|instance_id' timpani-o timpani-n sample-apps` = 0; `evttask --source timerfd/udp/pipe/self`를 TIMPANI 설정 변경 없이 전환하여 01~07 재확인 | — |

---

## 11. 파일 변경 목록 및 구현 순서 (P1 WBS)

| 순서 | 작업 | 파일 | 의존 |
|:--|:--|:--|:--|
| O-1 | proto 필드 추가(§2) + 생성 코드 | `timpani-o/proto/*.proto`, Rust `timpani_rust/timpani-o/proto/schedinfo.proto` 동기(필드만) | 승인(C-03) |
| O-2 | MIT 분리·검증·복원 | `schedinfo_service.{h,cpp}`, `global_scheduler.h` | O-1 |
| O-3 | hyperperiod 일관화, $B_{tt}$, FIFO 승인, min-gap 거부, 2-레이어 발행, table_id | `global_scheduler.cpp`, `publication_coordinator.cpp` | O-2 |
| O-4 | 폴트 유형 보존·필드 전달 | `orchestrator_service.cpp`, `fault_client.{h,cpp}` | O-1 |
| O-5 | GTest 추가(§3.5) | `timpani-o/tests/*` | O-2~4 |
| N-1 | `maps.h`/`job_fsm.h`/BPF ops/DSQ/ringbuf/SYSCALL prog | `timpani-n/src/bpf/*` | — |
| N-2 | `libttsched` v2 + TimerMaster SHM v2 + L2 슬롯 | `sample-apps/src/libttsched.{h,c}`, `timer_master.{h,cpp}` | — |
| N-3 | TaskRegistry 재설계, main.cpp 레이어 분기·바인딩·정리 | `task_registry.{h,cpp}`, `main.cpp`, `bpf_loader.{h,cpp}` | N-1, N-2, O-1 |
| N-0 | **PoC(OI-9)**: SYSCALL 프로그램의 `scx_bpf_kick_cpu`, 컨텍스트 반환, 유휴 CPU 승격 지연, TT 지터 영향, 예산 갱신 유실 없음 | `timpani-n/src/bpf/*`(실험 브랜치) | — |
| N-4 | Replenisher + kick 백엔드 | `replenisher.{h,cpp}`(신규), `bpf_loader.*` | N-0, N-1 |
| N-5 | JobTraceCollector, EventSourceMonitor, FaultMonitor 집계, NodeClient 필드 | `job_trace.{h,cpp}`(신규), `event_source_monitor.{h,cpp}`(신규), `fault_monitor.*`, `grpc/node_client.*` | N-1, N-2, O-1 |
| N-6 | timpani-n GTest 타깃 신설(§5.8) | `timpani-n/tests/*`, `CMakeLists.txt` | N-1~5 |
| S-1 | `evttask.c`, FCW 어댑터 축소, `libtimpani_rt` 폐기, yaml/json MIT | `sample-apps/src/evttask.c`, `examples/someip/*`, `scripts/*` | N-2, O-1 |
| D-1 | 문서 정합: DDR-002 §7, DDR-005, DDR-007 §4.2/§4.5(보충 의미론)/§4.5.3/§7, DDR-012, `structure.md`, README/CHANGELOG | `doc/**` | 전체 |
| T-1 | 통합 스크립트(`scripts/verify_event_driven.sh`): AC-008-01~12 자동화 가능 항목 | `scripts/` | 전체 |

권장 병렬화: (O-1→O-2→O-3→O-5), (N-1 ∥ N-2) → N-3 → (N-4 ∥ N-5) → N-6, S-1은 N-2/O-1 이후. 각 묶음은 별도 PR(`feat(timpani-o): ...`, `feat(timpani-n): ...`, `feat(sample-apps): ...`).

---

## 12. 오픈 이슈 (LLD 수준)

| # | 이슈 | 영향 |
|:--|:--|:--|
| L-1 | `scx_bpf_kick_cpu`의 SYSCALL 프로그램 호출 가능성과 컨텍스트 반환(OI-9) — 커널 6.12.x 빌드에서 PoC. 함께 확인: 플래그 0 kick이 TT 실행 중인 CPU의 지터에 주는 영향, Replenisher 우선순위(TM−1) | §5.3 백엔드 선택 |
| L-2 | `ops.runnable`이 `sched_setattr(SCHED_EXT)` 전환 시점에 호출되는지(첫 job 측정 왜곡) — 첫 job에 `JOB_FLAG_FIRST` 추가 여부 | 통계 정확도 |
| L-3 | `trace_enabled`를 `.bss` 변수로 둘지 1-엔트리 ARRAY 맵으로 둘지(libbpf 스켈레톤 접근 편의) | 구현 편의 |
| L-4 | name→idx 캐시 무효화 규칙 — **종결**: Event source 핸들(`ttsched_evt_open`)은 슬롯 인덱스를 캐시하고, 슬롯의 `kind`·이름이 맞지 않으면 notify에서 다시 찾는다(§6.2) | — |
| L-5 | `JobTraceCollector` CSV와 기존 `gpdata` 포맷 통합 여부 — HLD-008 H-2가 담당(여기서는 추적하지 않음) | 도구 체인 |
| L-6 | Rust proto(`timpani_rust/timpani-o/proto/schedinfo.proto`)에 필드만 동기화할지, Rust 포트 작업 시 일괄 반영할지 | Rust CI |
| L-8 | `ttsched_evt_wait()`에서 대기 중인 워커를 Event 없이 깨우는 수단. P1에는 없다: 프로세스 종료 때는 워커를 깨우지 않고(HLD-008 §8.1 9항), 꼭 깨워야 하면 `ttsched_evt_notify(src, 0)`을 쓰되 도달 1건·경우에 따라 MIT 위반 1건·Job 1건으로 계수된다. 전용 취소 API(깨움 워드만 올리고 wait가 0을 반환)를 둘지는 P2에서 결정 | 워커만 끝내는 앱의 통계 |
| L-7 | 마커 wait point의 커널 식별(REQ-008 OI-17, DDR-008 F-8). 후보: `ttsched_evt_wait()`가 wait point에서 `sched_yield()`를 호출하고 BPF `ops.yield`가 Completion(및 수면 없는 복귀면 Activation)을 기록. `sched_yield()`는 앱도 부를 수 있으므로 이것만으로는 wait point를 뜻하지 않는다 — 마커 사용 플래그가 선 태스크에 한해 적용하고, 그런 태스크는 라이브러리 밖에서 yield하지 않는다는 계약이 필요하다. 채택하면 A-06 제약과 병합 구간의 거짓 DMISS가 마커 태스크에서 사라진다 | Job 경계·응답 시간 정밀도. P1 채택 여부는 PoC 후 결정(HLD-008 H-4와 같은 이슈, 이 항목이 담당) |
