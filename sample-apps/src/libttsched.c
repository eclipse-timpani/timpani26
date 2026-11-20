// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "libttsched.h"
#include "tt_completion.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static __thread volatile struct timpani_ttsched_shm* g_shm = NULL;
static __thread int g_task_idx = -1; /* index into g_shm->tasks[], -1 = not found yet */
static __thread uint32_t g_shm_generation = 0;
static __thread int g_has_releases;
static __thread struct timpani_tt_completion g_job;

_Static_assert(TIMPANI_TT_COMPLETE_OP == (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG),
               "TT completion must use a private futex wake");

static int schedule_is_current(void)
{
    return __atomic_load_n(&g_shm->magic, __ATOMIC_ACQUIRE) == TIMPANI_TTSCHED_MAGIC &&
           __atomic_load_n(&g_shm->generation, __ATOMIC_ACQUIRE) == g_shm_generation;
}

static int find_task_slot(volatile struct timpani_ttsched_shm* shm,
                          const char* name)
{
    uint32_t n, i;

    if (shm->magic != TIMPANI_TTSCHED_MAGIC) return -1;

    n = shm->n_tasks;
    for (i = 0; i < n && i < TIMPANI_MAX_TASKS; i++) {
        if (strncmp((const char*)shm->tasks[i].name, name, 15) == 0)
            return (int)i;
    }
    return -1;
}

void ttsched_init(void)
{
    int fd = shm_open("/timpani_ttsched", O_RDONLY, 0666);
    if (fd < 0) return;

    struct stat status;
    g_has_releases = fstat(fd, &status) == 0 && status.st_size >= (off_t)sizeof(*g_shm);
    g_shm = (volatile struct timpani_ttsched_shm*)mmap(
        NULL, sizeof(struct timpani_ttsched_shm), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        return;
    }

    /* Slot lookup: schedule may not be ready yet — retry in wait_next_period */
    char my_name[16] = {};
    prctl(PR_GET_NAME, my_name, 0, 0, 0);
    g_task_idx = find_task_slot(g_shm, my_name);
    g_shm_generation = g_shm->generation;
}

int ttsched_wait_next_period(void)
{
    uint32_t current;

    if (g_job.deadline_ns) {
        syscall(SYS_futex, &g_job, TIMPANI_TT_COMPLETE_OP, 1, NULL, NULL,
                TIMPANI_TT_COMPLETE_BITSET);
        g_job.deadline_ns = 0;
    }

    if (!g_shm) {
        /* SHM not mapped yet — retry open/map on every call */
        ttsched_init();
        if (!g_shm) {
            usleep(1000);
            return -1;
        }
    }

    /* Detect timpani-n restart: magic is zeroed by TimerMaster before it
     * publishes a new schedule table (or remains 0 after shm_unlink +
     * re-create).  Re-init to pick up the new SHM object. */
    if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) {
        /* Unmap stale SHM and re-open the (possibly new) object */
        munmap((void*)g_shm, sizeof(struct timpani_ttsched_shm));
        g_shm = NULL;
        g_task_idx = -1;
        ttsched_init();
        if (!g_shm) {
            usleep(1000);
            return -1;
        }
        /* If still not ready (magic still 0), wait for schedule publish */
        if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) {
            usleep(1000);
            return -1;
        }
    }

    /* Detect schedule table update via generation counter */
    if (g_shm->generation != g_shm_generation) {
        g_shm_generation = g_shm->generation;
        g_task_idx = -1;
    }

    /* Retry slot lookup if schedule arrived after ttsched_init() */
    if (g_task_idx < 0) {
        char my_name[16] = {};
        prctl(PR_GET_NAME, my_name, 0, 0, 0);
        g_task_idx = find_task_slot(g_shm, my_name);
        if (g_task_idx < 0) {
            usleep(1000);
            return -1;
        }
    }

    volatile uint32_t* counter = &g_shm->tasks[g_task_idx].counter;
    current = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!schedule_is_current()) return -1;
        uint32_t observed = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
        if (!schedule_is_current()) return -1;
        if (observed != current) {
            if (!g_has_releases) return 0;

            const volatile struct timpani_tt_release* release = &g_shm->releases[g_task_idx];
            uint32_t version = __atomic_load_n(&release->version, __ATOMIC_SEQ_CST);
            uint32_t job_counter = __atomic_load_n(&release->counter, __ATOMIC_SEQ_CST);
            uint64_t deadline = __atomic_load_n(&release->deadline_ns, __ATOMIC_SEQ_CST);
            if (!schedule_is_current()) return -1;
            if (!(version & 1u) && job_counter == observed &&
                version == __atomic_load_n(&release->version, __ATOMIC_SEQ_CST)) {
                g_job.deadline_ns = deadline;
                return 0;
            }
        }
        if (syscall(SYS_futex, (uint32_t*)counter, FUTEX_WAIT, observed,
                    NULL, NULL, 0) == -1 && errno != EINTR && errno != EAGAIN)
            return -1;
    }
}
