// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "bpf_loader.h"
#include "phase0_prepare.h"
#include <dirent.h>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "timpani.skel.h"

namespace {

/**
 * parse_cpu_range_to_mask() - Convert a cpuset range string (e.g. "2-3,5") to
 * a bitmask.  Returns 0 if the string is empty or unparseable.
 */
static uint64_t parse_cpu_range_to_mask(const std::string& range_str)
{
    uint64_t mask = 0;
    std::istringstream ss(range_str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // strip whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(token);
        if (token.empty()) continue;
        auto dash = token.find('-');
        if (dash == std::string::npos) {
            int cpu = std::stoi(token);
            if (cpu >= 0 && cpu < 64) mask |= (1ULL << cpu);
        } else {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int c = lo; c <= hi && c < 64; c++) mask |= (1ULL << c);
        }
    }
    return mask;
}

/**
 * read_file_line() - Read the first non-empty line of a file.
 * Returns empty string on failure.
 */
static std::string read_file_line(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) return line;
    }
    return {};
}

/**
 * scan_cgroup_tree_for_isolated() - Walk /sys/fs/cgroup recursively and
 * aggregate cpuset.cpus.effective for all cgroups whose
 * cpuset.cpus.partition == "isolated".
 *
 * This is the Stage-2 fallback when the root-level cpuset.cpus.isolated knob
 * is unavailable or empty (older kernels, unusual configs).
 */
static uint64_t scan_cgroup_tree_for_isolated(const std::string& base)
{
    uint64_t mask = 0;
    DIR* dir = opendir(base.c_str());
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string sub = base + "/" + entry->d_name;

        // Check if this cgroup is an isolated partition
        std::string part_val = read_file_line(sub + "/cpuset.cpus.partition");
        if (part_val == "isolated") {
            std::string eff = read_file_line(sub + "/cpuset.cpus.effective");
            if (eff.empty()) eff = read_file_line(sub + "/cpuset.cpus");
            mask |= parse_cpu_range_to_mask(eff);
        }

        // Recurse
        mask |= scan_cgroup_tree_for_isolated(sub);
    }
    closedir(dir);
    return mask;
}

/**
 * detect_isolated_cpu_mask() - Detect isolated CPUs from cgroup v2.
 *
 * Stage 1: Read /sys/fs/cgroup/cpuset.cpus.isolated (root-level aggregate).
 *          This file is always accurate regardless of where child cgroup
 *          partitions were created.
 * Stage 2: If Stage 1 is unavailable or empty, scan the cgroup tree for
 *          cgroups with cpuset.cpus.partition == "isolated".
 *
 * Returns the bitmask of isolated CPUs, or 0 if none found.
 */
static uint64_t detect_isolated_cpu_mask()
{
    // Stage 1: root aggregate knob (kernel 5.14+, cgroup v2)
    static const std::string ISOLATED_KNOB = "/sys/fs/cgroup/cpuset.cpus.isolated";
    std::string val = read_file_line(ISOLATED_KNOB);
    if (!val.empty()) {
        uint64_t mask = parse_cpu_range_to_mask(val);
        if (mask != 0) {
            std::cout << "[BpfLoader] Isolated CPUs (Stage 1 — "
                      << ISOLATED_KNOB << "): " << val << std::endl;
            return mask;
        }
    }

    // Stage 2: cgroup tree scan
    std::cout << "[BpfLoader] cpuset.cpus.isolated empty or unavailable, "
                 "falling back to cgroup tree scan..." << std::endl;
    uint64_t mask = scan_cgroup_tree_for_isolated("/sys/fs/cgroup");
    if (mask != 0) {
        std::cout << "[BpfLoader] Isolated CPUs (Stage 2 — tree scan): "
                  << "mask=0x" << std::hex << mask << std::dec << std::endl;
    }
    return mask;
}

} // anonymous namespace

