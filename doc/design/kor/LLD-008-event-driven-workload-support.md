<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-008: Event-Driven Workload Support — 상세 설계 (Low-Level Design)

**날짜:** 2026-11-10
**상태:** Draft (v1, Phase 1 범위) — DDR-008(Proposal)의 D-01~D-17 확정을 전제로 함
**작성:** Human + AI
**상위 문서:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), [DDR-008](DDR-008-event-driven-workload-support.md), [HLD-008](HLD-008-event-driven-workload-support.md)
**기준 코드:** `timpani26` HEAD `4201573` (`timpani-o/`, `timpani-n/`, `sample-apps/`, `examples/someip/`)

> 본 문서는 HLD-008의 컴포넌트를 **파일·구조체·함수 수준**으로 전개한다. 코드 블록은 구현 지침용 스케치이며, 식별자·필드 번호는 본 문서를 단일 기준(single source)으로 삼는다. Phase 2 항목은 `[P2]`로 표시하고 훅만 남긴다.
> 코딩 규칙: 기존 저장소 스타일(C++17, `TLOG_*`, SPDX 헤더, 4-space)과 `.github/copilot-instructions.md`(RT 경로에 gRPC/할당 금지, proto 변경 승인) 준수.

---

## 요약 (Korean Summary)

- **proto**: `schedinfo.TaskInfo.min_inter_arrival_us=11`, `FaultType` 3종 추가, `FaultInfo` 3필드 추가; `node_control.CbsConfig.min_inter_arrival_us=9, cpu=10`, `FaultType` 2종 추가, `FaultInfo` 3필드 추가. 발행 규칙: L2는 `EVENT_DRIVEN` 레이어(`layer_index=2`).
- **timpani-o**: `ClassifiedTask.mit_us` 분리, `validate_l2_task()`, `compute_tt_blocking()`, `admit_l2_fifo()`(이용률+min-gap 거부+FIFO 블로킹 합), 전역 hyperperiod 일관화, 2-레이어 `build_table()`, 거부 시 복원, 폴트 유형 보존 매핑.
- **BPF**: `TaskMeta` 확장(job 상태·시퀀스·플래그), CPU별 DSQ(`0x100+cpu`/`0x200+cpu`), 신규 `runnable/quiescent/exit_task` 콜백, MIT 검사, `BUDGET_EXCEED` 전이 발행, ns 누산 차감, `job_trace_rb`, `timpani_replenish_kick`(SYSCALL).
- **timpani-n**: `TaskRegistry` 재설계(미해결 목록·재스캔·종료 감지), 신규 `Replenisher`, 신규 `JobTraceCollector`, `FaultMonitor` 집계 확장, `TimerMaster` SHM v2, `main.cpp` 테이블 적용 분기.
- **libttsched v2**: SHM v2 레이아웃(64 슬롯, `kind/t_arr_ns/job_seq`), `ttsched_evt_wait()`, `ttsched_evt_signal()`.
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
| 라이브러리 | FR-L-001~006, IF-L-001~003 | §6 |
| 샘플 앱 | FR-G-001, FR-L-006 | §7 |
| 타이밍 | TMG-001~013 | §8 |
| 검증 인터페이스 | IF-V-001~003 | §5.4, §10 |
| 비기능 | NFR-001(용량), NFR-002(BPF 반복 상한), NFR-003(TT 무회귀), NFR-005(proto 호환) | §4.2, §4.5, §8, §2.3 |
| 테스트 | NFR-006/007, AC-008-* | §10 |

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
//   0x1 MIT_RELATED  — the DMISS job had an MIT violation
//   0x2 THROTTLED    — the DMISS job was throttled during its response window
//   0x4 EARLY        — detected before completion (runnable past deadline)
//   0x8 MARKER       — job boundary came from libttsched marker API
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
  uint32 cpu = 10;                  // assigned isolated CPU (explicit)
}
```

**발행 규칙**(메시지 변경 없음): `PartitionConfig.layers = [ {layer_index=1, model=TIME_TRIGGERED, tt_slots=[...], cbs_entries=[]}, {layer_index=2, model=EVENT_DRIVEN, tt_slots=[], cbs_entries=[...]} ]`. 비어 있는 레이어는 생략 가능. `table_id = "table_" + generation`.

### 2.3 하위 호환 규칙
- 모든 변경은 필드 추가. 구 Pullpiri(필드 1~10)·구 timpani-n(필드 1~8)이 보낸/받은 메시지는 그대로 역직렬화된다.
- timpani-n이 `CbsConfig.cpu == 0`이고 파티션 cpuset이 있으면 `cpuset.cpus(0)`을 사용(전환 호환). O는 항상 `cpu`를 채운다.
- `mock_pullpiri.py`, `scripts/grpcurl/cbs.json`, `scripts/mock_pullpiri.yaml`에 `min_inter_arrival_us` 추가.

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
    ParamInvalid,        // NEW: 0<C≤D≤MIT violated
    GapTooSmall,         // NEW: min_gap < CBS_MIN_EXEC_US (was warning)
    DeadlineInfeasible,  // NEW: FIFO blocking condition failed
};

struct TtBlocking {          // NEW (per CPU)
    std::vector<std::pair<uint32_t,uint32_t>> merged_us;   // merged TT occupancy [start,end) sorted, one hyperperiod
    uint32_t hp_us = 0;
    uint32_t max_contig_us = 0;                            // longest merged block (wrap-aware); equals B_tt(k,L) for L ≥ that block
    uint32_t occupancy_within_us(uint32_t L) const;        // B_tt(k, L): max TT time in any window of length L (wrap-aware)
    // [P2] sbf_k(L) gap-supply function
};

// Tunables (header constants, overridable by -D at build)
static constexpr double   U_BOUND        = 0.80;
static constexpr double   U_OVERHEAD     = 0.02;
static constexpr uint32_t CBS_MIN_EXEC_US = 100;
static constexpr uint32_t L_WAKE_MAX_US  = 50;     // NEW: REQ-008 TIM-001 candidate
static constexpr uint32_t DEFAULT_HP_US  = 10000;  // CBS-only node hyperperiod
```

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
    if (t.mit_us == 0)                     return err(ParamInvalid, "MIT must be > 0");
    if (t.wcet_us == 0)                    return err(ParamInvalid, "WCET must be > 0");
    if (t.wcet_us > t.deadline_us)         return err(ParamInvalid, "WCET > deadline");
    if (t.deadline_us > t.mit_us)          return err(ParamInvalid, "deadline > MIT (arbitrary deadline unsupported in P1)");
    return std::nullopt;
}

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
    for (auto& m : b.merged_us) b.max_contig_us = std::max(b.max_contig_us, m.second - m.first);
    if (b.merged_us.size() > 1 && b.merged_us.back().second == hp_us && b.merged_us.front().first == 0)   // wrap
        b.max_contig_us = std::max(b.max_contig_us,
            (b.merged_us.back().second - b.merged_us.back().first) + (b.merged_us.front().second - b.merged_us.front().first));
    return b;
}

