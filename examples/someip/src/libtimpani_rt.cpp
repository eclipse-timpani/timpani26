/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <timpani_rt.h>
#include <timpani_rt_shm.h>
#include <libttsched.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <climits>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/futex.h>
#include <iomanip>

#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

/* --- /timpani_rt unified SHM: CBS/Event/Fault state, simulating a real
 * timpani-n until one is actually running. Connects to an existing object
 * if a real timpani-n already publishes it; otherwise creates+owns it. --- */
namespace {

struct EventContract {
    uint64_t cbs_key;
    uint64_t mit_ns;
    uint64_t last_arrival_ns;
};

volatile struct timpani_rt_shm* g_shm = nullptr;
int g_shm_fd = -1;

std::mutex g_contract_mutex;
std::unordered_map<uint64_t, EventContract> g_contracts;

thread_local uint64_t g_current_cbs_key = 0;
thread_local int g_current_cbs_slot = -1;

uint64_t monotonic_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

/* Opens /timpani_rt if a real timpani-n already published it, otherwise
 * creates+zero-inits it ourselves (best-effort; not race-proof across
 * concurrently-starting processes, acceptable for example/simulation use). */
bool shm_connect() {
    if (g_shm) return true;

    bool created = false;
    int fd = shm_open(TIMPANI_RT_SHM_PATH, O_RDWR, 0666);
    if (fd < 0) {
        fd = shm_open(TIMPANI_RT_SHM_PATH, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd >= 0) {
            created = true;
        } else if (errno == EEXIST) {
            fd = shm_open(TIMPANI_RT_SHM_PATH, O_RDWR, 0666);
        }
    }
    if (fd < 0) return false;

    if (created && ftruncate(fd, TIMPANI_RT_SHM_SIZE) != 0) {
        close(fd);
        return false;
    }

    void* mem = mmap(NULL, TIMPANI_RT_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        return false;
    }

    g_shm_fd = fd;
    g_shm = static_cast<volatile struct timpani_rt_shm*>(mem);

    if (created || g_shm->magic != TIMPANI_RT_MAGIC) {
        memset(const_cast<struct timpani_rt_shm*>((struct timpani_rt_shm*)mem), 0, TIMPANI_RT_SHM_SIZE);
        g_shm->version = TIMPANI_RT_VERSION;
        g_shm->flags = TIMPANI_FLAG_CBS_ENABLED | TIMPANI_FLAG_EVENT_ENABLED;
        __atomic_store_n(&g_shm->magic, TIMPANI_RT_MAGIC, __ATOMIC_RELEASE);
    }
    return true;
}

/* DDR-008 §2.3: allowed t0 (arrival) -> t1 (CBS thread bound) latency. */
constexpr uint64_t E2E_LATENCY_LIMIT_NS = 3000000ULL;

/* Lock-free enqueue into fault_rb, mirrors DDR-008-LLD's emit_mit_violation(). */
void emit_fault(uint32_t type, uint64_t event_key, uint64_t measured_ns, uint64_t limit_ns) {
    if (!g_shm) return;
    uint32_t head = __atomic_load_n(&g_shm->fault_rb.head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&g_shm->fault_rb.tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= TIMPANI_MAX_FAULTS) return; /* ring full, drop */

    volatile struct timpani_fault_event* fault = &g_shm->fault_rb.faults[head & TIMPANI_FAULT_RINGBUF_MASK];
    fault->type = type;
    fault->event_key = event_key;
    fault->measured_ns = measured_ns;
    fault->limit_ns = limit_ns;
    fault->timestamp_ns = monotonic_now_ns();

    __atomic_store_n(&g_shm->fault_rb.head, head + 1, __ATOMIC_RELEASE);
}

void emit_mit_violation(uint64_t event_key, uint64_t interval_ns, uint64_t mit_ns) {
    emit_fault(TIMPANI_FAULT_MIT_VIOLATION, event_key, interval_ns, mit_ns);
}

} // namespace

static uint64_t fnv1a_64(const std::string& str) {
    uint64_t hash = FNV_OFFSET_BASIS;
    for (char c : str) {
        hash = (hash ^ static_cast<uint8_t>(c)) * FNV_PRIME;
    }
    return hash;
}

static std::string normalize_id(const std::string& id) {
    if (id.rfind("0x", 0) == 0 || id.rfind("0X", 0) == 0) {
        try {
            unsigned long val = std::stoul(id, nullptr, 16);
            return std::to_string(val);
        } catch (...) {
            return id;
        }
    }
    return id;
}

static pid_t get_tid() {
    return syscall(SYS_gettid);
}