namespace timpani {
namespace node {

BpfLoader::BpfLoader()
    : active_map_idx_(0), isolated_cpu_mask_(0), skel_(nullptr)
{
}

BpfLoader::~BpfLoader() {
    unload_programs();
}

bool BpfLoader::load_programs() {
    // Detect isolated CPUs before loading the BPF program so the mask can be
    // injected into the BPF global (skel->bss->isolated_cpu_mask) before
    // attach.  The BPF init() op uses this mask to create per-CPU TT DSQs
    // only for actually-isolated CPUs — no hardcoded CPU count needed.
    uint64_t iso_mask = detect_isolated_cpu_mask();
    if (iso_mask == 0) {
        // No isolated CPUs found: scx_timpani has no CPUs to manage.
        // This is a fatal misconfiguration for an RT system — abort rather
        // than silently running without RT guarantees.
        std::cerr << "[BpfLoader] FATAL: No isolated CPUs detected from cgroup.\n"
                  << "            Expected: /sys/fs/cgroup/cpuset.cpus.isolated\n"
                  << "                  or: a cgroup with cpuset.cpus.partition=isolated\n"
                  << "            Configure CPU isolation before starting timpani-n."
                  << std::endl;
        return false;
    }

    isolated_cpu_mask_ = iso_mask;

    // Open skeleton (parse BPF ELF, do NOT load yet)
    skel_ = timpani_bpf__open();
    if (!skel_) {
        std::cerr << "[BpfLoader] Failed to open BPF skeleton" << std::endl;
        return false;
    }

    // Inject isolated CPU mask into BPF .rodata global BEFORE load+verifier.
    // isolated_cpu_mask is declared `volatile const` in BPF source, so the
    // skeleton exposes it via ->rodata (writable before load, read-only after).
    // The BPF init() callback reads this to create per-CPU TT DSQs.
    skel_->rodata->isolated_cpu_mask = iso_mask;
    std::cout << "[BpfLoader] Injecting isolated_cpu_mask=0x"
              << std::hex << iso_mask << std::dec
              << " into BPF global" << std::endl;

    // Load (verifier runs here) then attach (init() called here)
    if (timpani_bpf__load(skel_) != 0) {
        std::cerr << "[BpfLoader] Failed to load BPF skeleton" << std::endl;
        unload_programs();
        return false;
    }

    int err = timpani_bpf__attach(skel_);
    if (err) {
        std::cerr << "[BpfLoader] Failed to attach BPF skeleton" << std::endl;
        unload_programs();
        return false;
    }

    return true;
}

void BpfLoader::unload_programs() {
    if (skel_) {
        timpani_bpf__destroy(skel_);
        skel_ = nullptr;
    }
    isolated_cpu_mask_ = 0;
}

int BpfLoader::tt_table_fd() const {
    if (!skel_) return -1;
    return bpf_map__fd(active_map_idx_ == 0 ? skel_->maps.tt_table_map_0
                                             : skel_->maps.tt_table_map_1);
}

int BpfLoader::cbs_fd() const {
    if (!skel_) return -1;
    return bpf_map__fd(active_map_idx_ == 0 ? skel_->maps.cbs_map_0
                                             : skel_->maps.cbs_map_1);
}

int BpfLoader::task_meta_fd() const {
    if (!skel_) return -1;
    return bpf_map__fd(active_map_idx_ == 0 ? skel_->maps.task_meta_map_0
                                             : skel_->maps.task_meta_map_1);
}

bool BpfLoader::update_tt_slot(const TtSlotKey& key, const TtSlotBpf& slot) {
    int fd = tt_table_fd();
    if (fd < 0) return false;
    return bpf_map_update_elem(fd, &key, &slot, BPF_ANY) == 0;
}

bool BpfLoader::update_cbs_state(uint64_t hash, const CbsState& state) {
    int fd = cbs_fd();
    if (fd < 0) return false;
    return bpf_map_update_elem(fd, &hash, &state, BPF_ANY) == 0;
}

bool BpfLoader::update_task_meta(uint32_t pid, const TaskMeta& meta) {
    int fd = task_meta_fd();
    if (fd < 0) return false;
    return bpf_map_update_elem(fd, &pid, &meta, BPF_ANY) == 0;
}

bool BpfLoader::delete_task_meta(uint32_t pid) {
    int fd = task_meta_fd();
    if (fd < 0) return false;
    return bpf_map_delete_elem(fd, &pid) == 0;
}

bool BpfLoader::delete_tt_slot(const TtSlotKey& key) {
    int fd = tt_table_fd();
    if (fd < 0) return false;
    return bpf_map_delete_elem(fd, &key) == 0;
}

bool BpfLoader::delete_cbs_state(uint64_t hash) {
    int fd = cbs_fd();
    if (fd < 0) return false;
    return bpf_map_delete_elem(fd, &hash) == 0;
}

bool BpfLoader::update_current_slot(uint32_t cpu, uint32_t slot_idx) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.current_slot_map), &cpu, &slot_idx, BPF_ANY) == 0;
}

bool BpfLoader::update_next_tt_start(uint32_t cpu, uint64_t next_start_ns) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.next_tt_start_map), &cpu, &next_start_ns, BPF_ANY) == 0;
}