// B_tt(k, L): TT occupancy is a periodic step function, so the maximum over windows of fixed length L is attained
// when the window starts at a block start or ends at a block end. Evaluate both candidate sets over two hyperperiods.
uint32_t TtBlocking::occupancy_within_us(uint32_t L) const {
    if (merged_us.empty() || L == 0) return 0;
    if (L >= hp_us) return static_cast<uint32_t>(total_tt_us() * (L / hp_us)) + occupancy_within_us(L % hp_us);  // whole periods + remainder
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
    // (1) utilization (existing logic, uses mit_us)
    double u_avail = U_BOUND - u_tt - U_OVERHEAD;
    if (u_avail <= 0.0) return fail(NoCbsBandwidth);
    // (2) min gap → reject (was warning)                                     FR-O-014
    if (!gaps.empty() && min_len(gaps) < CBS_MIN_EXEC_US) return fail(GapTooSmall);
    // utilization admission in decreasing U_s order (existing)
    ...
    // (3) FIFO blocking condition for every admitted task                     FR-O-013 (P1)
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

`assign_cpus()`의 CBS worst-fit 단계에서도 후보 CPU마다 조건 (2)/(3)을 사전 검사하여 통과하는 CPU만 후보로 둔다(FR-O-014). 모든 CPU가 실패하면 `NoCbsCpu`.

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
| `SchedInfoServiceTest.RejectWcetGtDeadline` / `RejectDeadlineGtMit` / `RejectZeroMit` | FR-O-003 |
| `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | FR-O-005 |
| `GlobalSchedulerTest.TtBlockingMergesAdjacentSlots` / `TtBlockingWrapAround` / `TtBlockingWindowSpansTwoSlots` (D=15 ms, TT 2 ms @ 10 ms → B_tt=4 ms) | compute_tt_blocking, occupancy_within_us |
| `GlobalSchedulerTest.FifoAdmission_FcwConfigPasses` (C=2000, B_tt=2000, Lw=50, D=5000) | FR-O-013 |
| `GlobalSchedulerTest.FifoAdmission_SecondL2Rejected` (C=2000×2, B_tt=2000, D=5000 → 6050 > 5000) | FR-O-013 |
| `GlobalSchedulerTest.MinGapRejects` | FR-O-014 |
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

/* job_flags / FaultEvent.flags / JobRecord.flags — identical bit meanings to proto FaultInfo.flags */
#define JOB_FLAG_MIT_VIOLATION (1u << 0)
#define JOB_FLAG_THROTTLED     (1u << 1)
#define JOB_FLAG_EARLY_DMISS   (1u << 2)
#define JOB_FLAG_MARKER        (1u << 3)   /* set by JobTraceCollector (userspace) when the task's SHM slot is kind==EVT; never set in BPF */
#define JOB_FLAG_DMISS         (1u << 4)
#define JOB_FLAG_TASK_EXIT     (1u << 5)   /* JobRecord only: task exited */

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
    __u64 replenish_at_ns;
    /* ---- 008 additions ---- */
    __u32 exec_frac_ns;       /* sub-µs carry between stops (0..999) */
    __u32 throttle_episodes;  /* incremented on remaining 1→0 transition */
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

struct ReplenishKickArgs {       /* input of timpani_replenish_kick (SYSCALL prog) */
    __u64 cbs_key;
    __u32 cpu;
    __u32 _pad;
};
```

> `TaskMeta`/`CbsState` 레이아웃 변경은 `timpani-n` 사용자 공간(같은 헤더)과 함께 빌드되므로 추가 호환 조치는 없다. TT 경로 필드 의미는 불변(NFR-003).
> 용량(NFR-001): `task_meta_map` 4096, `cbs_map` 1024, SHM 64 슬롯(TT+L2 합) → 노드당 L2 ≥ 32, CPU당 ≥ 8 충족. CPU별 DSQ는 길이 제한이 없다.

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

/* L2 DSQs are used as priority queues (vtime = activation_ns): insert only with scx_bpf_dispatch_vtime();
 * mixing FIFO and PRIQ insertion on one DSQ is rejected by the kernel. */

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

static __always_inline void job_fsm_on_runnable(struct TaskMeta *m, __u64 now, struct FsmEvent *ev)
{
    ev->emit_fault = 0; ev->emit_record = 0;
    if (m->job_state != JOB_WAITING) return;                 /* mid-job wakeup: ignore (FR-B-002) */
    m->job_state = JOB_ACTIVE;
    m->activation_ns = now; m->t_start_ns = 0; m->budget_used_us = 0; m->job_flags = 0; m->job_seq++;
    if (m->last_act_ns && m->mit_us) {
        __u64 mit_ns = (__u64)m->mit_us * 1000ULL;
        if (now - m->last_act_ns < mit_ns) {                  /* MIT violation (FR-B-006) */
            m->job_flags |= JOB_FLAG_MIT_VIOLATION; m->mit_viol_count++;
            ev->emit_fault = 1; ev->fault_type = FAULT_MIT_VIOLATION;
            ev->expected_ns = m->last_act_ns + mit_ns; ev->actual_ns = now;
        }
    }
    m->last_act_ns = now;
}

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
    m->job_state = JOB_WAITING;                                /* P1 rule (b): any sleep = completion */
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
    job_fsm_on_runnable(m, now, &ev);
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
        cbs_lazy_replenish(c, bpf_ktime_get_ns());                           /* unchanged helper */
        __u64 flags = cbs_enqueue_flags(enq_flags);
        /* PRIQ keyed by t_act: FIFO-by-activation (D-08, FR-B-016) survives throttle → promote */
        if (c->remaining_us == 0) { m->job_flags |= JOB_FLAG_THROTTLED; scx_bpf_dispatch_vtime(p, dsq_throttled(cpu), SCX_SLICE_DFL, m->activation_ns, flags); }
        else                        scx_bpf_dispatch_vtime(p, dsq_cbs(cpu),       SCX_SLICE_DFL, m->activation_ns, flags);
        return;
    }
    scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
}

