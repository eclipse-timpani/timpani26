/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <vsomeip/vsomeip.hpp>
#include <timpani_rt.h>
#include <libttsched.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <csignal>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <vector>
#include <iomanip>
#include <cmath>
#include "calc_workload.h"

/* Same real-time signal number as sample-apps/src/sample_apps.h's SIGNO_TT,
 * used here for our own local timer_create+sigwait period fallback. */
#define TT_LOCAL_TIMER_SIGNO (__SIGRTMIN + 2)

/* Radar-side detection range; must exceed the FCW alarm-off distance (18 m) so the
 * consumer still receives the events it needs to clear the alarm. */
#define ALERT_RANGE_M 15.0f

#define TT_WCET_MS 2.0
#define TT_DEADLINE_MS 20.0
#define TT_JITTER_LIMIT_MS 1.0 /* L1 execution-jitter target */

static uint64_t clock_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

static std::shared_ptr<vsomeip::application> app;
static std::atomic<bool> run_producer{true};

void signal_handler(int sig) {
    std::cout << "\n[mock_radar_producer] Shutting down..." << std::endl;
    run_producer = false;
    if (app) {
        app->stop();
    }
}

struct RadarTarget {
    uint32_t seq;
    uint32_t id;
    float distance_meters;
    float velocity_mps;
};