extern "C" {

int timpani_rt_init(void) {
    bool shm_ok = shm_connect();
    std::cout << "[libtimpani_rt] timpani_rt_init: Library initialized successfully. "
              << "(/timpani_rt SHM: " << (shm_ok ? "connected" : "unavailable, logging-only fallback") << ")" << std::endl;
    return 0;
}

int timpani_tt_register(const char* workload_id, const char* task_id) {
    // Set thread name (PR_SET_NAME) so TimerMaster's ttsched SHM lookup finds it.
    // In Linux, the thread name is limited to 16 bytes (including NUL).
    prctl(PR_SET_NAME, task_id, 0, 0, 0);
    
    std::cout << "[libtimpani_rt] timpani_tt_register: Registering L1 TT -> workload: \"" 
              << workload_id << "\", task (thread name): \"" << task_id << "\"" << std::endl;
              
    // Also initialize the ttsched SHM mapping for the calling thread
    ttsched_init();
    return 0;
}

int timpani_tt_wait_period(void) {
    // Call the actual TIMPANI ttsched futex wait implementation
    int ret = ttsched_wait_next_period();
    if (ret != 0) {
        // Fall back to a standard sleep if TimerMaster is not active or SHM not published yet
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

int timpani_evt_register(
    const char* workload_id,
    const char* task_id,
    const char* service_id,
    const char* instance_id,
    const char* event_id,
    uint32_t mit_us
) {
    std::string norm_service = normalize_id(service_id);
    std::string norm_instance = normalize_id(instance_id);
    std::string norm_event = normalize_id(event_id);
    
    std::string event_str = norm_service + ":" + norm_instance + ":" + norm_event;
    uint64_t event_key = fnv1a_64(event_str);
    
    std::string cbs_str = std::string(workload_id) + ":" + std::string(task_id);
    uint64_t cbs_key = fnv1a_64(cbs_str);

    shm_connect();
    {
        std::lock_guard<std::mutex> lock(g_contract_mutex);
        g_contracts[event_key] = EventContract{cbs_key, static_cast<uint64_t>(mit_us) * 1000ULL, 0};
    }

    std::cout << "[libtimpani_rt] timpani_evt_register: Registered L2 Contract" << std::endl;
    std::cout << "  - Workload/Task: \"" << workload_id << "\" / \"" << task_id << "\"" << std::endl;
    std::cout << "  - cbs_key:       0x" << std::hex << cbs_key << " (\"" << cbs_str << "\")" << std::endl;
    std::cout << "  - Service/Inst/Evt: 0x" << service_id << " / 0x" << instance_id << " / 0x" << event_id << std::endl;
    std::cout << "  - event_key:     0x" << event_key << " (\"" << event_str << "\")" << std::dec << std::endl;
    std::cout << "  - mit:           " << mit_us << " us" << std::endl;
    
    return 0;
}

int timpani_evt_mark_arrival(
    const char* service_id,
    const char* instance_id,
    const char* event_id,
    uint32_t seq
) {
    std::string norm_service = normalize_id(service_id);
    std::string norm_instance = normalize_id(instance_id);
    std::string norm_event = normalize_id(event_id);
    
    std::string event_str = norm_service + ":" + norm_instance + ":" + norm_event;
    uint64_t event_key = fnv1a_64(event_str);

    uint64_t now_ns = monotonic_now_ns();

    bool violated = false;
    uint64_t interval_ns = 0, mit_ns = 0;
    {
        std::lock_guard<std::mutex> lock(g_contract_mutex);
        auto it = g_contracts.find(event_key);
        if (it != g_contracts.end()) {
            if (it->second.last_arrival_ns > 0) {
                interval_ns = now_ns - it->second.last_arrival_ns;
                mit_ns = it->second.mit_ns;
                violated = interval_ns < mit_ns;
            }
            it->second.last_arrival_ns = now_ns;
        }
    }

    if (g_shm) {
        uint32_t head = __atomic_load_n(&g_shm->events.head, __ATOMIC_RELAXED);
        volatile struct timpani_event_arrival* slot = &g_shm->events.arrivals[head & TIMPANI_EVENTS_RINGBUF_MASK];
        slot->event_key = event_key;
        slot->arrival_ns = now_ns;
        slot->seq = seq;
        __atomic_store_n(&g_shm->events.head, head + 1, __ATOMIC_RELEASE);

        // Wake any thread FUTEX_WAIT'ing on the arrivals ring, mirroring
        // TimerMaster's counter/FUTEX_WAKE pattern for the TT path.
        syscall(SYS_futex, (uint32_t*)&g_shm->events.head, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);

        if (violated) emit_mit_violation(event_key, interval_ns, mit_ns);
    }

    std::cout << "[libtimpani_rt] timpani_evt_mark_arrival: event_key = 0x" << std::hex << event_key 
              << " (\"" << event_str << "\")" << std::dec 
              << ", seq = " << seq 
              << ", timestamp = " << now_ns << " ns (t0 마킹)" << std::endl;
    if (violated) {
        std::cout << "[libtimpani_rt] timpani_evt_mark_arrival: MIT VIOLATION interval="
                  << (interval_ns / 1000) << "us < mit=" << (mit_ns / 1000) << "us -> fault_rb" << std::endl;
    }

    return 0;
}

int timpani_cbs_bind_thread(const char* workload_id, const char* task_id) {
    pid_t tid = get_tid();
    std::string cbs_str = std::string(workload_id) + ":" + std::string(task_id);
    uint64_t cbs_key = fnv1a_64(cbs_str);

    int slot = -1;
    if (shm_connect()) {
        for (uint32_t i = 0; i < g_shm->cbs.n_tasks; i++) {
            if (g_shm->cbs.slots[i].cbs_key == cbs_key) { slot = static_cast<int>(i); break; }
        }
        if (slot < 0) {
            uint32_t n = __atomic_load_n(&g_shm->cbs.n_tasks, __ATOMIC_RELAXED);
            if (n < TIMPANI_MAX_CBS_TASKS) {
                slot = static_cast<int>(n);
                g_shm->cbs.slots[slot].cbs_key = cbs_key;
                __atomic_store_n(&g_shm->cbs.n_tasks, n + 1, __ATOMIC_RELEASE);
            }
        }
        if (slot >= 0) {
            g_shm->cbs.slots[slot].tid = static_cast<uint32_t>(tid);
            __atomic_store_n(&g_shm->cbs.slots[slot].state, static_cast<uint32_t>(TIMPANI_CBS_ACTIVE), __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_shm->generation, 1, __ATOMIC_RELEASE);
        }
    }
    g_current_cbs_key = cbs_key;
    g_current_cbs_slot = slot;

    // FR-041: t1 - t0 against the latest arrival of this task's event contract (approximation under backlog).
    uint64_t e2e_event_key = 0, e2e_ns = 0;
    {
        std::lock_guard<std::mutex> lock(g_contract_mutex);
        for (const auto& kv : g_contracts) {
            if (kv.second.cbs_key == cbs_key && kv.second.last_arrival_ns > 0) {
                e2e_event_key = kv.first;
                e2e_ns = monotonic_now_ns() - kv.second.last_arrival_ns;
                break;
            }
        }
    }
    const bool e2e_exceeded = e2e_event_key != 0 && e2e_ns > E2E_LATENCY_LIMIT_NS;
    if (e2e_exceeded) emit_fault(TIMPANI_FAULT_E2E_EXCEED, e2e_event_key, e2e_ns, E2E_LATENCY_LIMIT_NS);

    std::cout << "[libtimpani_rt] timpani_cbs_bind_thread: bound TID = " << tid 
              << " to cbs_key = 0x" << std::hex << cbs_key << std::dec 
              << " (\"" << cbs_str << "\")"
              << (slot >= 0 ? (" [shm cbs.slots[" + std::to_string(slot) + "]]") : std::string(" [shm unavailable]"))
              << std::endl;
    if (e2e_exceeded) {
        std::cout << "[libtimpani_rt] timpani_cbs_bind_thread: WAKEUP/E2E LATENCY EXCEEDED t1-t0="
                  << (e2e_ns / 1000) << "us > limit=" << (E2E_LATENCY_LIMIT_NS / 1000) << "us -> fault_rb" << std::endl;
    }
              
    return 0;
}

int timpani_cbs_unbind_thread(void) {
    pid_t tid = get_tid();

    if (g_shm && g_current_cbs_slot >= 0) {
        volatile struct timpani_cbs_slot* s = &g_shm->cbs.slots[g_current_cbs_slot];
        // Only clear if this slot still belongs to us (TID recycling guard).
        if (s->tid == static_cast<uint32_t>(tid) && s->cbs_key == g_current_cbs_key) {
            s->tid = 0;
            __atomic_store_n(&s->state, static_cast<uint32_t>(TIMPANI_CBS_IDLE), __ATOMIC_RELEASE);
        }
    }
    g_current_cbs_slot = -1;
    g_current_cbs_key = 0;

    std::cout << "[libtimpani_rt] timpani_cbs_unbind_thread: unbound TID = " << tid 
              << " (cbs_tid_map 에서 TID 엔트리 안전 제거)" << std::endl;
    return 0;
}

int timpani_cbs_register_socket(
    int socket_fd, 
    const char* workload_id, 
    const char* task_id, 
    uint32_t mit_us
) {
    std::cout << "[libtimpani_rt] [Legacy] timpani_cbs_register_socket: fd = " << socket_fd 
              << " -> workload: \"" << workload_id << "\", task: \"" << task_id << "\"" << std::endl;
    return 0;
}

}