static __always_inline void promote_throttled_if_replenished(__u32 cpu, __u64 now)
{
    /* bpf_for_each(scx_dsq, p, dsq_throttled(cpu), 0), ≤ PROMOTE_BUDGET iterations: lazy-replenish the task's CbsState;
     * if remaining_us > 0 → scx_bpf_dispatch_vtime_from_dsq(&it, p, dsq_cbs(cpu), meta->activation_ns, 0).
     * vtime = t_act keeps FIFO-by-activation across the throttle episode (FR-B-016). No kick: we are on cpu. */
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
    __u64 now = bpf_ktime_get_ns();
    if (m->scheduling_type != SCHED_TYPE_CBS) return;
    job_fsm_on_running(m, now);
    /* slice cap: unchanged (min(remaining, us_until_next_tt), ≥1µs); exec_start_ns = now */
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
                if (c->period_us) {
                    __u64 pn = (__u64)c->period_us * 1000ULL;
                    if (c->replenish_at_ns == 0) c->replenish_at_ns = now + pn;
                    else if (now >= c->replenish_at_ns) c->replenish_at_ns += ((now - c->replenish_at_ns) / pn + 1) * pn;
                }
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

/* userspace-triggered replenish + kick (DDR-008 D-07 option 1) */
SEC("syscall")
int timpani_replenish_kick(struct ReplenishKickArgs *args)
{
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &args->cbs_key);
    if (c) cbs_lazy_replenish(c, bpf_ktime_get_ns());
    scx_bpf_kick_cpu((s32)args->cpu, SCX_KICK_IDLE);
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

`emit_fault()`는 `bpf_ringbuf_reserve` 실패 시 `fault_drop_count++`(FR-B-023). `emit_job_record()`는 `JobRecord{..., t_act=activation_ns, t_start=t_start_ns, t_done=now, budget_used_us, flags=job_flags}`를 제출한다.

### 4.5 검증기(verifier)·호환 메모
- `ops.runnable`/`ops.quiescent`/`ops.exit_task`는 sched_ext 6.12 struct_ops 멤버로 존재한다. `SCX_DEQ_SLEEP`은 vmlinux.h enum.
- `select_cpu`는 `nr_cpus_allowed == 1`인 태스크에 대해 호출되지 않을 수 있으므로 활성화 기록을 `runnable`로 옮긴 것이 정확성에도 필요하다.
- `promote_throttled_if_replenished` 반복 상한 `PROMOTE_BUDGET=16` 유지(SRS NFR-002).
- `timpani_replenish_kick`의 `scx_bpf_kick_cpu` 호출 가능 여부는 커널 kfunc 등록 집합에 의존 → **OI-9 PoC**. 불가 시 §5.3 `KickBackend::KickerThreads`.
- `job_fsm.h`는 `__always_inline`·고정폭 타입만 사용하며 `bpf_helpers.h` 미포함 → 호스트 컴파일 가능.

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
uint32_t cpu = e.cpu() != 0 || !has_partition_cpu ? e.cpu() : partition_cpu;     // explicit cpu (fallback)
uint32_t mit = e.min_inter_arrival_us() ? e.min_inter_arrival_us() : e.period_us();
evt_tasks.push_back({e.task_id(), e.task_id_hash(), e.workload_id_hash()});      // → TimerMaster SHM slot (kind=EVT)
servers.push_back({key(e), cpu, (uint64_t)e.period_us() * 1000ULL});             // → Replenisher
fault_monitor.set_task_params(key(e), e.period_us(), e.current_limit());          // MIT window + threshold
auto r = registry.resolve_now(e.task_id());                                        // std::optional<TaskRegistry::Resolved>
if (!r) { registry.add_unresolved(e.task_id(), make_l2_binding(e, cpu, mit)); continue; }   // FR-N-003 (not an apply error)
bind_l2_task(r->tid, e, cpu, mit); registry.track(e.task_id(), r->tid, make_l2_binding(e, cpu, mit));

// (C) bind_l2_task(): called at apply time or later by TaskRegistry callback
TaskMeta meta{}; meta.scheduling_type = SCHED_TYPE_CBS; meta.assigned_cpu = cpu; meta.mit_us = mit; meta.job_state = JOB_WAITING; ...
bpf_loader.update_task_meta(tid, meta); apply_sched_ext(tid, kind);
CbsState st{};
if (bpf_loader.lookup_cbs_state(key, &old) && same_params(old, e)) st = old;    // D-17: preserve remaining/replenish_at
else { st.budget_us = e.budget_us(); st.period_us = e.period_us(); st.remaining_us = e.budget_us();
       st.replenish_at_ns = mono_now() + period_ns; st.deadline_us = e.deadline_us(); }
bpf_loader.update_cbs_state(key, st);
track(workload_id → tid, key);

// (D) after loops
if (timer_master.same_tt_table(slots, hp, epoch)) timer_master.update_evt_slots(evt_tasks);   // FR-N-006 / D-17(b): L2-only change → SHM republish, no thread restart
else                                             timer_master.set_schedule_table(slots, evt_tasks, hp, epoch);
replenisher.set_servers(servers);
registry.start_rescan_if_pending();
job_trace.set_task_names(hash → name);                     // for CSV output
node_client.send_table_applied(table.table_id(), apply_success, apply_error);
```

제거/STOP 경로(`RecoverySignal`, 테이블 소멸)는 기존 처리에 더해 `replenisher.remove(key)`, `fault_monitor.clear_task(key)`, `job_trace.clear_task(key)`, `registry.remove_expected(name)`, `sched_setattr(tid, SCHED_OTHER)`을 수행한다(FR-N-005).

### 5.2 `task_registry.{h,cpp}` — 재설계

```cpp
class TaskRegistry {
public:
    struct Resolved { pid_t tid; bool is_thread; };
    using OnResolved = std::function<void(const std::string& name, Resolved r)>;
    using OnExited   = std::function<void(const std::string& name, pid_t tid)>;

    explicit TaskRegistry(std::chrono::milliseconds rescan_period = 500ms);
    std::optional<Resolved> resolve_now(const std::string& comm);          // process first, then thread (PR #51 logic moved here)
    void add_unresolved(const std::string& comm, std::function<void(Resolved)> binder);
    void remove_expected(const std::string& comm);
    void track(const std::string& comm, pid_t tid, std::function<void(Resolved)> binder);   // bound tasks, for exit detection + re-bind
    void notify_exit(pid_t tid);                                           // from JobTraceCollector (JOB_FLAG_TASK_EXIT)
    void set_on_exited(OnExited cb);
    void start(); void stop();                                             // rescan thread (default policy, non-isolated)
private:
    void rescan_loop();      // while unresolved non-empty or tracked non-empty: scan /proc every period
    std::map<std::string, std::function<void(Resolved)>> unresolved_;
    std::map<pid_t, std::pair<std::string, std::function<void(Resolved)>>> tracked_;   // tid → (name, binder)
    std::mutex mu_; std::thread th_; std::atomic<bool> running_;
};
```

`rescan_loop()`: (1) `unresolved_` 각 이름에 대해 `resolve_now()`; 성공 시 binder 실행 후 `tracked_`로 이동, (2) `tracked_` 각 TID에 대해 `/proc/<tid>` 존재 확인; 없으면 `on_exited` 호출 후 (name, binder)를 `unresolved_`로 되돌린다(FR-N-004; `notify_exit()`도 같은 경로), (3) 두 집합이 모두 비면 다음 테이블까지 휴면. `T_bind` ≤ 주기 + 스캔 시간(≈ 수 ms) ≤ 1 s.

### 5.3 `replenisher.{h,cpp}` — 신규

```cpp
class Replenisher {
public:
    struct Server { uint64_t cbs_key; uint32_t cpu; uint64_t period_ns; uint64_t next_ns; };
    enum class KickBackend { BpfSyscallProg, KickerThreads };
    Replenisher(BpfLoader& bpf, KickBackend backend, int fifo_prio = 98);
    void set_servers(std::vector<Server> servers);      // rebuild heap (non-RT context; allocation allowed here only)
    void remove(uint64_t cbs_key);
    void start(); void stop();
private:
    void loop();   // SCHED_FIFO; pinned per config (non-isolated CPU by default)
    std::mutex mu_; std::condition_variable cv_; bool table_changed_ = false;   // set_servers()/remove() lock mu_, set table_changed_, notify cv_
    void kick(const Server& s);
    std::priority_queue<Server, std::vector<Server>, ByNext> heap_;      // sized at set_servers(); no growth in loop
    ...
};

void Replenisher::loop() {
    while (running_) {
        {   std::unique_lock lk(mu_);                      // uncontended except during table change
            cv_.wait(lk, [&]{ return !running_ || table_changed_ || !heap_.empty(); });   // empty heap (no L2 servers) → sleep, never top() on empty
            if (!running_) break;
            if (table_changed_) { rebuild_heap_locked(); table_changed_ = false; continue; }
        }
        Server s = heap_.top(); heap_.pop();
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &to_ts(s.next_ns), nullptr);   // interrupted early via pthread_kill(SIGUSR2) on table change
        if (table_changed_) continue;                     // heap will be rebuilt at loop top
        kick(s);                                          // idempotent replenish + CPU kick (≤ 100 µs target)
        CbsState st; if (bpf_.lookup_cbs_state(s.cbs_key, &st) && st.replenish_at_ns > s.next_ns) s.next_ns = st.replenish_at_ns;
        else s.next_ns += s.period_ns;
        heap_.push(s);
    }
}

