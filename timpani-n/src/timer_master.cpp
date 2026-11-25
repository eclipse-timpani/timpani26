// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "timer_master.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../sample-apps/src/ttsched_shm.h"
#include "phase0_prepare.h"

namespace timpani {
namespace node {

namespace {

uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

void sleep_until_realtime(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr) ==
           EINTR) {
    }
}

// DDR-011 Phase 0 resident RT thread: no allocation, locks or logging.
class Phase0RtPort final : public phase0::RtPlatform
{
  public:
    Phase0RtPort(BpfLoader& bpf, volatile struct timpani_ttsched_shm* shm,
                 ::KernelControl* control)
        : bpf_(bpf), shm_(shm), control_(control)
    {
        struct timex tx = {};
        if (adjtimex(&tx) >= 0) {
            tai_offset_ns_ = static_cast<int64_t>(tx.tai) * 1000000000LL;
        }
    }

    uint64_t now_ns() override { return clock_ns(CLOCK_REALTIME); }

    void fire(const phase0::RtSlot& slot, uint64_t target_ns,
              uint64_t next_start_ns) override
    {
        bpf_.update_current_slot(slot.cpu, slot.slot_idx);
        if (next_start_ns > target_ns) {
            bpf_.update_next_tt_start(
                slot.cpu,
                clock_ns(CLOCK_MONOTONIC) + (next_start_ns - target_ns));
        }
        if (!shm_) return;
        const uint64_t deadline_ns =
            target_ns + tai_offset_ns_ + slot.deadline_ns;
        ttsched_shm_publish_release(shm_, slot.shm_idx, deadline_ns);
        uint32_t* counter =
            const_cast<uint32_t*>(&shm_->tasks[slot.shm_idx].counter);
        syscall(SYS_futex, counter, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    void open_tt_gate() override
    {
        if (control_) {
            __atomic_store_n(&control_->tt_handover_gate, 0u, __ATOMIC_RELEASE);
        }
    }

  private:
    BpfLoader& bpf_;
    volatile struct timpani_ttsched_shm* shm_;
    ::KernelControl* control_;
    uint64_t tai_offset_ns_ = 0;
};

// DDR-011 Phase 0 handover coordinator: non-RT kernel operations.
class Phase0CoordinatorPort final : public phase0::CoordinatorPort
{
  public:
    Phase0CoordinatorPort(BpfLoader& bpf, ::KernelControl* control)
        : bpf_(bpf), control_(control)
    {
    }

    uint64_t now_ns() override { return clock_ns(CLOCK_REALTIME); }
    void sleep_until(uint64_t ns) override { sleep_until_realtime(ns); }

    bool stage_bank(const phase0::NPrepared& prepared) override
    {
        return bpf_.stage_table(prepared);
    }

    void discard_staged_bank() override
    {
        // Staging an empty table clears the inactive bank.
        if (!bpf_.stage_table(phase0::NPrepared())) {
            std::cerr << "[TimerMaster] Phase 0: staged bank not cleared"
                      << std::endl;
        }
    }

    bool tt_idle(uint32_t cpu) override
    {
        uint32_t runnable = 0;
        if (!bpf_.tt_runnable_count(cpu, runnable) || runnable != 0) {
            return false;
        }
        const uint32_t bank =
            __atomic_load_n(&control_->active_bank_idx, __ATOMIC_SEQ_CST) & 1u;
        return __atomic_load_n(&control_->bank_active_refs[bank],
                               __ATOMIC_SEQ_CST) == 0;
    }

    void set_tt_gate(bool closed) override
    {
        __atomic_store_n(&control_->tt_handover_gate, closed ? 1u : 0u,
                         __ATOMIC_SEQ_CST);
    }

    bool select_staged_bank() override { return bpf_.select_staged_bank(); }

    int active_bank() override
    {
        return static_cast<int>(
            __atomic_load_n(&control_->active_bank_idx, __ATOMIC_SEQ_CST) & 1u);
    }

    bool reset_slot_state(uint32_t cpu, uint64_t first_start_ns) override
    {
        uint64_t next_start = 0;
        if (first_start_ns != 0) {
            const uint64_t now = clock_ns(CLOCK_REALTIME);
            next_start = clock_ns(CLOCK_MONOTONIC) +
                         (first_start_ns > now ? first_start_ns - now : 0);
        }
        return bpf_.update_current_slot(cpu, SLOT_NONE) &&
               bpf_.update_next_tt_start(cpu, next_start);
    }

  private:
    BpfLoader& bpf_;
    ::KernelControl* control_;
};

}  // namespace

TimerMaster::TimerMaster(BpfLoader& bpf_loader)
    : bpf_loader_(bpf_loader),
      running_(false),
      table_pending_(false),
      hyperperiod_ns_(0),
      epoch_ns_(0),
      shm_fd_(-1),
      ttsched_shm_(nullptr)
{
    shm_fd_ = shm_open("/timpani_ttsched", O_CREAT | O_RDWR, 0666);
    if (shm_fd_ >= 0) {
        if (ftruncate(shm_fd_, sizeof(struct timpani_ttsched_shm)) < 0) {
            std::cerr << "[TimerMaster] ftruncate failed" << std::endl;
        }
        ttsched_shm_ = static_cast<volatile struct timpani_ttsched_shm*>(
            mmap(nullptr, sizeof(struct timpani_ttsched_shm),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0));
        if (ttsched_shm_ == MAP_FAILED) {
            ttsched_shm_ = nullptr;
            std::cerr << "[TimerMaster] shm mmap failed" << std::endl;
        } else {
            /* Not ready (magic=0, n_tasks=0) with an even, stable generation
             * above any value left in a reused object (DDR-011 D-P0-05). */
            ttsched_shm_reset(ttsched_shm_);
            std::cout << "[TimerMaster] shm /timpani_ttsched created"
                      << std::endl;
        }
    } else {
        std::cerr << "[TimerMaster] shm_open failed" << std::endl;
    }
}

TimerMaster::~TimerMaster()
{
    stop();

    if (ttsched_shm_ && ttsched_shm_ != MAP_FAILED) {
        /* Signal shutdown to all waiting tasks:
         * 1. Zero magic so tasks detect timpani-n is gone after wakeup.
         * 2. FUTEX_WAKE on every task counter to unblock FUTEX_WAIT.
         *    INT_MAX wakes all waiters regardless of count. */
        __atomic_store_n(const_cast<uint32_t*>(&ttsched_shm_->magic),
                         0u, __ATOMIC_SEQ_CST);
        uint32_t n = ttsched_shm_->n_tasks;
        if (n > TIMPANI_MAX_TASKS) n = TIMPANI_MAX_TASKS;
        for (uint32_t i = 0; i < n; i++) {
            syscall(SYS_futex,
                    const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                    FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        munmap(const_cast<struct timpani_ttsched_shm*>(ttsched_shm_),
               sizeof(struct timpani_ttsched_shm));
    }
    if (shm_fd_ >= 0) close(shm_fd_);
    shm_unlink("/timpani_ttsched");
}

void TimerMaster::set_schedule_table(const std::vector<SlotEntry>& slots,
                                     uint64_t hyperperiod_us, uint64_t epoch_ns)
{
    if (phase0_) {
        std::cerr << "[TimerMaster] set_schedule_table() is refused in "
                     "Phase 0 mode; use apply_phase0_table()"
                  << std::endl;
        return;
    }
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    /* Runtime table updates are serialized by the gRPC callback path in this
     * component. The slot-table swap, SHM publication, and worker restart are
     * therefore treated as a single state transition. If this becomes
     * multi-threaded, an explicit higher-level serializer should be added. */
    slot_table_ = slots;
    hyperperiod_ns_ = hyperperiod_us * 1000ULL;
    epoch_ns_ = epoch_ns;
    // Sort by offset_ns ascending
    std::sort(slot_table_.begin(), slot_table_.end(),
              [](const SlotEntry& a, const SlotEntry& b) {
                  return a.offset_ns < b.offset_ns;
              });

    /* Publish per-task SHM slots for targeted wake.
     * Collect unique tasks (same task may appear in multiple slots). */
    if (ttsched_shm_) {
        /* Odd generation and magic=0 mark the update in progress so tasks
         * never use a half-written table (DDR-011 D-P0-05). */
        const uint32_t publishing = ttsched_shm_begin_publish(ttsched_shm_);

        /* Wake all tasks currently waiting on SHM slots so they notice magic == 0
         * and re-initialize their slot indices when the new schedule table is published. */
        for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; ++i) {
            __atomic_fetch_add(const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                               1u, __ATOMIC_SEQ_CST);
            syscall(SYS_futex, const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                    FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        std::map<uint64_t, int> prev_mapping = task_hash_to_shm_idx_;
        task_hash_to_shm_idx_.clear();

        std::vector<bool> used_slots(TIMPANI_MAX_TASKS, false);

        /* First pass: preserve stable SHM indices for existing tasks */
        for (const auto& entry : slot_table_) {
            if (task_hash_to_shm_idx_.count(entry.task_id_hash)) continue;
            auto it = prev_mapping.find(entry.task_id_hash);
            if (it != prev_mapping.end() && it->second < TIMPANI_MAX_TASKS &&
                !used_slots[it->second]) {
                uint32_t idx = it->second;
                task_hash_to_shm_idx_[entry.task_id_hash] = idx;
                used_slots[idx] = true;
                ttsched_shm_store_name(ttsched_shm_, idx,
                                       entry.task_name.c_str());
            }
        }

        /* Second pass: allocate lowest available SHM indices for new tasks */
        for (const auto& entry : slot_table_) {
            if (task_hash_to_shm_idx_.count(entry.task_id_hash)) continue;

            uint32_t idx = 0;
            while (idx < TIMPANI_MAX_TASKS && used_slots[idx]) {
                idx++;
            }
            if (idx >= TIMPANI_MAX_TASKS) {
                std::cerr << "[TimerMaster] Too many unique tasks (max "
                          << TIMPANI_MAX_TASKS << ")" << std::endl;
                break;
            }

            used_slots[idx] = true;
            task_hash_to_shm_idx_[entry.task_id_hash] = idx;
            ttsched_shm_store_name(ttsched_shm_, idx, entry.task_name.c_str());
            ttsched_shm_store_counter(ttsched_shm_, idx, 0);
        }

        uint32_t total_slots = 0;
        for (const auto& kv : task_hash_to_shm_idx_) {
            if (kv.second + 1 > total_slots) {
                total_slots = kv.second + 1;
            }
            std::cout << "[TimerMaster] SHM slot[" << kv.second
                      << "] = " << ttsched_shm_->tasks[kv.second].name
                      << " hash=0x" << std::hex << kv.first << std::dec
                      << std::endl;
        }

        /* Publish n_tasks, then the next even generation and magic */
        ttsched_shm_store_n_tasks(ttsched_shm_, total_slots);
        const uint32_t generation =
            ttsched_shm_end_publish(ttsched_shm_, publishing);

        std::cout << "[TimerMaster] SHM published: " << total_slots
                  << " task slots ready (gen=" << generation << ")"
                  << std::endl;
    }

    table_pending_ = true;  // signal existing threads to exit

    /* If already running, respawn per-CPU threads with the new table */
    if (running_) {
        /* Release lock before calling start() which also acquires it */
        lock.unlock();
        start();
    }
}

void TimerMaster::start()
{
    /* Lock ordering rule: lifecycle_mutex_ must be acquired before
     * schedule_mutex_ whenever both are needed. This prevents lock inversion
     * between the gRPC-driven table updates and the timer-thread lifecycle. */
    std::lock_guard<std::recursive_mutex> lk(lifecycle_mutex_);

    if (phase0_) {
        /* DDR-011 Phase 0: one resident RT thread, never restarted. */
        if (running_) return;
        running_ = true;
        table_pending_ = false;
        std::cout << "[TimerMaster] Started the Phase 0 resident timer thread"
                  << std::endl;
        cpu_threads_.emplace_back(&TimerMaster::phase0_loop, this);
        return;
    }

    /* Stop any existing per-CPU threads before spawning new ones */
    stop();

    running_ = true;
    table_pending_ = false;

    /* Partition slot_table_ by CPU */
    std::map<uint32_t, std::vector<SlotEntry>> per_cpu_slots;
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        for (const auto& entry : slot_table_) {
            per_cpu_slots[entry.cpu].push_back(entry);
        }
    }

    if (per_cpu_slots.empty()) {
        std::cout << "[TimerMaster] Started in idle mode (awaiting initial schedule table)" << std::endl;
        /* No slots yet — spawn a single idle-poll thread that waits for table_pending_ */
        cpu_threads_.emplace_back([this]() {
            struct sched_param param = {};
            param.sched_priority = 99;
            sched_setscheduler(0, SCHED_FIFO, &param);

            struct timespec next_ts;
            clock_gettime(CLOCK_REALTIME, &next_ts);
            while (running_ && !table_pending_) {
                next_ts.tv_nsec += 1000000;
                if (next_ts.tv_nsec >= 1000000000) {
                    next_ts.tv_sec += 1;
                    next_ts.tv_nsec -= 1000000000;
                }
                clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts,
                                nullptr);
            }
        });
        return;
    }

    /* Each per-CPU slot list is already sorted by offset_ns (inherited from
     * slot_table_ which was sorted in set_schedule_table). */
    uint64_t hp_ns, ep_ns;
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        hp_ns = hyperperiod_ns_;
        ep_ns = epoch_ns_;
    }

    std::cout << "[TimerMaster] Starting per-CPU timer threads across "
              << per_cpu_slots.size() << " CPU(s) (hyperperiod="
              << (hp_ns / 1000ULL) << "us)" << std::endl;

    for (auto& [cpu, cpu_slots] : per_cpu_slots) {
        std::cout << "[TimerMaster] Spawning per-CPU thread for CPU " << cpu
                  << " with " << cpu_slots.size() << " slots" << std::endl;
        cpu_threads_.emplace_back(&TimerMaster::cpu_thread_loop, this,
                                  cpu, std::move(cpu_slots), hp_ns, ep_ns);
    }
}

void TimerMaster::stop()
{
    std::lock_guard<std::recursive_mutex> lk(lifecycle_mutex_);
    running_ = false;
    for (auto& t : cpu_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    cpu_threads_.clear();
}

uint64_t TimerMaster::compute_next_tt_start_for_cpu(
    const std::vector<SlotEntry>& cpu_slots,
    size_t current_slot_idx,
    uint64_t current_hyperperiod_start,
    uint64_t hyperperiod_ns)
{
    if (cpu_slots.empty() || hyperperiod_ns == 0) return 0;

    size_t next_idx = (current_slot_idx + 1) % cpu_slots.size();
    uint64_t next_hyperperiod_start = current_hyperperiod_start;
    if (next_idx <= current_slot_idx) {
        next_hyperperiod_start += hyperperiod_ns;
    }
    return next_hyperperiod_start + cpu_slots[next_idx].offset_ns;
}

void TimerMaster::remove_workload(uint64_t workload_id_hash)
{
    if (phase0_) {
        std::cerr << "[TimerMaster] remove_workload() is refused in Phase 0 "
                     "mode"
                  << std::endl;
        return;
    }
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    /* remove_workload() follows the same gRPC-driven update path as
     * set_schedule_table() and reuses the same serialized state transition. */
    auto it = std::remove_if(slot_table_.begin(), slot_table_.end(),
                             [workload_id_hash](const SlotEntry& entry) {
                                 return entry.workload_id_hash == workload_id_hash;
                             });
    if (it != slot_table_.end()) {
        slot_table_.erase(it, slot_table_.end());
        table_pending_ = true;
        std::cout << "[TimerMaster] Removed slots for workload hash: 0x" 
                  << std::hex << workload_id_hash << std::dec << std::endl;
        if (running_) {
            lock.unlock();
            start();
        }
    }
}

void TimerMaster::cpu_thread_loop(uint32_t cpu,
                                  std::vector<SlotEntry> cpu_slots,
                                  uint64_t hyperperiod_ns,
                                  uint64_t epoch_ns)
{
    /* "Per-CPU" here means each worker loop manages one partition of the slot
     * table for a specific CPU. The thread itself is not pinned to that CPU
     * with sched_setaffinity(), so this is a logical partitioning scheme rather
     * than an execution-affinity guarantee. */
    struct sched_param param = {};
    param.sched_priority = 99;
    sched_setscheduler(0, SCHED_FIFO, &param);

    std::vector<long long> jitters;
    jitters.reserve(1500);

    /* Sort per-CPU slots by offset_ns ascending */
    std::sort(cpu_slots.begin(), cpu_slots.end(),
              [](const SlotEntry& a, const SlotEntry& b) {
                  return a.offset_ns < b.offset_ns;
              });

    std::cout << "[TimerMaster:CPU" << cpu << "] Thread started with "
              << cpu_slots.size() << " slots, hyperperiod="
              << (hyperperiod_ns / 1000ULL) << "us" << std::endl;

    while (running_) {
        if (table_pending_) break;

        if (cpu_slots.empty() || hyperperiod_ns == 0) {
            struct timespec next_ts;
            clock_gettime(CLOCK_REALTIME, &next_ts);
            while (running_ && !table_pending_) {
                next_ts.tv_nsec += 1000000;
                if (next_ts.tv_nsec >= 1000000000) {
                    next_ts.tv_sec += 1;
                    next_ts.tv_nsec -= 1000000000;
                }
                clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts,
                                nullptr);
            }
            break;
        }

        size_t next_slot_idx = 0;
        uint64_t current_hyperperiod_start = epoch_ns;
        struct timespec now_ts;
        clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t now_ns =
            (uint64_t)now_ts.tv_sec * 1000000000ULL + now_ts.tv_nsec;

        if (epoch_ns == 0) {
            current_hyperperiod_start = now_ns;
        } else if (epoch_ns < now_ns) {
            uint64_t elapsed_ns = now_ns - epoch_ns;
            uint64_t periods = (elapsed_ns / hyperperiod_ns) + 1;
            current_hyperperiod_start =
                epoch_ns + periods * hyperperiod_ns;
            std::cout << "[TimerMaster:CPU" << cpu << "] Catch-up applied: jumped "
                      << periods << " periods into the future." << std::endl;
        }
        std::cout << "[TimerMaster:CPU" << cpu << "] Hyperperiod start time set: "
                  << (current_hyperperiod_start / 1000ULL) << " us"
                  << std::endl;

        while (running_ && !table_pending_) {
            const auto& slot = cpu_slots[next_slot_idx];
            uint64_t target_ns = current_hyperperiod_start + slot.offset_ns;

            struct timespec next_ts;
            next_ts.tv_sec = target_ns / 1000000000ULL;
            next_ts.tv_nsec = target_ns % 1000000000ULL;

            /* Kernel TAI - UTC seconds; read before sleeping to keep it off the release path. */
            struct timex tx = {};
            adjtimex(&tx);
            const long long tai_offset_ns = tx.tai * 1000000000LL;

            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts, nullptr);

            struct timespec wakeup_ts;
            clock_gettime(CLOCK_REALTIME, &wakeup_ts);

            long long actual_ns =
                (long long)wakeup_ts.tv_sec * 1000000000LL + wakeup_ts.tv_nsec;
            long long jitter = actual_ns - (long long)target_ns;

            const int64_t planned_deadline_ns = static_cast<int64_t>(target_ns) +
                                               tai_offset_ns +
                                               slot.deadline_us * 1000LL;

            bpf_loader_.update_current_slot(slot.cpu, slot.slot_idx);

            struct timespec mono_ts;
            clock_gettime(CLOCK_MONOTONIC, &mono_ts);
            uint64_t now_mono_ns =
                (uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;
            uint64_t next_tt_realtime_ns = compute_next_tt_start_for_cpu(
                cpu_slots, next_slot_idx, current_hyperperiod_start,
                hyperperiod_ns);
            if (next_tt_realtime_ns > target_ns) {
                uint64_t next_delta_ns = next_tt_realtime_ns - target_ns;
                bpf_loader_.update_next_tt_start(slot.cpu,
                                                 now_mono_ns + next_delta_ns);
            }

            jitters.push_back(jitter);

            if (jitters.size() == 1000) {
                long long sum = 0, min_j = jitters[0], max_j = jitters[0];
                for (long long j : jitters) {
                    sum += j;
                    if (j < min_j) min_j = j;
                    if (j > max_j) max_j = j;
                }
                std::sort(jitters.begin(), jitters.end());

                std::cout
                    << "\n[CPU" << cpu
                    << "] Timer Master Wakeup Jitter (CLOCK_REALTIME ABSTIME):"
                    << std::endl;
                std::cout << "  Samples: 1000" << std::endl;
                std::cout << "  Min: " << min_j << " ns" << std::endl;
                std::cout << "  Max: " << max_j << " ns" << std::endl;
                std::cout << "  Avg: " << sum / 1000 << " ns" << std::endl;
                std::cout << "  P99: " << jitters[990] << " ns" << std::endl;
                std::cout << "  P999: " << jitters[999] << " ns" << std::endl;

                std::cout << "\n[CPU" << cpu
                          << "] Histogram (10us buckets, absolute jitter):"
                          << std::endl;
                int buckets[10] = {0};
                int outliers = 0;
                for (long long j : jitters) {
                    long long abs_j = j < 0 ? -j : j;
                    int bucket_idx = (int)(abs_j / 10000);
                    if (bucket_idx >= 0 && bucket_idx < 10)
                        buckets[bucket_idx]++;
                    else
                        outliers++;
                }
                for (int i = 0; i < 10; ++i) {
                    std::cout << "  " << (i * 10) << "us - " << ((i + 1) * 10)
                              << "us: " << buckets[i] << std::endl;
                }
                std::cout << "  > 100us outliers: " << outliers << std::endl;

                std::cout << "[CPU" << cpu
                          << "] Measurement complete. Continuing..."
                          << std::endl;
                jitters.clear();
                jitters.reserve(1500);
            }

            wake_task(slot.task_id_hash, planned_deadline_ns > 0 ? planned_deadline_ns : 1);

            next_slot_idx++;
            if (next_slot_idx >= cpu_slots.size()) {
                next_slot_idx = 0;
                current_hyperperiod_start += hyperperiod_ns;
            }
        }
    }

    std::cout << "[TimerMaster:CPU" << cpu << "] Thread exiting" << std::endl;
}

void TimerMaster::wake_task(uint64_t task_id_hash, uint64_t deadline_ns)
{
    if (!ttsched_shm_) return;

    /* Read the task-to-SHM mapping under schedule_mutex_ so this wake-up path
     * stays consistent with set_schedule_table()/remove_workload() updates. */
    std::lock_guard<std::mutex> lock(schedule_mutex_);
    // Known race (#60): an old-table thread can still publish into a new generation.

    auto it = task_hash_to_shm_idx_.find(task_id_hash);
    if (it == task_hash_to_shm_idx_.end()) return;

    const int idx = it->second;
    ttsched_shm_publish_release(ttsched_shm_, static_cast<uint32_t>(idx),
                                deadline_ns);
    auto* counter = &ttsched_shm_->tasks[idx].counter;
    syscall(SYS_futex, const_cast<uint32_t*>(counter),
            FUTEX_WAKE, 1, /* wake only the ONE task waiting on this counter */
            nullptr, nullptr, 0);
}

bool TimerMaster::enable_phase0()
{
    std::lock_guard<std::recursive_mutex> lk(lifecycle_mutex_);
    if (running_) return false;
    phase0_ = true;
    return true;
}

void TimerMaster::phase0_loop()
{
    struct sched_param param = {};
    param.sched_priority = 99;
    sched_setscheduler(0, SCHED_FIFO, &param);

    Phase0RtPort port(bpf_loader_, ttsched_shm_, bpf_loader_.kernel_control());
    phase0::RtEngine engine(phase0_state_, port, ttsched_shm_);
    while (running_) {
        const uint64_t wake = engine.next_wake_ns();
        if (clock_ns(CLOCK_REALTIME) < wake) {
            sleep_until_realtime(wake);
            continue;
        }
        engine.on_wake(clock_ns(CLOCK_REALTIME));
    }
}

bool TimerMaster::apply_phase0_table(const phase0::NPrepared& prepared,
                                     phase0::ApplyResult& result)
{
    std::lock_guard<std::mutex> guard(phase0_mutex_);
    result = phase0::ApplyResult();
    auto reject = [&result](std::string error) {
        result.rejected = true;
        result.error = std::move(error);
        return false;
    };
    if (!phase0_ || !running_) {
        return reject("the Phase 0 resident timer is not running");
    }
    if (phase0_failed_) {
        return reject("the Phase 0 trial stopped after a failed handover");
    }
    ::KernelControl* control = bpf_loader_.kernel_control();
    if (!ttsched_shm_ || !control) {
        return reject("ttsched SHM or the BPF scheduler is unavailable");
    }

    auto next = std::make_unique<phase0::Context>();
    std::string error;
    if (!phase0::build_context(prepared, ++phase0_token_, *next, error)) {
        return reject(error);
    }
    result.token = next->token;

    Phase0CoordinatorPort port(bpf_loader_, control);
    const phase0::ArmResult armed = phase0::arm_handover(
        phase0_state_, port, ttsched_shm_, phase0_active_.get(), *next,
        prepared, phase0_owners_);
    result.planned_boundary_ns = armed.boundary_ns;
    if (armed.outcome == phase0::ArmOutcome::kRejected) {
        return reject(armed.error);
    }
    // Published to the RT thread: keep it alive whatever happens next.
    phase0_pending_ = std::move(next);
    if (armed.outcome == phase0::ArmOutcome::kFailed) {
        phase0_failed_ = true;
        result.error = armed.error;
        return false;
    }

    const phase0::Completion done =
        phase0::await_completion(phase0_state_, port, armed.boundary_ns);
    result.actual_apply_ns = done.actual_ns;
    if (!done.applied) {
        phase0_failed_ = true;
        result.error = done.error;
        return false;
    }

    // The RT thread switched contexts before reporting kApplied, so the
    // previous context is no longer referenced.
    phase0_active_ = std::move(phase0_pending_);
    phase0_owners_ = phase0::owners_from(prepared.shm);
    phase0_state_.phase.store(phase0::Phase::kIdle, std::memory_order_release);
    result.applied = true;
    result.planned_boundary_ns = done.planned_ns;
    result.shm_generation = armed.shm_generation;
    result.active_bank = port.active_bank();
    return true;
}

}  // namespace node
}  // namespace timpani