int BpfLoader::get_fault_ringbuf_fd() const {
    if (!skel_) return -1;
    return bpf_map__fd(skel_->maps.fault_ringbuf);
}

void BpfLoader::swap_shadow_maps() {
    active_map_idx_ = 1 - active_map_idx_;
}

int BpfLoader::get_active_map_idx() const {
    return active_map_idx_;
}

int BpfLoader::get_shadow_map_idx() const {
    return 1 - active_map_idx_;
}

bool BpfLoader::clear_bank(int bank) {
    if (!skel_) return false;
    bank = clamp_bank(bank);

    struct {
        struct bpf_map* map;
        size_t key_size;
    } maps[3] = {
        {bank == 0 ? skel_->maps.tt_table_map_0 : skel_->maps.tt_table_map_1,
         sizeof(TtSlotKey)},
        {bank == 0 ? skel_->maps.cbs_map_0 : skel_->maps.cbs_map_1,
         sizeof(uint64_t)},
        {bank == 0 ? skel_->maps.task_meta_map_0 : skel_->maps.task_meta_map_1,
         sizeof(uint32_t)},
    };

    for (const auto& m : maps) {
        int fd = bpf_map__fd(m.map);
        if (fd < 0) return false;
        std::vector<uint8_t> key(m.key_size, 0);
        std::vector<uint8_t> next(m.key_size, 0);
        // Walk every entry and delete it. bpf_map_get_next_key() returns
        // -ENOENT when the walk is exhausted.
        int ret = bpf_map_get_next_key(fd, nullptr, next.data());
        while (ret == 0) {
            std::swap(key, next);
            if (bpf_map_delete_elem(fd, key.data()) != 0) {
                return false;  // cleanup failure: bank left dirty
            }
            ret = bpf_map_get_next_key(fd, key.data(), next.data());
        }
        if (ret != -ENOENT) return false;
    }
    return true;
}

bool BpfLoader::stage_table(const phase0::NPrepared& prepared) {
    if (!skel_) return false;

    int shadow = get_shadow_map_idx();
    // The shadow bank must have no in-flight BPF references before it is
    // cleared; otherwise a running callback could read a half-written bank.
    if (__atomic_load_n(
            &skel_->bss->kernel_control.bank_active_refs[shadow],
            __ATOMIC_ACQUIRE) != 0) {
        return false;
    }
    if (!clear_bank(shadow)) return false;

    int tt_fd = bpf_map__fd(shadow == 0 ? skel_->maps.tt_table_map_0
                                        : skel_->maps.tt_table_map_1);
    int meta_fd = bpf_map__fd(shadow == 0 ? skel_->maps.task_meta_map_0
                                           : skel_->maps.task_meta_map_1);
    int cbs_fd = bpf_map__fd(shadow == 0 ? skel_->maps.cbs_map_0
                                          : skel_->maps.cbs_map_1);
    if (tt_fd < 0 || meta_fd < 0 || cbs_fd < 0) {
        clear_bank(shadow);
        return false;
    }

    // TT slots: staged inactive (tt_runnable = 0, activation_ns = 0) so the
    // bank is inert until apply_table_update() switches the active index.
    for (const auto& slot : prepared.slots) {
        TtSlotKey key = {.cpu = slot.cpu, .slot_idx = slot.slot_idx};
        TtSlotBpf bpf_slot = {};
        bpf_slot.workload_id_hash = slot.workload_id_hash;
        bpf_slot.task_id_hash = slot.task_id_hash;
        bpf_slot.offset_us = slot.offset_us;
        bpf_slot.duration_us = slot.duration_us;
        bpf_slot.deadline_us = slot.deadline_us;
        bpf_slot.cpu = slot.cpu;
        if (bpf_map_update_elem(tt_fd, &key, &bpf_slot, BPF_ANY) != 0) {
            clear_bank(shadow);  // discard partial candidate, no residual
            return false;
        }
    }

    // TaskMeta: one entry per prepared task, keyed by PID. Staged inactive
    // (tt_runnable = 0, activation_ns = 0) so the bank is inert until
    // apply_table_update() switches the active index.
    std::map<uint64_t, const phase0::PreparedSlot*> slot_by_task;
    for (const auto& slot : prepared.slots) {
        slot_by_task.emplace(slot.task_id_hash, &slot);
    }
    for (const auto& [task_hash, task] : prepared.tasks) {
        if (task.pid < 0) {
            clear_bank(shadow);
            return false;
        }
        TaskMeta meta = {};
        meta.workload_id_hash = task.workload_id_hash;
        meta.task_id_hash = task_hash;
        meta.assigned_cpu = prepared.has_cpu ? prepared.cpu : 0;
        meta.scheduling_type = SCHED_TYPE_TT;
        meta.tt_runnable = 0;
        meta.activation_ns = 0;
        auto it = slot_by_task.find(task_hash);
        if (it != slot_by_task.end()) {
            meta.slot_duration_us = it->second->duration_us;
            meta.layer = static_cast<uint8_t>(it->second->layer_index);
        }
        uint32_t pid = static_cast<uint32_t>(task.pid);
        if (bpf_map_update_elem(meta_fd, &pid, &meta, BPF_ANY) != 0) {
            clear_bank(shadow);
            return false;
        }
    }

    // CBS states: Phase 0 candidates never contain CBS entries (rejected at
    // the P0-T02 pre-check), so the shadow bank's CBS map is cleared and
    // left empty. CBS staging is deferred to a later phase.

    return true;
}