void Replenisher::kick(const Server& s) {
    if (backend_ == KickBackend::BpfSyscallProg) {
        ReplenishKickArgs a{ s.cbs_key, s.cpu, 0 };
        bpf_.run_replenish_kick(a);                       // bpf_prog_test_run_opts(prog_fd, ctx_in=&a)
    } else {
        bpf_.apply_replenish_userspace(s.cbs_key, mono_now());   // same rule as cbs_lazy_replenish
        kickers_[s.cpu]->poke();                          // futex wake of per-CPU SCHED_FIFO 97 thread pinned to s.cpu
    }
}
```

`BpfLoader` 추가 API: `bool lookup_cbs_state(uint64_t, CbsState*)`, `int run_replenish_kick(const ReplenishKickArgs&)`, `bool apply_replenish_userspace(uint64_t key, uint64_t now)`, `int get_job_trace_rb_fd()`, `bool set_trace_enabled(bool)`(bss 맵 쓰기), `bool lookup_task_meta(uint32_t, TaskMeta*)`.

### 5.4 `job_trace.{h,cpp}` — 신규 `JobTraceCollector`

```cpp
class JobTraceCollector {
public:
    struct Hist { std::array<uint32_t, 1024> bins_10us{}; uint32_t overflow = 0; uint64_t count = 0; uint64_t sum_ns = 0; uint64_t max_ns = 0;
                  uint64_t percentile_ns(double p) const; };
    struct TaskStats { uint64_t activations, completions, dmiss, early_dmiss, budget_episodes, mit_violations, marker_jobs;
                       Hist response, wake; uint64_t last_budget_used_us; };
    JobTraceCollector(BpfLoader& bpf, const std::string& node_id);
    void set_task_names(std::map<uint64_t, std::string> hash_to_name);
    void set_shm(volatile struct timpani_ttsched_shm* shm, std::map<uint64_t,int> hash_to_slot);  // t_arr/job_seq merge
    void set_enabled(bool);                                   // writes trace_enabled; opens/closes CSV sink
    void set_on_task_exit(std::function<void(uint64_t task_hash)>);
    void add_tt_jitter_sample(uint32_t cpu, uint32_t slot_idx, uint64_t target_ns, uint64_t actual_ns);   // from TimerMaster
    const TaskStats& stats(uint64_t key) const;
    void clear_task(uint64_t key);
    void start(); void stop();
private:
    static int rb_cb(void* ctx, void* data, size_t len);   // JobRecord → stats + CSV line
    // CSV: "job,<node>,<wl_hash>,<task>,<job_seq>,<t_arr>,<t_act>,<t_start>,<t_done>,<cpu>,<budget_used_us>,<flags>"
    //      "tt,<node>,<cpu>,<slot_idx>,<target_ns>,<actual_ns>,<jitter_ns>"
};
```

- 응답 시간 히스토그램: `t_done − t_act`, 깨움 지연: `t_start − t_act`(`t_start==0`이면 제외). 10 µs 빈 × 1024(=10.24 ms) + overflow.
- `t_arr`/`marker`: 태스크의 SHM 슬롯이 `kind==EVT`이면 마커 사용 태스크로 보고 `flags |= JOB_FLAG_MARKER`(IF-V-001 `marker_used`)를 설정하고, 슬롯의 `t_arr_ns`(가장 최근 signal의 도달 시각)를 병합한다. 한 job이 여러 signal을 처리(코얼레싱)했는지는 SHM `job_seq` 증가량 − BPF `job_seq` 증가량으로 판별해 레코드에 남긴다.
- `JOB_FLAG_TASK_EXIT` 레코드 → `on_task_exit` → `TaskRegistry::notify_exit`.
- 출력 파일: `<node_id>.jobtrace.csv`(기본 비활성; `-t` CLI 또는 `SIGUSR1`로 토글).

### 5.5 `fault_monitor.{h,cpp}` — 집계 확장

```cpp
void set_task_params(Key key, uint32_t period_us, uint32_t current_limit);   // replaces update_current_limits for L2
struct MitWindow { uint64_t window_start_ns; uint32_t count; };
std::map<Key, MitWindow> mit_windows_;
std::map<Key, uint64_t> last_budget_episode_replenish_at_;   // dedupe BUDGET_EXCEED by replenish_at
using FaultCallback = std::function<void(const FaultEvent&, uint32_t count, uint32_t dmiss_count)>;

