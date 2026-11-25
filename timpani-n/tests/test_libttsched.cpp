// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// DDR-011 Phase 0 P0-T05: /timpani_ttsched even/odd generation publication
// and the libttsched wake rules (only an own counter advance is a job;
// removed or cleared tasks get -1).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../sample-apps/src/ttsched_shm.h"

namespace {

using Shm = volatile timpani_ttsched_shm;

static_assert(TTSCHED_NAME_LEN == 16, "slot name holds 15 bytes and a NUL");
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr),
              "uint32_t SHM fields must be lock-free");
static_assert(offsetof(timpani_ttsched_shm, tasks) % alignof(uint32_t) == 0 &&
                  sizeof(timpani_task_slot) % alignof(uint32_t) == 0,
              "uint32_t SHM fields must stay naturally aligned");

uintptr_t address(const volatile uint32_t* word)
{
    return reinterpret_cast<uintptr_t>(word);
}

std::string name_at(Shm* shm, uint32_t idx)
{
    char out[TTSCHED_NAME_LEN];
    ttsched_shm_load_name(shm, idx, out);
    return out;
}

// Stands in for FUTEX_WAIT: every call runs the next scripted event and
// returns as a wake would. A call past the script fails the wait.
struct ScriptedWait {
    struct Call {
        uintptr_t word;
        uint32_t expected;
    };
    std::vector<std::function<void()>> events;
    std::vector<Call> calls;

    static int wait(const volatile uint32_t* word, uint32_t expected, void* ctx)
    {
        auto* self = static_cast<ScriptedWait*>(ctx);
        self->calls.push_back({address(word), expected});
        if (self->calls.size() > self->events.size()) return -1;
        self->events[self->calls.size() - 1]();
        return 0;
    }
};

class TtschedShmTest : public ::testing::Test
{
  protected:
    void SetUp() override { std::memset(&raw_, 0, sizeof(raw_)); }

    Shm* shm() { return &raw_; }

    // Publishes @names at their index; "" clears a slot.
    uint32_t publish(const std::vector<std::string>& names)
    {
        const uint32_t publishing = ttsched_shm_begin_publish(shm());
        for (size_t i = 0; i < names.size(); ++i) {
            ttsched_shm_store_name(shm(), static_cast<uint32_t>(i),
                                   names[i].c_str());
        }
        ttsched_shm_store_n_tasks(shm(), static_cast<uint32_t>(names.size()));
        return ttsched_shm_end_publish(shm(), publishing);
    }

    void set_n_tasks(uint32_t n)
    {
        const uint32_t publishing = ttsched_shm_begin_publish(shm());
        ttsched_shm_store_n_tasks(shm(), n);
        ttsched_shm_end_publish(shm(), publishing);
    }

    void fire(uint32_t idx)
    {
        __atomic_fetch_add(&raw_.tasks[idx].counter, 1u, __ATOMIC_SEQ_CST);
    }

    uint32_t counter(uint32_t idx)
    {
        return ttsched_shm_load_counter(shm(), idx);
    }

    uintptr_t counter_word(uint32_t idx)
    {
        return address(&raw_.tasks[idx].counter);
    }

    ttsched_snapshot read(const char* name)
    {
        ttsched_snapshot snap;
        ttsched_shm_read(shm(), name, &snap);
        return snap;
    }

    ttsched_wake wait(ScriptedWait& script, ttsched_snapshot& snap)
    {
        return ttsched_shm_wait(shm(), "self", &snap, &ScriptedWait::wait,
                                &script);
    }

    timpani_ttsched_shm raw_;
};

ttsched_snapshot stable(uint32_t generation, int idx, uint32_t counter)
{
    ttsched_snapshot snap{};
    snap.valid = 1;
    snap.magic_ok = 1;
    snap.generation = generation;
    snap.idx = idx;
    snap.counter = counter;
    return snap;
}

// ---------------------------------------------------------------------------
// Writer protocol
// ---------------------------------------------------------------------------

TEST_F(TtschedShmTest, ResetGivesNotReadyEvenGenerationDistinctFromStale)
{
    EXPECT_EQ(ttsched_shm_reset(shm()), 2u);
    EXPECT_EQ(raw_.magic, 0u);
    EXPECT_EQ(raw_.n_tasks, 0u);
    EXPECT_EQ(raw_.generation, 2u);

    const std::pair<uint32_t, uint32_t> cases[] = {
        {1, 2}, {5, 6}, {6, 8}, {0xFFFFFFFEu, 0}, {0xFFFFFFFFu, 0}};
    for (const auto& item : cases) {
        raw_.generation = item.first;
        raw_.magic = TIMPANI_TTSCHED_MAGIC;
        raw_.n_tasks = 3;
        EXPECT_EQ(ttsched_shm_reset(shm()), item.second) << item.first;
        EXPECT_EQ(raw_.magic, 0u);
        EXPECT_EQ(raw_.n_tasks, 0u);
    }
}

