/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <sys/prctl.h>
#include <libttsched.h>

static std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    g_running = false;
}

void run_dummy_task(const char* task_name, int period_ms) {
    prctl(PR_SET_NAME, task_name, 0, 0, 0);
    std::cout << "[dummy_tasks] Started thread for task: " << task_name << std::endl;
    ttsched_init();

    while (g_running) {
        int ret = ttsched_wait_next_period();
        if (ret != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "[dummy_tasks] Launching threads task_A..task_F..." << std::endl;

    std::thread t1(run_dummy_task, "task_A", 10);
    std::thread t2(run_dummy_task, "task_B", 20);
    std::thread t3(run_dummy_task, "task_C", 20);
    std::thread t4(run_dummy_task, "task_D", 20);
    std::thread t5(run_dummy_task, "task_E", 20);
    std::thread t6(run_dummy_task, "task_F", 20);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();

    return 0;
}