// BPF fault_type → node_control.FaultType: explicit table, never an integer cast (numbers differ; DDR-008 D-10)
static node::v1::FaultType to_proto_fault(uint8_t bpf_type) {
    switch (bpf_type) {
    case FAULT_DMISS:                 return node::v1::DMISS;                  // 0 → 1
    case FAULT_BUDGET_EXCEED:         return node::v1::BUDGET_EXCEED;          // 1 → 2
    case FAULT_MIT_VIOLATION:         return node::v1::MIT_VIOLATION;          // 2 → 4
    case FAULT_WAKEUP_LATENCY_EXCEED: return node::v1::WAKEUP_LATENCY_EXCEED;  // 3 → 5
    case FAULT_WATCHDOG:              return node::v1::WATCHDOG;               // 4 → 3
    default:                          return node::v1::UNKNOWN;
    }
}

// ring_buf_callback():
switch (ev.fault_type) {
case FAULT_DMISS:          dmiss_count++; if (dmiss_count > limit && !reported) callback(ev, 1, dmiss_count);  // unchanged rule + ev.flags passthrough
case FAULT_BUDGET_EXCEED:  if (last_episode[key] != ev.expected_deadline_ns) { last_episode[key] = ...; callback(ev, 1, 0); }
case FAULT_MIT_VIOLATION:  w = mit_windows_[key]; if (now - w.start ≥ period_ns) { if (w.count) flush(key); w = {now, 0}; } w.count++;
                           // flush on window roll-over or on poll tick if window elapsed (≤ 1 report per T_s)
}
```

`NodeClient::send_fault(...)`는 `type = to_proto_fault(ev.fault_type)`, `task_id`(hash→name), `node_id`, `timestamp_ns`(REALTIME), `count`, `flags`, `job_seq`를 채운다(FR-N-015).

### 5.6 `timer_master.{h,cpp}` — SHM v2 및 L2 슬롯

```cpp
struct EvtTaskEntry { std::string task_name; uint64_t task_id_hash; uint64_t workload_id_hash; };
void set_schedule_table(const std::vector<SlotEntry>& slots, const std::vector<EvtTaskEntry>& evt_tasks,
                        uint64_t hyperperiod_us, uint64_t epoch_ns);
