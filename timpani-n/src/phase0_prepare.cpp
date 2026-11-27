// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "phase0_prepare.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <tuple>
#include <utility>

namespace timpani {
namespace node {
namespace phase0 {

static_assert(kMaxIdBytes < sizeof(timpani_task_slot::name),
              "task IDs must fit the SHM name with a NUL terminator");

namespace {

bool fail(std::string& error, std::string reason)
{
    error = std::move(reason);
    return false;
}

bool valid_identifier(const std::string& value)
{
    return !value.empty() && value.size() <= kMaxIdBytes &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return ch > 0 && ch <= 0x7f; });
}

bool is_isolated(uint64_t mask, uint32_t cpu)
{
    return cpu < kMaxCpus && ((mask >> cpu) & 1ULL) != 0;
}

// Keeps ID <-> hash one-to-one within a table.
bool bind_identity(std::map<std::string, uint64_t>& by_id,
                   std::map<uint64_t, std::string>& by_hash,
                   const std::string& id, uint64_t hash)
{
    auto id_it = by_id.find(id);
    if (id_it != by_id.end()) return id_it->second == hash;
    if (by_hash.count(hash) != 0) return false;
    by_id.emplace(id, hash);
    by_hash.emplace(hash, id);
    return true;
}

bool slot_less(const PreparedSlot& a, const PreparedSlot& b)
{
    return std::tie(a.offset_ns, a.cpu, a.workload_id_hash, a.task_id_hash,
                    a.slot_idx) < std::tie(b.offset_ns, b.cpu,
                                           b.workload_id_hash, b.task_id_hash,
                                           b.slot_idx);
}

using SlotParameters = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, int32_t>;

std::vector<SlotParameters> workload_slots(
    const std::vector<PreparedSlot>& slots, uint64_t workload_id_hash)
{
    std::vector<SlotParameters> result;
    for (const auto& slot : slots) {
        if (slot.workload_id_hash == workload_id_hash) {
            // Snapshot ordinals can change when other workloads are updated.
            result.emplace_back(slot.task_id_hash, slot.offset_us,
                                slot.duration_us, slot.deadline_us, slot.cpu,
                                slot.current_limit, slot.layer_index);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool parse_uint(const std::string& text, uint64_t& value)
{
    if (text.empty() || text.size() > 9) return false;
    value = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        value = value * 10 + static_cast<uint64_t>(ch - '0');
    }
    return true;
}

std::string trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string read_first_line(const std::string& path)
{
    std::ifstream file(path);
    std::string line;
    while (file.is_open() && std::getline(file, line)) {
        if (!line.empty()) return line;
    }
    return {};
}

uint64_t parse_or_zero(const std::string& text)
{
    uint64_t mask = 0;
    return parse_cpu_list(text, mask) ? mask : 0;
}

// Mirrors BpfLoader's Stage-2 fallback over cgroup v2 partitions.
uint64_t scan_isolated_partitions(const std::string& base)
{
    uint64_t mask = 0;
    DIR* dir = opendir(base.c_str());
    if (!dir) return 0;
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || entry->d_type != DT_DIR) continue;
        const std::string sub = base + "/" + entry->d_name;
        if (read_first_line(sub + "/cpuset.cpus.partition") == "isolated") {
            std::string cpus = read_first_line(sub + "/cpuset.cpus.effective");
            if (cpus.empty()) cpus = read_first_line(sub + "/cpuset.cpus");
            mask |= parse_or_zero(cpus);
        }
        mask |= scan_isolated_partitions(sub);
    }
    closedir(dir);
    return mask;
}

bool is_number(const char* text)
{
    if (*text == '\0') return false;
    for (const char* p = text; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

bool is_live(const std::string& pid_dir)
{
    const std::string stat = read_first_line(pid_dir + "/stat");
    const auto paren = stat.rfind(')');
    if (paren == std::string::npos || paren + 2 >= stat.size()) return false;
    const char state = stat[paren + 2];
    return state != 'Z' && state != 'X' && state != 'x';
}

}  // namespace

bool slot_fits_hyperperiod(uint32_t offset_us, uint32_t duration_us,
                           uint32_t hyperperiod_us)
{
    return static_cast<uint64_t>(offset_us) +
               static_cast<uint64_t>(duration_us) <=
           static_cast<uint64_t>(hyperperiod_us);
}

bool select_boundary(uint64_t epoch_ns, uint64_t hyperperiod_ns,
                     uint64_t now_ns, Boundary& out)
{
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    if (hyperperiod_ns == 0) return false;

    uint64_t index = 0;
    if (now_ns >= epoch_ns) {
        const uint64_t elapsed = (now_ns - epoch_ns) / hyperperiod_ns;
        if (elapsed == kMax) return false;
        index = elapsed + 1;
    }
    if (index > (kMax - epoch_ns) / hyperperiod_ns) return false;
    const uint64_t target = epoch_ns + index * hyperperiod_ns;
    // Slot times inside [T, T + H) must stay representable.
    if (target > kMax - hyperperiod_ns) return false;

    out.epoch_ns = epoch_ns;
    out.hyperperiod_ns = hyperperiod_ns;
    out.index = index;
    out.target_ns = target;
    return true;
}

bool next_stable_generation(uint32_t generation, uint32_t& next)
{
    if (generation > std::numeric_limits<uint32_t>::max() - 2) return false;
    next = generation + 2;
    return true;
}

bool parse_cpu_list(const std::string& text, uint64_t& mask)
{
    uint64_t result = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string token = trim(
            text.substr(start, comma == std::string::npos ? std::string::npos
                                                          : comma - start));
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        if (token.empty()) continue;

        const size_t dash = token.find('-');
        uint64_t low = 0;
        uint64_t high = 0;
        if (dash == std::string::npos) {
            if (!parse_uint(token, low)) return false;
            high = low;
        } else if (!parse_uint(trim(token.substr(0, dash)), low) ||
                   !parse_uint(trim(token.substr(dash + 1)), high) ||
                   low > high) {
            return false;
        }
        // CPUs beyond the mask width are ignored, as in BpfLoader.
        for (uint64_t cpu = low; cpu <= high && cpu < kMaxCpus; ++cpu) {
            result |= 1ULL << cpu;
        }
    }
    mask = result;
    return true;
}

uint64_t detect_isolated_cpu_mask()
{
    const uint64_t mask =
        parse_or_zero(read_first_line("/sys/fs/cgroup/cpuset.cpus.isolated"));
    return mask != 0 ? mask : scan_isolated_partitions("/sys/fs/cgroup");
}

std::map<std::string, std::vector<int32_t>> scan_proc_pids(
    const std::set<std::string>& names)
{
    std::map<std::string, std::vector<int32_t>> found;
    DIR* proc = opendir("/proc");
    if (!proc) return found;
    while (struct dirent* entry = readdir(proc)) {
        if (entry->d_type != DT_DIR || !is_number(entry->d_name)) continue;
        const std::string dir = std::string("/proc/") + entry->d_name;
        std::ifstream comm_file(dir + "/comm");
        std::string comm;
        if (!comm_file.is_open() || !std::getline(comm_file, comm)) continue;
        if (names.count(comm) == 0 || !is_live(dir)) continue;
        uint64_t pid = 0;
        if (parse_uint(entry->d_name, pid) && pid > 0) {
            found[comm].push_back(static_cast<int32_t>(pid));
        }
    }
    closedir(proc);
    for (auto& item : found) std::sort(item.second.begin(), item.second.end());
    return found;
}

bool read_shm_generation(uint32_t& generation)
{
    const int fd = shm_open("/timpani_ttsched", O_RDONLY, 0);
    if (fd < 0) return false;
    void* addr = mmap(nullptr, sizeof(struct timpani_ttsched_shm), PROT_READ,
                      MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) return false;
    const auto* shm = static_cast<const struct timpani_ttsched_shm*>(addr);
    generation = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    munmap(addr, sizeof(struct timpani_ttsched_shm));
    return true;
}

uint64_t realtime_now_ns()
{
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

bool prepare_table(const Table& table, const ActiveBaseline& active,
                   const NodeEnvironment& env, NPrepared& out,
                   std::string& error)
{
    if (table.table_id().empty()) return fail(error, "empty table_id");
    if (table.node_id() != env.node_id) {
        return fail(error, "node_id mismatch: expected '" + env.node_id +
                               "', got '" + table.node_id() + "'");
    }
    if (table.hyperperiod_us() == 0) {
        return fail(error, "hyperperiod_us must be positive");
    }
    if (active.has_table && table.epoch_ns() != active.epoch_ns) {
        return fail(error, "epoch_ns differs from the active table");
    }
    if (active.has_table && table.hyperperiod_us() != active.hyperperiod_us) {
        return fail(error, "hyperperiod_us differs from the active table");
    }

    std::set<uint32_t> cpus;
    size_t slot_count = 0;
    for (const auto& partition : table.partitions()) {
        bool has_slots = false;
        for (const auto& layer : partition.layers())
            has_slots = has_slots || layer.tt_slots_size() != 0;
        if (partition.cpuset().cpus_size() == 0 && !has_slots) continue;
        if (!partition.cpuset().isolated() ||
            partition.cpuset().cpus_size() != 1) {
            return fail(error,
                        "partition must contain one isolated CPU");
        }
        const uint32_t partition_cpu = partition.cpuset().cpus(0);
        cpus.insert(partition_cpu);
        for (const auto& layer : partition.layers()) {
            if (layer.model() != timpani::node::v1::TIME_TRIGGERED) {
                return fail(error, "non-TT layer is outside Phase 0");
            }
            if (layer.cbs_entries_size() > 0) {
                return fail(error, "CBS entries are outside Phase 0");
            }
            slot_count += static_cast<size_t>(layer.tt_slots_size());
            if (slot_count > kMaxTtSlots) {
                return fail(error, "TT slot capacity exceeds 2048");
            }
        }
    }

    NPrepared prepared;
    prepared.table_id = table.table_id();
    prepared.epoch_ns = table.epoch_ns();
    prepared.hyperperiod_us = table.hyperperiod_us();
    prepared.slots.reserve(slot_count);

    std::map<std::string, uint64_t> workload_by_id;
    std::map<uint64_t, std::string> workload_by_hash;
    std::map<std::string, uint64_t> task_by_id;
    std::map<uint64_t, std::string> task_by_hash;
    std::map<uint64_t, uint64_t> task_workload;
    std::map<uint64_t, uint32_t> task_cpu;
    uint32_t slot_idx = 0;
    for (const auto& partition : table.partitions()) {
        if (partition.cpuset().cpus_size() == 0) continue;
        const uint32_t partition_cpu = partition.cpuset().cpus(0);
        for (const auto& layer : partition.layers()) {
            for (const auto& slot : layer.tt_slots()) {
                const std::string where =
                    "TT slot " + std::to_string(slot_idx) + ": ";
                if (!valid_identifier(slot.workload_id()) ||
                    !valid_identifier(slot.task_id())) {
                    return fail(error, where + "IDs must be 1..15-byte ASCII");
                }
                if (slot.duration_us() == 0 || slot.deadline_us() == 0) {
                    return fail(error, where + "duration/deadline must be > 0");
                }
                if (!slot_fits_hyperperiod(slot.offset_us(), slot.duration_us(),
                                           table.hyperperiod_us())) {
                    return fail(error, where + "slot end exceeds hyperperiod");
                }
                if (slot.cpu() != partition_cpu) {
                    return fail(error, where + "slot CPU differs from partition");
                }
                if (!is_isolated(env.isolated_cpu_mask, slot.cpu())) {
                    return fail(error, where + "CPU " +
                                           std::to_string(slot.cpu()) +
                                           " is not isolated");
                }
                if (!bind_identity(workload_by_id, workload_by_hash,
                                   slot.workload_id(),
                                   slot.workload_id_hash())) {
                    return fail(error, "workload ID/hash conflict: " +
                                           slot.workload_id());
                }
                if (!bind_identity(task_by_id, task_by_hash, slot.task_id(),
                                   slot.task_id_hash())) {
                    return fail(error,
                                "task ID/hash conflict: " + slot.task_id());
                }
                auto owner = task_workload.emplace(slot.task_id_hash(),
                                                   slot.workload_id_hash());
                if (owner.first->second != slot.workload_id_hash()) {
                    return fail(error, "task belongs to multiple workloads: " +
                                           slot.task_id());
                }
                auto assigned = task_cpu.emplace(slot.task_id_hash(), slot.cpu());
                if (!assigned.second && assigned.first->second != slot.cpu()) {
                    return fail(error, "task moves between CPUs: " +
                                           slot.task_id());
                }
                cpus.insert(slot.cpu());

                PreparedSlot entry;
                entry.offset_us = slot.offset_us();
                entry.duration_us = slot.duration_us();
                entry.deadline_us = slot.deadline_us();
                entry.offset_ns =
                    static_cast<uint64_t>(slot.offset_us()) * 1000ULL;
                entry.duration_ns =
                    static_cast<uint64_t>(slot.duration_us()) * 1000ULL;
                entry.cpu = slot.cpu();
                entry.slot_idx = slot_idx++;
                entry.workload_id_hash = slot.workload_id_hash();
                entry.task_id_hash = slot.task_id_hash();
                entry.current_limit = slot.current_limit();
                entry.layer_index = layer.layer_index();
                prepared.slots.push_back(entry);
            }
        }
    }

    if (!cpus.empty()) {
        for (uint32_t cpu : cpus) {
            if (!is_isolated(env.isolated_cpu_mask, cpu)) {
                return fail(error,
                            "CPU " + std::to_string(cpu) + " is not isolated");
            }
        }
        prepared.cpus = cpus;
        prepared.has_cpu = true;
        prepared.cpu = *cpus.begin();
    } else {
        prepared.cpus = active.cpus;
        prepared.has_cpu = active.has_cpu;
        prepared.cpu = active.cpu;
    }
    if (task_by_hash.size() > kMaxTasks) {
        return fail(error, "unique TT task capacity exceeds 32");
    }

    for (const auto& [hash, record] : active.tasks) {
        auto by_hash = task_by_hash.find(hash);
        auto by_id = task_by_id.find(record.task_id);
        if ((by_hash != task_by_hash.end() &&
             by_hash->second != record.task_id) ||
            (by_id != task_by_id.end() && by_id->second != hash)) {
            return fail(error,
                        "task ID/hash conflicts with the active table: " +
                            record.task_id);
        }
        if (by_hash != task_by_hash.end() &&
            task_workload.at(hash) != record.workload_id_hash) {
            return fail(error, "task workload differs from the active table: " +
                                   record.task_id);
        }
    }
    for (const auto& [hash, id] : active.workloads) {
        auto by_hash = workload_by_hash.find(hash);
        auto by_id = workload_by_id.find(id);
        if ((by_hash != workload_by_hash.end() && by_hash->second != id) ||
            (by_id != workload_by_id.end() && by_id->second != hash)) {
            return fail(
                error,
                "workload ID/hash conflicts with the active table: " + id);
        }
        if (by_hash != workload_by_hash.end() &&
            workload_slots(active.slots, hash) !=
                workload_slots(prepared.slots, hash)) {
            return fail(error,
                        "retained workload slot parameters changed: " + id);
        }
    }

    if (!select_boundary(
            table.epoch_ns(),
            static_cast<uint64_t>(table.hyperperiod_us()) * 1000ULL, env.now_ns,
            prepared.boundary)) {
        return fail(error, "target boundary exceeds the uint64 time range");
    }
    if (!next_stable_generation(env.shm_generation,
                                prepared.shm.next_generation)) {
        return fail(error, "SHM generation would wrap");
    }
    prepared.shm.base_generation = env.shm_generation;

    std::map<std::string, std::vector<int32_t>> resolved;
    if (!task_by_id.empty()) {
        if (!env.resolve_pids) return fail(error, "PID resolver unavailable");
        std::set<std::string> names;
        for (const auto& item : task_by_id) names.insert(item.first);
        resolved = env.resolve_pids(names);
    }
    std::set<int32_t> used_pids;
    for (const auto& [id, hash] : task_by_id) {
        auto found = resolved.find(id);
        if (found == resolved.end() || found->second.empty()) {
            return fail(error, "task not found: " + id);
        }
        if (found->second.size() != 1) {
            return fail(error, "task resolves to multiple PIDs: " + id);
        }
        const int32_t pid = found->second.front();
        if (pid <= 0 || !used_pids.insert(pid).second) {
            return fail(error, "task PID is invalid or shared: " + id);
        }
        const uint32_t assigned_cpu = task_cpu.at(hash);
        auto retained = active.tasks.find(hash);
        if (retained != active.tasks.end() && retained->second.pid != pid) {
            return fail(error, "retained task PID changed: " + id);
        }
        if (retained != active.tasks.end() &&
            retained->second.assigned_cpu != assigned_cpu) {
            return fail(error, "retained task CPU changed: " + id);
        }
        TaskRecord record;
        record.task_id = id;
        record.workload_id_hash = task_workload.at(hash);
        record.workload_id = workload_by_hash.at(record.workload_id_hash);
        record.pid = pid;
        record.assigned_cpu = assigned_cpu;
        prepared.tasks.emplace(hash, std::move(record));
    }

    std::sort(prepared.slots.begin(), prepared.slots.end(), slot_less);

    // Retained tasks keep their SHM index. New tasks prefer indexes unused by
    // the active table and reuse a just-removed index only when necessary.
    std::array<bool, kMaxTasks> used{};
    std::array<bool, kMaxTasks> active_used{};
    for (const auto& [hash, record] : active.tasks) {
        if (record.shm_index >= kMaxTasks) {
            return fail(error,
                        "active SHM index is invalid: " + record.task_id);
        }
        active_used[record.shm_index] = true;
        auto task = prepared.tasks.find(hash);
        if (task == prepared.tasks.end()) continue;
        if (used[record.shm_index]) {
            return fail(error, "active SHM index is shared: " + record.task_id);
        }
        used[record.shm_index] = true;
        task->second.shm_index = record.shm_index;
    }
    std::set<uint64_t> placed;
    for (const auto& slot : prepared.slots) {
        const uint64_t hash = slot.task_id_hash;
        if (active.tasks.count(hash) != 0 || !placed.insert(hash).second) {
            continue;
        }
        size_t index = kMaxTasks;
        for (bool avoid_removed : {true, false}) {
            for (size_t i = 0; i < kMaxTasks && index == kMaxTasks; ++i) {
                if (!used[i] && !(avoid_removed && active_used[i])) index = i;
            }
        }
        if (index == kMaxTasks) {
            return fail(error, "unique TT task capacity exceeds 32");
        }
        used[index] = true;
        prepared.tasks.at(hash).shm_index = static_cast<uint32_t>(index);
    }
    for (const auto& [hash, record] : prepared.tasks) {
        ShmEntry& entry = prepared.shm.entries[record.shm_index];
        entry.occupied = true;
        entry.task_id_hash = hash;
        std::memcpy(entry.name.data(), record.task_id.data(),
                    record.task_id.size());
        prepared.shm.n_tasks =
            std::max(prepared.shm.n_tasks, record.shm_index + 1);
    }

    for (const auto& [hash, record] : active.tasks) {
        if (prepared.tasks.count(hash) == 0) {
            prepared.removed_tasks.emplace(hash, record);
        }
    }
    for (const auto& [hash, id] : active.workloads) {
        if (workload_by_hash.count(hash) == 0) {
            prepared.removed_workloads.emplace(hash, id);
        }
    }
    prepared.workloads = std::move(workload_by_hash);

    out = std::move(prepared);
    return true;
}

ActiveBaseline baseline_from(const NPrepared& prepared)
{
    ActiveBaseline active;
    active.has_table = true;
    active.epoch_ns = prepared.epoch_ns;
    active.hyperperiod_us = prepared.hyperperiod_us;
    active.has_cpu = prepared.has_cpu;
    active.cpu = prepared.cpu;
    active.cpus = prepared.cpus;
    active.slots = prepared.slots;
    active.tasks = prepared.tasks;
    active.workloads = prepared.workloads;
    return active;
}

bool AttemptSlot::offer(const Table& table)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_) return false;
        busy_ = true;
    }
    Table copy(table);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.Swap(&copy);
        ready_ = true;
    }
    ready_cv_.notify_one();
    return true;
}

bool AttemptSlot::wait_take(Table& out, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_cv_.wait_for(lock, timeout, [this] { return ready_; })) {
        return false;
    }
    ready_ = false;
    out.Clear();
    out.Swap(&table_);
    return true;
}

void AttemptSlot::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    ready_ = false;
    table_.Clear();
}

bool AttemptSlot::busy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

}  // namespace phase0
}  // namespace node
}  // namespace timpani
