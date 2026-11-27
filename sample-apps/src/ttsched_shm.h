// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Internal /timpani_ttsched publication protocol shared by the timpani-n
 * writer and libttsched (DDR-011 D-P0-05, LLD-011 L-P0-05). Not installed:
 * the layout and API in libttsched.h are unchanged.
 *
 * generation is even while a publication is stable and odd while one is
 * being written. A reader uses fields only when magic is valid and the same
 * even generation is observed before and after copying them. All shared
 * fields are accessed with same-width __atomic operations.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libttsched.h"

#define TTSCHED_NAME_LEN (sizeof(((struct timpani_task_slot*)0)->name))

/* ---- Writer ---- */

/* Marks the object not ready with an even generation above any stale one. */
static inline uint32_t ttsched_shm_reset(
    volatile struct timpani_ttsched_shm* shm)
{
    uint32_t gen =
        (__atomic_load_n(&shm->generation, __ATOMIC_RELAXED) | 1u) + 1u;

    __atomic_store_n(&shm->magic, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->n_tasks, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->generation, gen, __ATOMIC_RELEASE);
    return gen;
}

/* Starts a publication. Returns the odd generation that marks it. */
static inline uint32_t ttsched_shm_begin_publish(
    volatile struct timpani_ttsched_shm* shm)
{
    uint32_t gen = __atomic_load_n(&shm->generation, __ATOMIC_RELAXED);

    gen += (gen & 1u) ? 2u : 1u;
    __atomic_store_n(&shm->magic, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->generation, gen, __ATOMIC_RELAXED);
    /* A reader that observes any later field store also observes @gen. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return gen;
}

/* Copies at most 15 bytes of @name and clears the rest of the slot name. */
static inline void ttsched_shm_store_name(
    volatile struct timpani_ttsched_shm* shm, uint32_t idx, const char* name)
{
    size_t i = 0;

    for (; name && i + 1 < TTSCHED_NAME_LEN && name[i] != '\0'; ++i)
        __atomic_store_n(&shm->tasks[idx].name[i], name[i], __ATOMIC_RELAXED);
    for (; i < TTSCHED_NAME_LEN; ++i)
        __atomic_store_n(&shm->tasks[idx].name[i], '\0', __ATOMIC_RELAXED);
}

static inline void ttsched_shm_store_counter(
    volatile struct timpani_ttsched_shm* shm, uint32_t idx, uint32_t value)
{
    __atomic_store_n(&shm->tasks[idx].counter, value, __ATOMIC_RELAXED);
}

static inline uint32_t ttsched_shm_publish_release(
    volatile struct timpani_ttsched_shm* shm, uint32_t idx,
    uint64_t deadline_ns)
{
    volatile struct timpani_tt_release* release = &shm->releases[idx];
    uint32_t* counter = (uint32_t*)&shm->tasks[idx].counter;
    const uint32_t next_counter =
        __atomic_load_n(counter, __ATOMIC_RELAXED) + 1u;
    const uint32_t base_version =
        __atomic_load_n(&release->version, __ATOMIC_RELAXED) & ~1u;

    __atomic_store_n(&release->version, base_version + 1u,
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&release->counter, next_counter, __ATOMIC_SEQ_CST);
    __atomic_store_n(&release->deadline_ns, deadline_ns, __ATOMIC_SEQ_CST);
    __atomic_store_n(&release->version, base_version + 2u,
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(counter, next_counter, __ATOMIC_RELEASE);
    return next_counter;
}

static inline void ttsched_shm_store_n_tasks(
    volatile struct timpani_ttsched_shm* shm, uint32_t n_tasks)
{
    __atomic_store_n(&shm->n_tasks, n_tasks, __ATOMIC_RELAXED);
}

/* Ends the publication marked by @publishing. Returns the even generation. */
static inline uint32_t ttsched_shm_end_publish(
    volatile struct timpani_ttsched_shm* shm, uint32_t publishing)
{
    uint32_t gen = publishing + 1u;

    __atomic_store_n(&shm->generation, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->magic, TIMPANI_TTSCHED_MAGIC, __ATOMIC_RELEASE);
    return gen;
}

/* ---- Reader ---- */

/* Starts a read. Returns 1 only for a valid magic and an even @gen. */
static inline int ttsched_shm_read_begin(
    const volatile struct timpani_ttsched_shm* shm, uint32_t* gen,
    int* magic_ok)
{
    uint32_t magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);

    *gen = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    if (magic_ok) *magic_ok = magic == TIMPANI_TTSCHED_MAGIC;
    return magic == TIMPANI_TTSCHED_MAGIC && (*gen & 1u) == 0;
}

/* Returns 1 if fields copied since ttsched_shm_read_begin() are consistent. */
static inline int ttsched_shm_read_validate(
    const volatile struct timpani_ttsched_shm* shm, uint32_t gen)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&shm->generation, __ATOMIC_RELAXED) == gen &&
           __atomic_load_n(&shm->magic, __ATOMIC_RELAXED) ==
               TIMPANI_TTSCHED_MAGIC;
}