```
- `bool same_tt_table(slots, hp, epoch) const`: 현재 적용된 TT 슬롯 집합(cpu/offset/duration/task_hash)·hyperperiod·epoch와 동일하면 true. `void update_evt_slots(evt_tasks)`: per-CPU 스레드를 유지한 채 SHM만 재발행(아래 publish 순서; TT 슬롯 인덱스 보존). FR-N-006 / D-17(b).
- 슬롯 할당 2-pass(기존 유지) 대상에 `evt_tasks`를 포함하고 `kind = TTSCHED_KIND_EVT`로 기록. EVT 슬롯의 `counter/t_arr_ns/job_seq`는 TimerMaster가 쓰지 않는다(publish 시 초기화만).
- publish 순서: `magic=0` → 전 슬롯 wake → 슬롯 재배치 → `n_tasks`(release) → `generation++`(release) → `version=2`, `magic=MAGIC_V2`(release).
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
| `TaskRegistryTest.ResolvesLateThread` (fork+prctl 후 ≤1 s) / `DetectsExit` | FR-N-003/004 |
| `ReplenisherTest.HeapOrder` / `RebuildOnTableChange` (kick 백엔드 mock) | FR-N-010 |
| `FaultMonitorTest.BudgetExceedOncePerEpisode` / `MitWindowAggregation` / `DmissThresholdUnchanged` | FR-N-012~014 |
| `JobTraceTest.HistogramPercentiles` / `MergesShmArrival` | FR-N-020~022 |
| `ShmV2Test.EvtSlotsAllocatedStable` / `MagicVersionMismatchReturnsMinusOne` | IF-L-001/002 |

---

## 6. `libttsched` v2 (`sample-apps/src/libttsched.{h,c}`)

> SRS OI-14 종결: 별도 라이브러리 없이 `libttsched`를 확장한다(DDR-008 D-03).

### 6.1 헤더

```c
#define TIMPANI_TTSCHED_MAGIC   0x54494D51U   /* "TIMQ" — v2; v1 "TIMP" readers get -1 (ABI break by design) */
#define TIMPANI_TTSCHED_VERSION 2U
#define TIMPANI_MAX_TASKS       64
#define TTSCHED_KIND_TT  0U
#define TTSCHED_KIND_EVT 1U
#define TTSCHED_EVT_BITSET 0x00455654U        /* "EVT": FUTEX_WAIT_BITSET mask marking an L2 wait point (P2 classifier); distinct from both magics */

struct timpani_task_slot {            /* 48 bytes */
    char     name[16];
    uint32_t counter;                 /* futex word: TT → TimerMaster; EVT → ttsched_evt_signal() */
    uint32_t kind;                    /* TTSCHED_KIND_* */
    uint64_t t_arr_ns;                /* EVT: last arrival timestamp written by signal() (CLOCK_MONOTONIC) */
    uint32_t job_seq;                 /* EVT: incremented per signal() */
    uint32_t _pad;
    uint64_t _reserved;
};

struct timpani_ttsched_shm {          /* 32 + 64*48 = 3104 bytes */
    uint32_t magic; uint32_t version; uint32_t n_tasks; uint32_t generation;
    uint64_t _reserved[2];
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
};

void ttsched_init(void);                         /* unchanged semantics; maps RDWR if permitted, else RDONLY (wait-only) */
int  ttsched_wait_next_period(void);             /* L1, unchanged */
int  ttsched_evt_wait(void);                     /* L2 wait point: FUTEX_WAIT_BITSET on own slot, mask TTSCHED_EVT_BITSET */
int  ttsched_evt_signal(const char *task_name, uint64_t t_arr_ns);   /* wake task; t_arr_ns==0 → now; returns 0 / -1 (slot not found / shm not ready) */
```

### 6.2 구현 요지

```c
static __thread uint32_t g_evt_last_seen;   /* counter value consumed by the last returned wait */
static __thread int      g_evt_primed;      /* 0 until the first wait (or after re-lookup): consume the current counter */

int ttsched_evt_wait(void) {
    /* same shm/magic/generation/slot-lookup preamble as ttsched_wait_next_period(); on re-lookup set g_evt_primed = 0 */
    volatile uint32_t *ctr = &g_shm->tasks[g_task_idx].counter;
    if (!g_evt_primed) { g_evt_last_seen = __atomic_load_n(ctr, __ATOMIC_ACQUIRE); g_evt_primed = 1; }
    /* lost-wakeup safe: a signal that arrived while the previous job was running already changed *ctr → return at once */
    while (__atomic_load_n(ctr, __ATOMIC_ACQUIRE) == g_evt_last_seen) {
        long r = syscall(SYS_futex, ctr, FUTEX_WAIT_BITSET, g_evt_last_seen, NULL, NULL, TTSCHED_EVT_BITSET);
        if (r == -1 && errno != EAGAIN && errno != EINTR) break;
        if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) { /* shutdown/republish */ unmap; g_evt_primed = 0; return -1; }
    }
    g_evt_last_seen = __atomic_load_n(ctr, __ATOMIC_ACQUIRE);   /* several signals during one job coalesce into one wakeup; the app drains its queue */
    return 0;
}