TEST_F(TtschedShmTest, PublicationIsOddAndInvalidUntilItEnds)
{
    ttsched_shm_reset(shm());
    const uint32_t publishing = ttsched_shm_begin_publish(shm());
    EXPECT_EQ(publishing, 3u);
    EXPECT_EQ(raw_.generation, 3u);
    EXPECT_EQ(raw_.magic, 0u);
    ttsched_shm_store_name(shm(), 0, "self");
    ttsched_shm_store_n_tasks(shm(), 1);

    ttsched_snapshot snap = read("self");
    EXPECT_FALSE(snap.valid);
    EXPECT_FALSE(snap.magic_ok);
    EXPECT_EQ(snap.idx, -1);

    EXPECT_EQ(ttsched_shm_end_publish(shm(), publishing), 4u);
    EXPECT_EQ(raw_.magic, TIMPANI_TTSCHED_MAGIC);
    snap = read("self");
    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.generation, 4u);
    EXPECT_EQ(snap.idx, 0);
}

TEST_F(TtschedShmTest, EachPublicationAdvancesByTwoAndRecoversFromOdd)
{
    ttsched_shm_reset(shm());
    EXPECT_EQ(publish({"self"}), 4u);
    EXPECT_EQ(publish({"self"}), 6u);
    // An interrupted publication leaves an odd value; the next one still
    // starts at a different odd value and ends even.
    raw_.generation = 7;
    const uint32_t publishing = ttsched_shm_begin_publish(shm());
    EXPECT_EQ(publishing, 9u);
    EXPECT_EQ(ttsched_shm_end_publish(shm(), publishing), 10u);
}

TEST_F(TtschedShmTest, PublicationLeavesCountersUntouched)
{
    raw_.tasks[0].counter = 41;
    raw_.tasks[1].counter = 7;
    raw_.tasks[5].counter = 3;
    publish({"a", "b"});
    publish({"a", "", "c"});
    EXPECT_EQ(counter(0), 41u);
    EXPECT_EQ(counter(1), 7u);
    EXPECT_EQ(counter(2), 0u);
    EXPECT_EQ(counter(5), 3u);
}

TEST_F(TtschedShmTest, ReleaseRecordIsStableBeforeTheCounterAdvances)
{
    raw_.tasks[2].counter = 40;
    EXPECT_EQ(ttsched_shm_publish_release(shm(), 2, 123456789u), 41u);
    EXPECT_EQ(raw_.releases[2].version, 2u);
    EXPECT_EQ(raw_.releases[2].counter, 41u);
    EXPECT_EQ(raw_.releases[2].deadline_ns, 123456789u);
    EXPECT_EQ(counter(2), 41u);

    EXPECT_EQ(ttsched_shm_publish_release(shm(), 2, 987654321u), 42u);
    EXPECT_EQ(raw_.releases[2].version, 4u);
    EXPECT_EQ(raw_.releases[2].counter, 42u);
    EXPECT_EQ(raw_.releases[2].deadline_ns, 987654321u);
    EXPECT_EQ(counter(2), 42u);
}

TEST_F(TtschedShmTest, StoreNameTruncatesAndClearsTheRest)
{
    ttsched_shm_store_name(shm(), 3, "abcdefghijklmnopqrstuvwxyz");
    EXPECT_EQ(name_at(shm(), 3), "abcdefghijklmno");
    EXPECT_EQ(raw_.tasks[3].name[15], '\0');

    ttsched_shm_store_name(shm(), 3, "xy");
    EXPECT_EQ(name_at(shm(), 3), "xy");
    for (size_t i = 2; i < TTSCHED_NAME_LEN; ++i) {
        EXPECT_EQ(raw_.tasks[3].name[i], '\0') << i;
    }

    ttsched_shm_store_name(shm(), 4, "zz");
    ttsched_shm_store_name(shm(), 3, "");
    ttsched_shm_store_name(shm(), 4, nullptr);
    for (size_t i = 0; i < TTSCHED_NAME_LEN; ++i) {
        EXPECT_EQ(raw_.tasks[3].name[i], '\0') << i;
        EXPECT_EQ(raw_.tasks[4].name[i], '\0') << i;
    }
}

// ---------------------------------------------------------------------------
// Reader snapshot
// ---------------------------------------------------------------------------