int main(int argc, char* argv[]) {
    uint32_t interval_ms = 20; // Default 20ms radar scan period
    uint32_t tt_overrun_ms = 0;
    
    // Command line options
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--tt-overrun-ms" && i + 1 < argc) {
            tt_overrun_ms = std::stoul(argv[++i]);
            std::cout << "[mock_radar_producer] Injected TT overrun: +" << tt_overrun_ms << " ms CPU per period" << std::endl;
        } else if (arg == "--burst-ms" && i + 1 < argc) {
            interval_ms = std::stoul(argv[++i]);
            std::cout << "[mock_radar_producer] MIT Violation Mode: Scan interval set to " 
                      << interval_ms << " ms" << std::endl;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -h, --help        Show help\n"
                      << "  --burst-ms <ms>   Set scan period (Default 20ms. Lower to test MIT Violation)\n"
                      << "  --tt-overrun-ms <ms> Extra CPU per period; >18 exceeds the 20ms TT deadline (FAULT_DMISS)\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Block our local periodic-timer signal in this (main) thread BEFORE
    // spawning publish_thread, so the child thread inherits the blocked mask.
    // Linux delivers a process-targeted timer_create() signal to whichever
    // thread does not have it blocked, so it must stay blocked everywhere
    // except where publish_thread calls sigwait() for it.
    sigset_t tt_sig_set;
    sigemptyset(&tt_sig_set);
    sigaddset(&tt_sig_set, TT_LOCAL_TIMER_SIGNO);
    pthread_sigmask(SIG_BLOCK, &tt_sig_set, nullptr);

    // Initialize TIMPANI real-time library for L1 TT simulation
    timpani_rt_init();

    app = vsomeip::runtime::get()->create_application("mock_radar_producer");
    app->init();

    // Offer service 0x1024 (RadarService), instance 0x0001
    uint32_t service_id = 0x1024;
    uint32_t instance_id = 0x0001;
    app->offer_service(service_id, instance_id);

    // Offer event 0x8001 (ObstacleAlert)
    uint32_t event_id = 0x8001;
    app->offer_event(service_id, instance_id, event_id, {0x8001}, vsomeip::event_type_e::ET_EVENT);

    std::cout << "[mock_radar_producer] Starting L1 TT publishing thread..." << std::endl;
    
    std::thread publish_thread([service_id, instance_id, event_id, interval_ms, tt_overrun_ms]() {
        // Register this thread as TIMPANI L1 TT workload
        timpani_tt_register("radar_workload", "tt_task");

        // --burst-ms (MIT anomaly injection) is the only case where the period is
        // NOT driven by timpani-n; it uses a local timer + sigwait instead.
        const bool burst_mode = (interval_ms != 20);
        sigset_t sig_set;
        sigemptyset(&sig_set);
        sigaddset(&sig_set, TT_LOCAL_TIMER_SIGNO);

        if (burst_mode) {
            timer_t local_timer;
            struct sigevent sev{};
            sev.sigev_notify = SIGEV_SIGNAL;
            sev.sigev_signo = TT_LOCAL_TIMER_SIGNO;
            sev.sigev_value.sival_ptr = &local_timer;
            timer_create(CLOCK_MONOTONIC, &sev, &local_timer);

            struct itimerspec its{};
            its.it_value.tv_sec = interval_ms / 1000;
            its.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
            its.it_interval = its.it_value;
            timer_settime(local_timer, 0, &its, nullptr);
        } else {
            std::cout << "[radar] Using ttsched futex wakeup (/timpani_ttsched SHM)..." << std::endl;
        }
        std::cout << "[radar] per frame: period = wake-to-wake, jitter = period - " << interval_ms
                  << " ms, cpu = CPU time used, resp = wake->published" << std::endl;

        uint32_t seq = 0;
        float distance = 30.0f;
        float direction = -1.0f; // Approaching
        bool in_alert_range = false;
        uint64_t prev_wake_ns = 0;

        while (run_producer) {
            if (burst_mode) {
                int sig;
                sigwait(&sig_set, &sig);
            } else if (ttsched_wait_next_period() != 0) {
                continue; // SHM/slot not ready: skip the period, same as sample_apps -b
            }
            const uint64_t wake_ns = clock_ns(CLOCK_MONOTONIC);
            const uint64_t cpu0_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);

            // Simulated per-period computation that must be scheduled/budgeted
            // by timpani-n (sched_ext), standing in for the workload's runtime.
            run_nsqrt_loops(FCW_CALC_LOOP_COUNT);
            if (tt_overrun_ms > 0) run_nsqrt_for_cpu_ms(tt_overrun_ms);

            // 1. Simulate radar obstacle distance tracking with hysteresis cycle
            distance += direction * 0.5f; // Decreases/increases by 0.5m per frame
            if (distance <= 4.0f) {
                distance = 4.0f;
                direction = 1.0f; // Target starts moving away
                std::cout << "[radar] Target closest. Reversing direction." << std::endl;
            } else if (distance >= 35.0f) {
                distance = 35.0f;
                direction = -1.0f; // Target starts approaching
                std::cout << "[radar] Target furthest. Reversing direction." << std::endl;
            }

            // 2. Radar detects the obstacle alert; only detections are sent to FCW
            RadarTarget target{
                seq++,
                101, // Target ID
                distance,
                direction * 25.0f // Simulated relative velocity (mps)
            };

            const bool detected = (distance < ALERT_RANGE_M);
            if (detected != in_alert_range) {
                in_alert_range = detected;
                std::cout << (detected ? "[radar] Obstacle within alert range -> publishing ObstacleAlert."
                                       : "[radar] Obstacle cleared alert range -> publishing stopped.")
                          << std::endl;
            }
            // 3. Notify subscribers only for detections
            if (detected) {
                auto payload = vsomeip::runtime::get()->create_payload();
                payload->set_data(reinterpret_cast<const uint8_t*>(&target), sizeof(target));
                app->notify(service_id, instance_id, event_id, payload);
            }

            const double cpu_ms = (clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0_ns) / 1e6;
            const double resp_ms = (clock_ns(CLOCK_MONOTONIC) - wake_ns) / 1e6;
            const bool have_period = prev_wake_ns != 0;
            const double period_ms = have_period ? (wake_ns - prev_wake_ns) / 1e6 : 0.0;
            const double jitter_ms = have_period ? period_ms - interval_ms : 0.0;
            prev_wake_ns = wake_ns;

            std::cout << "[radar] frame " << std::setw(5) << target.seq
                      << " | " << std::fixed << std::setprecision(2) << std::setw(5) << target.distance_meters << " m"
                      << " | " << (detected ? "PUBLISH" : "-      ")
                      << " | period " << std::setw(6) << period_ms << " ms"
                      << " | jitter " << std::showpos << std::setw(6) << jitter_ms << std::noshowpos << " ms"
                      << " | cpu " << std::setw(5) << cpu_ms << " ms"
                      << " | resp " << std::setw(6) << resp_ms << " ms"
                      << (std::abs(jitter_ms) > TT_JITTER_LIMIT_MS ? " | JITTER" : "")
                      << (cpu_ms > TT_WCET_MS ? " | OVER_WCET" : "")
                      << (resp_ms > TT_DEADLINE_MS ? " | DEADLINE_MISS" : "")
                      << std::endl;
        }
    });

    app->start(); // Blocks main thread

    if (publish_thread.joinable()) {
        publish_thread.join();
    }

    std::cout << "[mock_radar_producer] Producer terminated." << std::endl;
    return 0;
}