int ttsched_evt_signal(const char *task_name, uint64_t t_arr_ns) {
    volatile struct timpani_ttsched_shm *shm = signal_shm_rw();          /* lazily mapped O_RDWR, cached per process */
    if (!shm || shm->magic != TIMPANI_TTSCHED_MAGIC) return -1;
    int idx = find_task_slot(shm, task_name);                            /* small cache: name → idx, invalidated on generation change */
    if (idx < 0 || shm->tasks[idx].kind != TTSCHED_KIND_EVT) return -1;
    if (t_arr_ns == 0) t_arr_ns = mono_now_ns();
    __atomic_store_n(&shm->tasks[idx].t_arr_ns, t_arr_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&shm->tasks[idx].job_seq, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&shm->tasks[idx].counter, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &shm->tasks[idx].counter, FUTEX_WAKE, 1, NULL, NULL, 0);   /* FUTEX_WAKE matches any bitset */
    return 0;
}
```

- `FUTEX_WAKE`(mask = MATCH_ANY)는 `FUTEX_WAIT_BITSET` 대기자를 깨우므로 TimerMaster의 종료·재발행 wake(`FUTEX_WAKE INT_MAX`)도 L2 대기자를 깨운다(FR-L-005).
- SHM 권한: 현행 `0666` 유지. `signal()`은 쓰기 매핑이 필요하므로 어댑터 프로세스는 쓰기 권한이 있어야 한다(OI-15: P1은 동일 프로세스).
- 비용: `signal()` = 3 atomic + 1 syscall(≈ 1–2 µs). `wait()`는 블로킹.

---

## 7. 샘플 앱

### 7.1 `sample-apps/src/evttask.c` (신규, 전송 계층 중립 L2 생성기)

```text
evttask --name cbs_task --source {timerfd|udp:<port>|pipe|self} --mit-us 18000 --wcet-us 1500
        [--burst N] [--overrun-us U] [--midjob-block-us B] [--marker on|off] [--count K]

threads:
  worker (comm = --name): ttsched_init();
     loop: wait = (--marker on) ? ttsched_evt_wait() : source_blocking_wait();     // epoll_wait/read
           run_nsqrt_for(wcet_us ± overrun); if (midjob_block_us) nanosleep(midjob_block_us) [reproduces A-06 violation];
  source  (not an L2 task): generates events at MIT (or MIT/3 for --burst N) → write(udp/pipe) or timerfd or
           ttsched_evt_signal(name, now) (for --source self with --marker on; for other sources with --marker on, the
           source thread calls signal() right after the write so t_arr is recorded)