TEST_F(TtschedShmTest, SnapshotNeedsMagicAndAnEvenGeneration)
{
    EXPECT_FALSE(read("self").valid);  // never published

    publish({"self"});
    EXPECT_TRUE(read("self").valid);

    raw_.generation = 3;  // magic still valid, but odd: being published
    ttsched_snapshot snap = read("self");
    EXPECT_FALSE(snap.valid);
    EXPECT_TRUE(snap.magic_ok);
    EXPECT_EQ(snap.idx, -1);

    raw_.generation = 4;
    raw_.magic = 0;  // not ready or timpani-n stopped
    snap = read("self");
    EXPECT_FALSE(snap.valid);
    EXPECT_FALSE(snap.magic_ok);
}

TEST_F(TtschedShmTest, ReadValidationRejectsChangesDuringTheCopy)
{
    publish({"self"});
    uint32_t gen = 0;
    int magic_ok = 0;
    ASSERT_TRUE(ttsched_shm_read_begin(shm(), &gen, &magic_ok));
    EXPECT_TRUE(magic_ok);
    EXPECT_TRUE(ttsched_shm_read_validate(shm(), gen));

    publish({"other"});  // a whole publication in between
    EXPECT_FALSE(ttsched_shm_read_validate(shm(), gen));

    ASSERT_TRUE(ttsched_shm_read_begin(shm(), &gen, nullptr));
    const uint32_t publishing = ttsched_shm_begin_publish(shm());
    EXPECT_FALSE(ttsched_shm_read_validate(shm(), gen));
    ttsched_shm_end_publish(shm(), publishing);

    ASSERT_TRUE(ttsched_shm_read_begin(shm(), &gen, nullptr));
    __atomic_store_n(&raw_.magic, 0u, __ATOMIC_SEQ_CST);  // shutdown
    EXPECT_FALSE(ttsched_shm_read_validate(shm(), gen));
}

TEST_F(TtschedShmTest, SnapshotFindsOwnSlotOnlyWithinNTasks)
{
    raw_.tasks[1].counter = 9;
    publish({"a", "self", "b"});
    ttsched_snapshot snap = read("self");
    ASSERT_TRUE(snap.valid);
    EXPECT_EQ(snap.idx, 1);
    EXPECT_EQ(snap.counter, 9u);

    publish({"a", "b", "c", "self"});
    set_n_tasks(3);  // the name left at index 3 is not published
    snap = read("self");
    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.idx, -1);

    set_n_tasks(1000);  // a corrupt count is clamped to the slot array
    EXPECT_EQ(ttsched_shm_load_n_tasks(shm()),
              static_cast<uint32_t>(TIMPANI_MAX_TASKS));
    snap = read("self");
    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.idx, 3);
    snap = read("missing");  // scans every slot, but no further
    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.idx, -1);
}

TEST_F(TtschedShmTest, EmptyOwnNameNeverMatchesAClearedSlot)
{
    publish({"", "a"});
    EXPECT_TRUE(read("").valid);
    EXPECT_EQ(read("").idx, -1);
    EXPECT_EQ(read(nullptr).idx, -1);
}

TEST_F(TtschedShmTest, NameComparisonUsesTheFifteenStoredBytes)
{
    publish({"abcdefghijklmnopq"});
    EXPECT_EQ(read("abcdefghijklmno").idx, 0);  // PR_GET_NAME form
    EXPECT_EQ(read("abcdefghijklmn").idx, -1);
}

// ---------------------------------------------------------------------------
// Wake classification
// ---------------------------------------------------------------------------

