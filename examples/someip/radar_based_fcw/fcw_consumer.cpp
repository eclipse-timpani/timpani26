/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <vsomeip/vsomeip.hpp>
#include <timpani_rt.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <csignal>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <pthread.h>
#include <cctype>
#include "calc_workload.h"

/* Injected CPU time: above the 2 ms CBS budget but below the 5 ms deadline, so an unthrottled run is budget-exceeded only. */
#define FCW_BUDGET_EXCEED_MS 3
#define FCW_BUDGET_EXCEED_DEFAULT_EVERY 30
#define FCW_CBS_BUDGET_MS 2.0
#define FCW_CBS_DEADLINE_MS 5.0

static std::shared_ptr<vsomeip::application> app;
static std::atomic<bool> run_consumer{true};
static uint32_t overrun_ms = 0;
static uint32_t wakeup_delay_ms = 0;
static uint32_t budget_exceed_every = 0; // 0 = off; N = every Nth processed event
static bool alarm_active = false;

struct RadarTarget {
    uint32_t seq;
    uint32_t id;
    float distance_meters;
    float velocity_mps;
};

struct QueuedTarget {
    RadarTarget target;
    uint64_t arrival_ns;
};

static std::mutex cbs_mtx;
static std::condition_variable cbs_cv;
static std::queue<QueuedTarget> cbs_queue;

static uint64_t clock_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void signal_handler(int sig) {
    std::cout << "\n[fcw] Shutting down..." << std::endl;
    run_consumer = false;
    if (app) {
        app->stop();
    }
    cbs_cv.notify_all();
}

void cbs_worker_loop() {
    pthread_setname_np(pthread_self(), "cbs_task");

    uint64_t processed = 0;

    while (run_consumer) {
        QueuedTarget item;
        {
            std::unique_lock<std::mutex> lock(cbs_mtx);
            cbs_cv.wait(lock, [] { return !cbs_queue.empty() || !run_consumer; });
            if (!run_consumer && cbs_queue.empty()) {
                break;
            }
            item = cbs_queue.front();
            cbs_queue.pop();
        }
        const RadarTarget& target = item.target;

        if (wakeup_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wakeup_delay_ms));
        }
        timpani_cbs_bind_thread("fcw_workload", "cbs_task");

        const uint64_t start_ns = clock_ns(CLOCK_MONOTONIC);
        const uint64_t cpu0_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);

        ++processed;
        bool injected = false;
        if (overrun_ms > 0) {
            run_nsqrt_for_cpu_ms(overrun_ms);
            injected = true;
        } else if (budget_exceed_every > 0 && processed % budget_exceed_every == 0) {
            run_nsqrt_for_cpu_ms(FCW_BUDGET_EXCEED_MS);
            injected = true;
        } else {
            run_nsqrt_loops(FCW_CALC_LOOP_COUNT);
        }

        const double cpu_ms = (clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0_ns) / 1e6;
        const uint64_t done_ns = clock_ns(CLOCK_MONOTONIC);
        const double wait_ms = (start_ns - item.arrival_ns) / 1e6;
        const double resp_ms = (done_ns - item.arrival_ns) / 1e6;

        // FCW hysteresis: ON below 15 m while approaching, OFF above 18 m
        if (!alarm_active && target.distance_meters < 15.0f && target.velocity_mps < 0.0f) {
            alarm_active = true;
            std::cout << "[fcw] >>> COLLISION ALERT ON  (distance " << target.distance_meters << " m, velocity "
                      << target.velocity_mps << " m/s)" << std::endl;
        } else if (alarm_active && target.distance_meters > 18.0f) {
            alarm_active = false;
            std::cout << "[fcw] <<< COLLISION ALERT OFF (distance " << target.distance_meters << " m)" << std::endl;
        }

        std::cout << "[fcw] frame " << std::setw(5) << target.seq
                  << " | " << std::fixed << std::setprecision(2) << std::setw(5) << target.distance_meters << " m"
                  << " | " << (alarm_active ? "ALERT" : "SAFE ")
                  << " | wait " << std::setw(5) << wait_ms << " ms"
                  << " | cpu " << std::setw(5) << cpu_ms << " ms"
                  << " | resp " << std::setw(6) << resp_ms << " ms"
                  << (injected ? " | INJECTED" : "")
                  << (cpu_ms > FCW_CBS_BUDGET_MS ? " | OVER_BUDGET" : "")
                  << (resp_ms > FCW_CBS_DEADLINE_MS ? " | DEADLINE_MISS" : "")
                  << std::endl;

        timpani_cbs_unbind_thread();
    }
}

