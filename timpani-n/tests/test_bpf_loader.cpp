// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// DDR-011 Phase 0 P0-T03: dual-bank BPF maps, KernelControl handover
// state machine, and shadow-bank staging with failure discard.
//
// These tests exercise the host-runnable free functions in bpf_loader.h
// (clamp_bank, apply_handover, stage_bank) and the KernelControl layout.
// They need neither the BPF skeleton nor libbpf, so they run on any host.
// BPF load/run behavior is covered by the Linux-only integration path.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "bpf_loader.h"

using timpani::node::apply_handover;
using timpani::node::BankWriter;
using timpani::node::clamp_bank;
using timpani::node::stage_bank;

namespace {

// ---------------------------------------------------------------------------
// clamp_bank
// ---------------------------------------------------------------------------

TEST(BpfLoaderBankTest, ClampBankValidValues)
{
    EXPECT_EQ(clamp_bank(0), 0);
    EXPECT_EQ(clamp_bank(1), 1);
}

TEST(BpfLoaderBankTest, ClampBankOutOfRangeFallsBackToZero)
{
    EXPECT_EQ(clamp_bank(-1), 0);
    EXPECT_EQ(clamp_bank(2), 0);
    EXPECT_EQ(clamp_bank(100), 0);
}

// ---------------------------------------------------------------------------
// apply_handover
// ---------------------------------------------------------------------------

TEST(BpfLoaderHandoverTest, SwitchesActiveBankAndClosesGate)
{
    KernelControl kc = {};
    kc.active_bank_idx = 0;
    kc.tt_handover_gate = 0;

    apply_handover(&kc, 1);

    EXPECT_EQ(kc.active_bank_idx, 1u);
    EXPECT_EQ(kc.tt_handover_gate, 0u);  // gate is always released
}

TEST(BpfLoaderHandoverTest, ClampsOutOfRangeShadowBank)
{
    KernelControl kc = {};
    apply_handover(&kc, 7);
    EXPECT_EQ(kc.active_bank_idx, 0u);
    EXPECT_EQ(kc.tt_handover_gate, 0u);
}

TEST(BpfLoaderHandoverTest, NullControlIsSafeNoOp)
{
    apply_handover(nullptr, 1);  // must not crash
}

// ---------------------------------------------------------------------------
// stage_bank (shadow-bank staging state machine)
// ---------------------------------------------------------------------------

struct MockBank {
    int clear_calls = 0;
    int tt_calls = 0;
    int meta_calls = 0;
    int cbs_calls = 0;
    int fail_tt_at = -1;  // -1 = never fail
    int fail_meta_at = -1;
    int fail_cbs_at = -1;
    int fail_clear = 0;   // nonzero = clear() fails
};

int mock_clear(void* ctx)
{
    auto* m = static_cast<MockBank*>(ctx);
    ++m->clear_calls;
    return m->fail_clear;
}

int mock_write_tt(void* ctx, int i)
{
    auto* m = static_cast<MockBank*>(ctx);
    ++m->tt_calls;
    return (m->fail_tt_at == i) ? -1 : 0;
}

int mock_write_meta(void* ctx, int i)
{
    auto* m = static_cast<MockBank*>(ctx);
    ++m->meta_calls;
    return (m->fail_meta_at == i) ? -1 : 0;
}

int mock_write_cbs(void* ctx, int i)
{
    auto* m = static_cast<MockBank*>(ctx);
    ++m->cbs_calls;
    return (m->fail_cbs_at == i) ? -1 : 0;
}

BankWriter make_writer(MockBank* m)
{
    BankWriter w = {};
    w.clear = mock_clear;
    w.write_tt = mock_write_tt;
    w.write_meta = mock_write_meta;
    w.write_cbs = mock_write_cbs;
    w.ctx = m;
    return w;
}

TEST(BpfLoaderStageTest, SuccessWritesAllEntries)
{
    MockBank m;
    EXPECT_TRUE(stage_bank(make_writer(&m), 3, 2, 1));
    EXPECT_EQ(m.clear_calls, 1);
    EXPECT_EQ(m.tt_calls, 3);
    EXPECT_EQ(m.meta_calls, 2);
    EXPECT_EQ(m.cbs_calls, 1);
}

TEST(BpfLoaderStageTest, MidWriteFailureDiscardsBank)
{
    MockBank m;
    m.fail_tt_at = 1;  // second TT write fails
    EXPECT_FALSE(stage_bank(make_writer(&m), 3, 2, 1));
    EXPECT_EQ(m.clear_calls, 2);  // initial clear + discard re-clear
    EXPECT_EQ(m.tt_calls, 2);     // no further TT writes after failure
    EXPECT_EQ(m.meta_calls, 0);   // later phases never started
    EXPECT_EQ(m.cbs_calls, 0);
}

TEST(BpfLoaderStageTest, MetaFailureDiscardsBank)
{
    MockBank m;
    m.fail_meta_at = 0;
    EXPECT_FALSE(stage_bank(make_writer(&m), 1, 2, 1));
    EXPECT_EQ(m.clear_calls, 2);
    EXPECT_EQ(m.cbs_calls, 0);
}

TEST(BpfLoaderStageTest, ClearFailureAbortsBeforeAnyWrite)
{
    MockBank m;
    m.fail_clear = -1;
    EXPECT_FALSE(stage_bank(make_writer(&m), 1, 1, 1));
    EXPECT_EQ(m.tt_calls, 0);
    EXPECT_EQ(m.meta_calls, 0);
    EXPECT_EQ(m.cbs_calls, 0);
}

TEST(BpfLoaderStageTest, MissingFunctionPointerIsRejected)
{
    MockBank m;
    BankWriter w = make_writer(&m);
    w.write_tt = nullptr;
    EXPECT_FALSE(stage_bank(w, 1, 1, 1));
    EXPECT_EQ(m.clear_calls, 0);
}

// ---------------------------------------------------------------------------
// KernelControl layout (must match the BPF .bss struct in maps.h)
// ---------------------------------------------------------------------------

TEST(BpfLoaderKernelControlTest, LayoutMatchesBpfBss)
{
    EXPECT_EQ(sizeof(KernelControl), 24u);
    EXPECT_EQ(offsetof(KernelControl, active_bank_idx), 0u);
    EXPECT_EQ(offsetof(KernelControl, tt_handover_gate), 4u);
    EXPECT_EQ(offsetof(KernelControl, bank_active_refs), 8u);
    EXPECT_EQ(sizeof(KernelControl::bank_active_refs), 16u);
}

}  // namespace
