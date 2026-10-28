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
#include "calc_workload.h"

static std::shared_ptr<vsomeip::application> app;
static std::atomic<bool> run_consumer{true};
static uint32_t overrun_ms = 0;
static uint32_t wakeup_delay_ms = 0;
static bool alarm_active = false;

struct RadarTarget {
    uint32_t seq;
    uint32_t id;
    float distance_meters;
    float velocity_mps;
};

static std::mutex cbs_mtx;
static std::condition_variable cbs_cv;
static std::queue<RadarTarget> cbs_queue;

void signal_handler(int sig) {
    std::cout << "\n[fcw_consumer] Shutting down..." << std::endl;
    run_consumer = false;
    if (app) {
        app->stop();
    }
    cbs_cv.notify_all();
}

void cbs_worker_loop() {
    pthread_setname_np(pthread_self(), "cbs_task");
    std::cout << "[fcw_consumer] CBS worker thread started and named 'cbs_task'" << std::endl;
    std::cout << "[fcw_consumer] Waiting for event arrival; CPU budget enforced by timpani-n (CBS)..." << std::endl;

    while (run_consumer) {
        RadarTarget target;
        {
            std::unique_lock<std::mutex> lock(cbs_mtx);
            cbs_cv.wait(lock, [] { return !cbs_queue.empty() || !run_consumer; });
            if (!run_consumer && cbs_queue.empty()) {
                break;
            }
            target = cbs_queue.front();
            cbs_queue.pop();
        }

        // 2. Bind current thread to L2 CBS Server
        if (wakeup_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wakeup_delay_ms));
        }
        timpani_cbs_bind_thread("fcw_workload", "cbs_task");

        // 3. Simulate WCET / Overrun (if enabled via --overrun-ms)
        if (overrun_ms > 0) {
            std::cout << "[fcw_consumer] Simulating CBS budget overrun: busy looping for " 
                      << overrun_ms << " ms..." << std::endl;
            run_nsqrt_for_cpu_ms(overrun_ms);
        } else {
            // Nominal computation, same nsqrt algo as sample_apps/producer
            run_nsqrt_loops(FCW_CALC_LOOP_COUNT);
        }

        // 4. FCW Hysteresis & Collision Alert Logic
        if (!alarm_active && target.distance_meters < 15.0f && target.velocity_mps < 0.0f) {
            alarm_active = true;
            std::cout << "\n>>> [WARN: COLLISION ALERT ON] <<< Target distance: " 
                      << target.distance_meters << " m, approaching velocity: " 
                      << target.velocity_mps << " m/s\n" << std::endl;
        } else if (alarm_active && target.distance_meters > 18.0f) {
            alarm_active = false;
            std::cout << "\n>>> [COLLISION ALERT OFF] <<< Target distance: " 
                      << target.distance_meters << " m (Safety distance restored)\n" << std::endl;
        }

        std::cout << "[fcw_consumer] Processed Frame #" << target.seq 
                  << " | Target Distance: " << std::fixed << std::setprecision(2) << target.distance_meters << " m"
                  << " | Alarm Status: " << (alarm_active ? "RED_ALERT" : "SAFE") << std::endl;

        // 5. Unbind callback thread
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

    // Push data to queue and wake up cbs worker
    {
        std::lock_guard<std::mutex> lock(cbs_mtx);
        cbs_queue.push(*target);
    }
    cbs_cv.notify_one();
}

int main(int argc, char* argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--overrun-ms" && i + 1 < argc) {
            overrun_ms = std::stoul(argv[++i]);
            std::cout << "[fcw_consumer] Simulated overrun duration set to " 
                      << overrun_ms << " ms (CBS capacity Cs = 2ms)" << std::endl;
        } else if (arg == "--wakeup-delay-ms" && i + 1 < argc) {
            wakeup_delay_ms = std::stoul(argv[++i]);
            std::cout << "[fcw_consumer] Injected wakeup delay: " << wakeup_delay_ms << " ms (limit 3 ms)" << std::endl;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -h, --help        Show help\n"
                      << "  --overrun-ms <ms> Simulate thread execution overhead (Default 0ms. Set >2 to test Throttling)\n"
                      << "  --wakeup-delay-ms <ms> Delay before CBS bind to exceed the 3ms wakeup-latency limit\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize TIMPANI real-time library
    timpani_rt_init();

    // Spawn dedicated CBS worker thread before starting someip app
    std::thread cbs_worker(cbs_worker_loop);

    // MIT = P - WCET_producer = 20ms - 2ms = 18ms (DDR-008); MIT == P flags TT jitter as violations
    timpani_evt_register("fcw_workload", "cbs_task", "0x1024", "0x0001", "0x8001", 18000);

    app = vsomeip::runtime::get()->create_application("fcw_consumer");
    app->init();

    app->register_state_handler([](vsomeip::state_type_e state) {
        if (state == vsomeip::state_type_e::ST_REGISTERED) {
            std::cout << "[fcw_consumer] Registered with SOME/IP Routing Manager" << std::endl;
        } else {
            std::cout << "[fcw_consumer] Deregistered from SOME/IP Routing Manager" << std::endl;
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

    std::cout << "[fcw_consumer] Consumer terminated." << std::endl;
    return 0;
}