static inline uint32_t ttsched_shm_load_n_tasks(
    const volatile struct timpani_ttsched_shm* shm)
{
    uint32_t n = __atomic_load_n(&shm->n_tasks, __ATOMIC_RELAXED);

    return n < TIMPANI_MAX_TASKS ? n : TIMPANI_MAX_TASKS;
}

/* Copies a slot name byte by byte into @out and NUL-terminates it. */
static inline void ttsched_shm_load_name(
    const volatile struct timpani_ttsched_shm* shm, uint32_t idx, char* out)
{
    size_t i;

    for (i = 0; i < TTSCHED_NAME_LEN; ++i)
        out[i] = __atomic_load_n(&shm->tasks[idx].name[i], __ATOMIC_RELAXED);
    out[TTSCHED_NAME_LEN - 1] = '\0';
}

static inline uint32_t ttsched_shm_load_counter(
    const volatile struct timpani_ttsched_shm* shm, uint32_t idx)
{
    return __atomic_load_n(&shm->tasks[idx].counter, __ATOMIC_RELAXED);
}

struct ttsched_snapshot {
    int valid;           /* a stable publication was observed */
    int magic_ok;        /* magic was valid when the read started */
    uint32_t generation; /* even generation of a valid snapshot */
    int idx;             /* own slot, or -1 when not published */
    uint32_t counter;    /* own slot counter when idx >= 0 */
};

/* Reads the slot published for @name. Returns snap->valid. */
static inline int ttsched_shm_read(
    const volatile struct timpani_ttsched_shm* shm, const char* name,
    struct ttsched_snapshot* snap)
{
    char slot_name[TTSCHED_NAME_LEN];
    uint32_t gen, n, i;
    uint32_t counter = 0;
    int idx = -1;

    snap->valid = 0;
    snap->generation = 0;
    snap->idx = -1;
    snap->counter = 0;
    if (!ttsched_shm_read_begin(shm, &gen, &snap->magic_ok)) return 0;

    n = ttsched_shm_load_n_tasks(shm);
    for (i = 0; i < n && idx < 0 && name && name[0] != '\0'; ++i) {
        ttsched_shm_load_name(shm, i, slot_name);
        if (strncmp(slot_name, name, TTSCHED_NAME_LEN - 1) == 0) {
            idx = (int)i;
            counter = ttsched_shm_load_counter(shm, i);
        }
    }
    if (!ttsched_shm_read_validate(shm, gen)) return 0;

    snap->valid = 1;
    snap->generation = gen;
    snap->idx = idx;
    snap->counter = counter;
    return 1;
}

enum ttsched_wake {
    TTSCHED_WAKE_JOB,       /* own counter advanced: run one job */
    TTSCHED_WAKE_REWAIT,    /* control wake, signal or moved slot: wait again */
    TTSCHED_WAKE_NOT_READY, /* no stable publication: return -1 */
    TTSCHED_WAKE_REMOVED,   /* own task not published (REMOVE/CLEAR): -1 */
    TTSCHED_WAKE_FAILED,    /* waiting itself failed: -1 */
};

/* Decides what a wake observed as @after means for the wait in @before. */
static inline enum ttsched_wake ttsched_classify_wake(
    const struct ttsched_snapshot* before, const struct ttsched_snapshot* after)
{
    uint32_t step;

    if (!after->valid) return TTSCHED_WAKE_NOT_READY;
    if (after->idx < 0) return TTSCHED_WAKE_REMOVED;
    /* Another slot's counter belongs to another task: never consume it. */
    if (after->idx != before->idx) return TTSCHED_WAKE_REWAIT;
    /* Only a forward step is a slot fire; a counter reset is not a job. */
    step = after->counter - before->counter;
    return step != 0 && step < 0x80000000u ? TTSCHED_WAKE_JOB
                                           : TTSCHED_WAKE_REWAIT;
}

/*
 * Blocks while *word == expected until a wake or a signal. Returns 0 when
 * the caller should re-check the publication, or -1 if waiting failed.
 */
typedef int (*ttsched_wait_fn)(const volatile uint32_t* word, uint32_t expected,
                               void* ctx);

/*
 * Waits until the own slot in @snap fires. @snap must be valid with
 * idx >= 0 and is updated to the last snapshot taken. Never returns
 * TTSCHED_WAKE_REWAIT.
 */
static inline enum ttsched_wake ttsched_shm_wait(
    const volatile struct timpani_ttsched_shm* shm, const char* name,
    struct ttsched_snapshot* snap, ttsched_wait_fn wait, void* ctx)
{
    struct ttsched_snapshot after;
    enum ttsched_wake wake;

    for (;;) {
        if (wait(&shm->tasks[snap->idx].counter, snap->counter, ctx) != 0)
            return TTSCHED_WAKE_FAILED;
        ttsched_shm_read(shm, name, &after);
        wake = ttsched_classify_wake(snap, &after);
        *snap = after;
        if (wake != TTSCHED_WAKE_REWAIT) return wake;
    }
}