TEST(TtschedClassify, OnlyAnOwnForwardCounterStepIsAJob)
{
    const ttsched_snapshot before = stable(4, 1, 10);
    ttsched_snapshot invalid{};
    invalid.idx = -1;
    const struct {
        const char* what;
        ttsched_snapshot after;
        ttsched_wake expected;
    } cases[] = {
        {"slot fired", stable(4, 1, 11), TTSCHED_WAKE_JOB},
        {"several fires", stable(4, 1, 13), TTSCHED_WAKE_JOB},
        {"control wake or signal", stable(4, 1, 10), TTSCHED_WAKE_REWAIT},
        {"retained fire across publication", stable(6, 1, 11),
         TTSCHED_WAKE_JOB},
        {"publication only", stable(6, 1, 10), TTSCHED_WAKE_REWAIT},
        {"slot moved or reused", stable(6, 2, 11), TTSCHED_WAKE_REWAIT},
        {"counter reset", stable(6, 1, 0), TTSCHED_WAKE_REWAIT},
        {"being published", invalid, TTSCHED_WAKE_NOT_READY},
        {"removed or cleared", stable(6, -1, 0), TTSCHED_WAKE_REMOVED},
    };
    for (const auto& item : cases) {
        EXPECT_EQ(ttsched_classify_wake(&before, &item.after), item.expected)
            << item.what;
    }

    const ttsched_snapshot wrap_before = stable(4, 1, 0xFFFFFFFFu);
    const ttsched_snapshot wrap_after = stable(4, 1, 0);
    EXPECT_EQ(ttsched_classify_wake(&wrap_before, &wrap_after),
              TTSCHED_WAKE_JOB);
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

TEST_F(TtschedShmTest, ControlWakesAreNotReportedAsJobs)
{
    publish({"self"});
    ttsched_snapshot snap = read("self");
    ASSERT_TRUE(snap.valid);

    ScriptedWait script;
    script.events = {
        [] {},                                 // control wake or signal
        [this] { publish({"self", "new"}); },  // re-publication only
        [this] { fire(0); },                   // the real slot fire
    };
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_JOB);
    ASSERT_EQ(script.calls.size(), 3u);
    for (const auto& call : script.calls) {
        EXPECT_EQ(call.word, counter_word(0));
        EXPECT_EQ(call.expected, 0u);
    }
    EXPECT_EQ(snap.counter, 1u);
}

TEST_F(TtschedShmTest, WakeDuringPublicationIsNotAJob)
{
    publish({"self"});
    ttsched_snapshot snap = read("self");

    // The legacy publication path increments every counter while magic=0.
    uint32_t publishing = 0;
    ScriptedWait script;
    script.events = {[this, &publishing] {
        publishing = ttsched_shm_begin_publish(shm());
        fire(0);
    }};
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_NOT_READY);
    EXPECT_FALSE(snap.valid);
    EXPECT_FALSE(snap.magic_ok);

    // The next call starts from the finished publication and waits for a
    // real fire.
    ttsched_shm_store_n_tasks(shm(), 1);
    ttsched_shm_end_publish(shm(), publishing);
    snap = read("self");
    ASSERT_TRUE(snap.valid);
    EXPECT_EQ(snap.counter, 1u);
    ScriptedWait next;
    next.events = {[] {}, [this] { fire(0); }};
    EXPECT_EQ(wait(next, snap), TTSCHED_WAKE_JOB);
    EXPECT_EQ(next.calls.size(), 2u);
}

TEST_F(TtschedShmTest, RemovedOrClearedTaskGetsRemoved)
{
    const std::vector<std::vector<std::string>> updates = {
        {"other", ""},  // REMOVE: name cleared
        {"other"},      // REMOVE: name beyond n_tasks
        {},             // CLEAR
    };
    for (const auto& update : updates) {
        SCOPED_TRACE(update.size());
        publish({"other", "self"});
        ttsched_snapshot snap = read("self");
        ASSERT_EQ(snap.idx, 1);
        ScriptedWait script;
        script.events = {[this, &update] { publish(update); }};
        EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_REMOVED);
        EXPECT_TRUE(snap.valid);
        EXPECT_EQ(snap.idx, -1);
        EXPECT_EQ(read("self").idx, -1);
    }
}

TEST_F(TtschedShmTest, ReusedSlotWakeIsNotConsumed)
{
    raw_.tasks[2].counter = 5;
    publish({"a", "self"});
    ttsched_snapshot snap = read("self");
    ASSERT_EQ(snap.idx, 1);

    ScriptedWait script;
    script.events = {
        [this] {
            publish({"a", "other", "self"});
            fire(1);  // another task now owns the old slot
        },
        [this] { fire(2); },
    };
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_JOB);
    ASSERT_EQ(script.calls.size(), 2u);
    EXPECT_EQ(script.calls[0].word, counter_word(1));
    EXPECT_EQ(script.calls[0].expected, 0u);
    EXPECT_EQ(script.calls[1].word, counter_word(2));
    EXPECT_EQ(script.calls[1].expected, 5u);
    EXPECT_EQ(snap.idx, 2);
    EXPECT_EQ(snap.counter, 6u);
}

TEST_F(TtschedShmTest, RetainedCounterSurvivesRepublication)
{
    raw_.tasks[1].counter = 41;
    publish({"a", "self"});
    ttsched_snapshot snap = read("self");
    ASSERT_EQ(snap.counter, 41u);

    ScriptedWait script;
    script.events = {[this] {
        fire(1);  // an old-table fire after the snapshot was prepared
        const uint32_t publishing = ttsched_shm_begin_publish(shm());
        ttsched_shm_store_name(shm(), 2, "new");
        ttsched_shm_store_counter(shm(), 2, 0);
        ttsched_shm_store_n_tasks(shm(), 3);
        ttsched_shm_end_publish(shm(), publishing);
    }};
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_JOB);
    EXPECT_EQ(counter(1), 42u);
    EXPECT_EQ(snap.counter, 42u);
    EXPECT_EQ(snap.idx, 1);
}