```
`cbstask.c`는 유지하되 README에서 `evttask`를 권장 생성기로 표기한다.

### 7.2 FCW 어댑터 축소 (`examples/someip/radar_based_fcw/fcw_consumer.cpp`)

```cpp
// vsomeip handler (any middleware thread) — the only TIMPANI touch point
void on_obstacle_alert_received(const std::shared_ptr<vsomeip::message>& m) {
    uint64_t t_arr = mono_now_ns();
    alerts.push({parse(m), t_arr});                       // app queue (lock-free or mutex outside worker's job)
    ttsched_evt_signal("cbs_task", t_arr);                // D-03 marker; no service/instance/event IDs
}
// worker
void* cbs_worker(void*) {
    pthread_setname_np(pthread_self(), "cbs_task"); ttsched_init();
    for (;;) {
        if (ttsched_evt_wait() < 0) continue;             // wait point (P1 rule a)
        while (auto a = alerts.try_pop()) process(*a);    // one job may drain several alerts; a signal during the drain bumps the counter, so the next evt_wait() returns at once (no lost wakeup)
    }
}
```
제거: `timpani_rt_init/evt_register/evt_mark_arrival/cbs_bind_thread/cbs_unbind_thread`, `/timpani_rt` SHM, 3 ms E2E 판정. 유지: `--overrun-ms`, `-be`, `--wakeup-delay-ms`(이상 주입). README의 "보고 위치" 열은 timpani-n 폴트/트레이스로 갱신.

---

## 8. 타이밍 분석 (Worst-case paths)

| 경로 | 구성 요소 | 예산(후보) | 대응 |
|:--|:--|:--|:--|
| 활성화 → 실행 개시 (Gap) | `FUTEX_WAKE` 또는 소켓 wakeup(≈1–3 µs) → `ops.runnable`+`enqueue`(≈1 µs, 맵 조회 2회) → `kick`/IPI(≈2–5 µs) → `ops.dispatch` consume → 컨텍스트 스위치(≈3–10 µs, PREEMPT_RT) | ≤ 50 µs p99.99 | TMG-001 |
| 활성화 → 실행 개시 (TT 중) | 위 + $B_{tt}$ (TT 연속 점유) | ≤ $B_{tt}$ + 50 µs | TMG-002 |
| 보충 시각 → 실행 가능 | `clock_nanosleep` 기상 지터(≈10–30 µs, TM 측정치 유사) → `bpf_prog_test_run`(≈2–5 µs) → `kick_cpu` IPI → `dispatch` promote(≤16 반복) | ≤ 100 µs p99.99 | TMG-003 |
| 예산 초과 | 슬라이스 만료 = 틱 경계(HZ=1000: ≤1 ms) + `stopping` 차감 | ≤ 1 ms (P1) | TMG-004 |
| DMISS 감지 | `quiescent` 즉시(완료) 또는 다음 `stopping`(조기, ≤ 슬라이스 ≤ $C_s$) | ≤ 마감 + $C_s$ | TMG-005 |
| DMISS 보고 | ringbuf → FaultMonitor 폴링(≤100 ms) → gRPC Write | ≤ +200 ms | TMG-006 |
| 감지 오버헤드 | `runnable`(≈300 ns) + `quiescent`(≈400 ns + ringbuf 제출 ≈300 ns) + `signal()` 1 syscall(≈1 µs) | ≤ 2 µs/이벤트 | TMG-011 |

L1 영향(TMG-008): L2 변경 중 TT 경로에 추가된 코드는 `select_cpu`의 분기 1개와 `dispatch`의 `cpu_is_isolated` 검사뿐이다.

---

## 9. 오류 처리 및 실패 모드

| 상황 | 처리 |
|:--|:--|
| L2 태스크 미발견(테이블 적용 시) | 적용 실패가 아님. 미해결 목록 → 재스캔. `TableApplied.success=true`, 로그 WARN |
| `cbs_map`/`task_meta_map` 갱신 실패 | 해당 엔트리만 실패 기록(`apply_error`), 나머지 계속 |
| ringbuf 공간 부족 | `fault_drop_count++`; JobTraceCollector가 주기적으로 읽어 WARN |
| `timpani_replenish_kick` 로드/실행 실패 | 시작 시 감지 → `KickBackend::KickerThreads`로 자동 전환, 로그 ERROR |
| SHM v1 소비자(구 libttsched) | `magic` 불일치로 `-1` 반복 반환 → 앱 로그로 식별 가능 |
| MIT=0 (전환 규칙도 불가) | O에서 `ParamInvalid` 거부 |
| 태스크 종료 | `exit_task` 레코드 또는 재스캔 → 상태 정리, 미해결로 복귀 |
| 동일 이름 다중 매칭 | 첫 매칭 사용(현행) + WARN. 유일성은 앱 계약(HLD §8.1) |

---

## 10. 테스트 훅 및 수용 기준 매핑

| AC-008 | 수단 | 훅 |
|:--|:--|:--|
| 01 | `evttask --marker on/off` ×100,000 이벤트; `fcw_consumer` | `jobtrace.csv` job 레코드 vs 생성기 로그(`job_seq`) |
| 02 | TT 테이블 고정 + `evttask --burst` A/B 1 h | `jobtrace.csv` tt 레코드 지터 p99.99 |
| 03 | `evttask --overrun-us`; `fcw_consumer -be` | `cbs_map` 샘플링(bpftool), `BUDGET_EXCEED` 수, Replenisher 로그 vs `remaining_us` 복원 시각 |
| 04 | `fcw_consumer --overrun-ms 10` | FaultMonitor 로그 타임스탬프, O 수신 로그 |
| 05 | `evttask --source udp` 외부 생성기(타 노드/CPU) | job 레코드 `t_start−t_act` |
| 06 | 1 h 정격 + 폭주 | `perf sched`/`sched_switch` 트레이스에서 cbs_task CPU 집합 |
| 07 | `mock_radar_producer --burst-ms 6` (≈ MIT/3) | `MIT_VIOLATION count`, DMISS `flags & 0x1` |
| 08 | GTest `FifoAdmission_*`, `RejectRestoresPreviousWorkload` | — |
| 09 | `mock_pullpiri.py` add/remove 10회 | 데몬 PID 불변, `bpftool map dump` 잔존 0 |
| 10 | `evttask`를 테이블 적용 후 기동 | `Registered CBS task` 로그 시각 − 기동 시각 |
| 11 | `mock_pullpiri.py` 수신 로그 | 유형/카운트 |
| 12 | `grep -rniE 'someip|vsomeip|service_id|instance_id' timpani-o timpani-n sample-apps` = 0 | — |

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
| N-4 | Replenisher + kick 백엔드(PoC 포함) | `replenisher.{h,cpp}`(신규), `bpf_loader.*` | N-1 |
| N-5 | JobTraceCollector, FaultMonitor 집계, NodeClient 필드 | `job_trace.{h,cpp}`(신규), `fault_monitor.*`, `grpc/node_client.*` | N-1, O-1 |
| N-6 | timpani-n GTest 타깃 신설(§5.8) | `timpani-n/tests/*`, `CMakeLists.txt` | N-1~5 |
| S-1 | `evttask.c`, FCW 어댑터 축소, `libtimpani_rt` 폐기, yaml/json MIT | `sample-apps/src/evttask.c`, `examples/someip/*`, `scripts/*` | N-2, O-1 |
| D-1 | 문서 정합: DDR-002 §7, DDR-005, DDR-007 §4.2/§4.5.3/§7, DDR-012, `structure.md`, README/CHANGELOG | `doc/**` | 전체 |
| T-1 | 통합 스크립트(`scripts/verify_event_driven.sh`): AC-008-01~12 자동화 가능 항목 | `scripts/` | 전체 |

권장 병렬화: (O-1→O-2→O-3→O-5), (N-1 ∥ N-2) → N-3 → (N-4 ∥ N-5) → N-6, S-1은 N-2/O-1 이후. 각 묶음은 별도 PR(`feat(timpani-o): ...`, `feat(timpani-n): ...`, `feat(sample-apps): ...`).

---

## 12. 오픈 이슈 (LLD 수준)

| # | 이슈 | 영향 |
|:--|:--|:--|
| L-1 | `scx_bpf_kick_cpu`의 SYSCALL 프로그램 호출 가능성(OI-9) — 커널 6.12.x 빌드에서 PoC | §5.3 백엔드 선택 |
| L-2 | `ops.runnable`이 `sched_setattr(SCHED_EXT)` 전환 시점에 호출되는지(첫 job 측정 왜곡) — 첫 job에 `JOB_FLAG_FIRST` 추가 여부 | 통계 정확도 |
| L-3 | `trace_enabled`를 `.bss` 변수로 둘지 1-엔트리 ARRAY 맵으로 둘지(libbpf 스켈레톤 접근 편의) | 구현 편의 |
| L-4 | `ttsched_evt_signal`의 name→idx 캐시 무효화 규칙(generation 변경 감지) | 어댑터 지연 |
| L-5 | `JobTraceCollector` CSV와 기존 `gpdata` 포맷 통합 여부 — HLD-008 H-2와 동일 이슈, H-2로 통합 | 도구 체인 |
| L-6 | Rust proto(`timpani_rust/timpani-o/proto/schedinfo.proto`)에 필드만 동기화할지, Rust 포트 작업 시 일괄 반영할지 | Rust CI |