void on_obstacle_alert_received(const std::shared_ptr<vsomeip::message>& msg) {
    auto payload = msg->get_payload();
    if (!payload || payload->get_length() < sizeof(RadarTarget)) {
        return;
    }

    const RadarTarget* target = reinterpret_cast<const RadarTarget*>(payload->get_data());

    // 1. Mark Event Arrival (t0 marking at Dispatch boundary)
    timpani_evt_mark_arrival("0x1024", "0x0001", "0x8001", target->seq);
    const uint64_t arrival_ns = clock_ns(CLOCK_MONOTONIC);

    // Push data to queue and wake up cbs worker
    {
        std::lock_guard<std::mutex> lock(cbs_mtx);
        cbs_queue.push(QueuedTarget{*target, arrival_ns});
    }
    cbs_cv.notify_one();
}

int main(int argc, char* argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--overrun-ms" && i + 1 < argc) {
            overrun_ms = std::stoul(argv[++i]);
            std::cout << "[fcw] Overrun: every event burns "
                      << overrun_ms << " ms CPU (budget 2 ms)" << std::endl;
        } else if (arg == "--wakeup-delay-ms" && i + 1 < argc) {
            wakeup_delay_ms = std::stoul(argv[++i]);
            std::cout << "[fcw] Wakeup delay: " << wakeup_delay_ms << " ms before CBS bind (limit 3 ms)" << std::endl;
        } else if (arg == "-be" || arg == "--budgetexceed") {
            budget_exceed_every = FCW_BUDGET_EXCEED_DEFAULT_EVERY;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                budget_exceed_every = std::stoul(argv[++i]);
                if (budget_exceed_every == 0) budget_exceed_every = FCW_BUDGET_EXCEED_DEFAULT_EVERY;
            }
            std::cout << "[fcw] Budget exceed: every " << budget_exceed_every
                      << "th event burns " << FCW_BUDGET_EXCEED_MS << " ms CPU (budget 2 ms)" << std::endl;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -h, --help        Show help\n"
                      << "  --overrun-ms <ms> Simulate thread execution overhead (Default 0ms. Set >2 to test Throttling)\n"
                      << "  --wakeup-delay-ms <ms> Delay before CBS bind to exceed the 3ms wakeup-latency limit\n"
                      << "  -be, --budgetexceed [N] Every Nth processed event (default 30) burns 3 ms CPU instead of ~1.5 ms\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize TIMPANI real-time library
    timpani_rt_init();

    std::cout << "[fcw] CBS worker 'cbs_task': budget " << FCW_CBS_BUDGET_MS << " ms, deadline "
              << FCW_CBS_DEADLINE_MS << " ms\n"
              << "[fcw] per frame: wait = arrival->start, cpu = CPU time used, resp = arrival->done (includes throttling)"
              << std::endl;

    // Spawn dedicated CBS worker thread before starting someip app
    std::thread cbs_worker(cbs_worker_loop);

    // MIT = P - WCET_producer = 20ms - 2ms = 18ms (DDR-008); MIT == P flags TT jitter as violations
    timpani_evt_register("fcw_workload", "cbs_task", "0x1024", "0x0001", "0x8001", 18000);

    app = vsomeip::runtime::get()->create_application("fcw_consumer");
    app->init();

    app->register_state_handler([](vsomeip::state_type_e state) {
        if (state == vsomeip::state_type_e::ST_REGISTERED) {
            std::cout << "[fcw] Registered with SOME/IP routing manager; waiting for ObstacleAlert events..." << std::endl;
        } else {
            std::cout << "[fcw] Deregistered from SOME/IP routing manager" << std::endl;
        }
    });

    uint32_t service_id = 0x1024;
    uint32_t instance_id = 0x0001;
    uint32_t event_id = 0x8001;

    app->register_message_handler(service_id, instance_id, event_id, on_obstacle_alert_received);

    app->request_service(service_id, instance_id);
    app->subscribe(service_id, instance_id, event_id);

    app->start();

    // Signal thread to exit and join
    {
        std::lock_guard<std::mutex> lock(cbs_mtx);
        run_consumer = false;
    }
    cbs_cv.notify_all();
    if (cbs_worker.joinable()) {
        cbs_worker.join();
    }

    std::cout << "[fcw] Consumer terminated." << std::endl;
    return 0;
}