TEST_F(TtschedShmTest, FireJustBeforeSleepingIsAJob)
{
    publish({"self"});
    ttsched_snapshot snap = read("self");
    fire(0);  // FUTEX_WAIT would return EAGAIN at once

    ScriptedWait script;
    script.events = {[] {}};
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_JOB);
    ASSERT_EQ(script.calls.size(), 1u);
    EXPECT_EQ(script.calls[0].expected, 0u);
}

TEST_F(TtschedShmTest, WaitFailureIsReported)
{
    publish({"self"});
    ttsched_snapshot snap = read("self");
    ScriptedWait script;  // no events: the first wait fails
    EXPECT_EQ(wait(script, snap), TTSCHED_WAKE_FAILED);
    EXPECT_EQ(script.calls.size(), 1u);
}

// ---------------------------------------------------------------------------
// Concurrent publication
// ---------------------------------------------------------------------------

// Valid snapshots must never mix two publications, including on weakly
// ordered CPUs such as arm64.
TEST(TtschedShmConcurrency, ReadersNeverObserveMixedPublications)
{
    constexpr uint32_t kSlots = 8;
    const std::vector<std::string> images[2] = {
        {"a0", "a1", "self"},
        {"b0", "b1", "b2", "b3", "b4", "b5", "b6", "self"},
    };
    timpani_ttsched_shm raw;
    std::memset(&raw, 0, sizeof(raw));
    Shm* shm = &raw;
    ttsched_shm_reset(shm);

    auto publish_image = [shm](const std::vector<std::string>& image) {
        const uint32_t publishing = ttsched_shm_begin_publish(shm);
        for (uint32_t i = 0; i < kSlots; ++i) {
            ttsched_shm_store_name(shm, i,
                                   i < image.size() ? image[i].c_str() : "");
        }
        ttsched_shm_store_n_tasks(shm, static_cast<uint32_t>(image.size()));
        ttsched_shm_end_publish(shm, publishing);
    };
    publish_image(images[0]);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (uint32_t round = 1; !stop.load(std::memory_order_relaxed);
             ++round) {
            publish_image(images[round % 2]);
            for (int spin = 0; spin < 20; ++spin) std::this_thread::yield();
        }
    });

    struct Result {
        uint64_t seen[2] = {0, 0};
        uint64_t mixed = 0;
        uint64_t bad_lookup = 0;
    };
    auto matches = [&](const std::vector<std::string>& image, uint32_t n,
                       char (*names)[TTSCHED_NAME_LEN]) {
        if (n != image.size()) return false;
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (names[i] != (i < image.size() ? image[i] : "")) return false;
        }
        return true;
    };
    auto reader = [&](Result& result) {
        char names[kSlots][TTSCHED_NAME_LEN];
        while (!stop.load(std::memory_order_relaxed)) {
            uint32_t gen = 0;
            if (ttsched_shm_read_begin(shm, &gen, nullptr)) {
                const uint32_t n = ttsched_shm_load_n_tasks(shm);
                for (uint32_t i = 0; i < kSlots; ++i) {
                    ttsched_shm_load_name(shm, i, names[i]);
                }
                if (ttsched_shm_read_validate(shm, gen)) {
                    if (matches(images[0], n, names)) {
                        ++result.seen[0];
                    } else if (matches(images[1], n, names)) {
                        ++result.seen[1];
                    } else {
                        ++result.mixed;
                    }
                }
            }
            ttsched_snapshot snap;
            if (ttsched_shm_read(shm, "self", &snap) && snap.idx != 2 &&
                snap.idx != 7) {
                ++result.bad_lookup;
            }
        }
    };

    Result results[2];
    std::thread first(reader, std::ref(results[0]));
    std::thread second(reader, std::ref(results[1]));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    writer.join();
    first.join();
    second.join();

    EXPECT_EQ(results[0].mixed + results[1].mixed, 0u);
    EXPECT_EQ(results[0].bad_lookup + results[1].bad_lookup, 0u);
    EXPECT_GT(results[0].seen[0] + results[1].seen[0], 0u);
    EXPECT_GT(results[0].seen[1] + results[1].seen[1], 0u);
}

}  // namespace
