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
#include <time.h>
#include <unistd.h>

#include "ttsched_shm.h"

#define TTSCHED_WAIT_RECHECK_NS 100000000L

static __thread volatile struct timpani_ttsched_shm* g_shm = NULL;
static __thread char g_name[TTSCHED_NAME_LEN]; /* PR_GET_NAME of this thread */
static __thread int g_has_releases;
static __thread struct timpani_tt_completion g_job;

_Static_assert(TIMPANI_TT_COMPLETE_OP == (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG),
               "TT completion must use a private futex wake");

static void load_task_name(void)
{
    memset(g_name, 0, sizeof(g_name));
    prctl(PR_GET_NAME, g_name, 0, 0, 0);
    g_name[TTSCHED_NAME_LEN - 1] = '\0';
}

static void unmap_shm(void)
{
    if (g_shm) {
        munmap((void*)g_shm, sizeof(struct timpani_ttsched_shm));
        g_shm = NULL;
    }
}

void ttsched_init(void)
{
    int fd;
    void* addr;

    unmap_shm();
    /* The slot itself is looked up on every wait: the schedule may not be
     * published yet. */
    load_task_name();
    fd = shm_open("/timpani_ttsched", O_RDONLY, 0666);
    if (fd < 0) return;

    struct stat status;
    g_has_releases =
        fstat(fd, &status) == 0 &&
        status.st_size >= (off_t)sizeof(struct timpani_ttsched_shm);
    addr = mmap(NULL, sizeof(struct timpani_ttsched_shm), PROT_READ, MAP_SHARED,
                fd, 0);
    close(fd);
    if (addr != MAP_FAILED) g_shm = (volatile struct timpani_ttsched_shm*)addr;
}

static int wait_counter(const volatile uint32_t* word, uint32_t expected,
                        void* ctx)
{
    static const struct timespec recheck = {
        .tv_sec = 0,
        .tv_nsec = TTSCHED_WAIT_RECHECK_NS,
    };

    (void)ctx;
    if (syscall(SYS_futex, (uint32_t*)word, FUTEX_WAIT, expected, &recheck,
                NULL, 0) == 0)
        return 0;
    /* EAGAIN means the counter changed, EINTR is a signal and ETIMEDOUT is
     * the periodic publication/mapping re-check. None is a job by itself. */
    return errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT ? 0 : -1;
}

static int load_release_deadline(const struct ttsched_snapshot* snap,
                                 uint64_t* deadline)
{
    const volatile struct timpani_tt_release* release;
    uint32_t before;
    uint32_t counter;
    uint32_t after;

    if (!g_has_releases || !snap->valid || snap->idx < 0 ||
        snap->idx >= (int)TIMPANI_MAX_TASKS)
        return 0;

    release = &g_shm->releases[snap->idx];
    before = __atomic_load_n(&release->version, __ATOMIC_SEQ_CST);
    counter = __atomic_load_n(&release->counter, __ATOMIC_SEQ_CST);
    *deadline = __atomic_load_n(&release->deadline_ns, __ATOMIC_SEQ_CST);
    after = __atomic_load_n(&release->version, __ATOMIC_SEQ_CST);

    return !(before & 1u) && before == after && counter == snap->counter &&
           ttsched_shm_read_validate(g_shm, snap->generation);
}

int ttsched_wait_next_period(void)
{
    struct ttsched_snapshot snap;
    int ready = 0;

    if (g_job.deadline_ns) {
        syscall(SYS_futex, &g_job, TIMPANI_TT_COMPLETE_OP, 1, NULL, NULL,
                TIMPANI_TT_COMPLETE_BITSET);
        g_job.deadline_ns = 0;
    }

    if (g_shm) {
        ready = ttsched_shm_read(g_shm, g_name, &snap);
        /* magic 0: timpani-n stopped, restarted or is publishing. Map the
         * current object again so that a re-created SHM is picked up. */
        if (!ready && !snap.magic_ok) unmap_shm();
    }
    if (!g_shm) {
        ttsched_init();
        if (g_shm) ready = ttsched_shm_read(g_shm, g_name, &snap);
    }
    if (!ready || snap.idx < 0) {
        /* Not published, being published, removed or cleared. */
        if (ready) load_task_name();
        usleep(1000);
        return -1;
    }

    /* Only an advance of this task's own counter in a stable publication is
     * a job. Publication, control and signal wakes are re-checked. */
    for (;;) {
        enum ttsched_wake wake =
            ttsched_shm_wait(g_shm, g_name, &snap, wait_counter, NULL);
        if (wake == TTSCHED_WAKE_JOB) {
            uint64_t deadline = 0;
            if (!g_has_releases) return 0;
            if (load_release_deadline(&snap, &deadline)) {
                g_job.deadline_ns = deadline;
                return 0;
            }
            /* Counter-only wake-ups from a table publication are not jobs. */
            if (!ttsched_shm_read(g_shm, g_name, &snap) || snap.idx < 0) {
                if (!snap.magic_ok) unmap_shm();
                return -1;
            }
            continue;
        }
        if (wake == TTSCHED_WAKE_FAILED) usleep(1000);
        if (!snap.magic_ok) unmap_shm();
        return -1;
    }
}