void BpfLoader::apply_table_update() {
    if (!skel_) return;
    int old = active_map_idx_;
    swap_shadow_maps();
    // Single handover step in kernel .bss. Callback-wide references pin and
    // revalidate their selected bank before any lookup.
    apply_handover(&skel_->bss->kernel_control, active_map_idx_);
    cleanup_old_bank(old);
}

void BpfLoader::cleanup_old_bank(int old_bank) {
    if (!skel_) return;
    old_bank = clamp_bank(old_bank);
    // Only clean once no BPF callback still references the old bank.
    if (__atomic_load_n(
            &skel_->bss->kernel_control.bank_active_refs[old_bank],
            __ATOMIC_ACQUIRE) == 0) {
        clear_bank(old_bank);
    }
}

::KernelControl* BpfLoader::kernel_control() {
    if (!skel_ || !skel_->bss) return nullptr;
    return &skel_->bss->kernel_control;
}

bool BpfLoader::tt_runnable_count(uint32_t cpu, uint64_t& count) const {
    if (!skel_) return false;
    int fd = bpf_map__fd(skel_->maps.tt_runnable_count_map);
    if (fd < 0) return false;
    return bpf_map_lookup_elem(fd, &cpu, &count) == 0;
}

bool BpfLoader::tt_held_count(uint32_t cpu, uint64_t& count) const {
    if (!skel_) return false;
    int fd = bpf_map__fd(skel_->maps.tt_held_count_map);
    if (fd < 0) return false;
    return bpf_map_lookup_elem(fd, &cpu, &count) == 0;
}

bool BpfLoader::reset_tt_hold_tracking() {
    if (!skel_ || !skel_->bss) return false;
    __atomic_store_n(&skel_->bss->tt_hold_failure_count, 0u,
                     __ATOMIC_RELEASE);
    return true;
}

bool BpfLoader::tt_hold_tracking_healthy() const {
    return skel_ && skel_->bss &&
           __atomic_load_n(&skel_->bss->tt_hold_failure_count,
                           __ATOMIC_ACQUIRE) == 0;
}

bool BpfLoader::kick_tt_cpu(uint32_t cpu) {
    if (!skel_ || !skel_->data || cpu >= 64) return false;
    __atomic_store_n(&skel_->data->tt_kick_cpu_request, cpu,
                     __ATOMIC_RELEASE);
    LIBBPF_OPTS(bpf_test_run_opts, options);
    const int program_fd = bpf_program__fd(skel_->progs.kick_tt_cpu);
    return program_fd >= 0 &&
           bpf_prog_test_run_opts(program_fd, &options) == 0 &&
           options.retval == 0;
}

bool BpfLoader::drain_tt_held() {
    constexpr auto timeout = std::chrono::milliseconds(100);
    constexpr auto poll = std::chrono::milliseconds(1);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        if (!tt_hold_tracking_healthy()) return false;

        bool pending = false;
        for (uint32_t cpu = 0; cpu < 64; ++cpu) {
            if ((isolated_cpu_mask_ & (1ULL << cpu)) == 0) continue;
            uint64_t held = 0;
            if (!tt_held_count(cpu, held)) return false;
            if (held != 0) {
                pending = true;
                if (!kick_tt_cpu(cpu)) return false;
            }
        }
        if (!pending) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(poll);
    }
}

bool BpfLoader::select_staged_bank() {
    if (!skel_ || !skel_->bss) return false;
    swap_shadow_maps();
    __atomic_store_n(&skel_->bss->kernel_control.active_bank_idx,
                     static_cast<uint32_t>(active_map_idx_), __ATOMIC_SEQ_CST);
    return true;
}

} // namespace node
} // namespace timpani
