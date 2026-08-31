// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_host_stream_plan.cpp — the framework-free oracle for GH#4 8B3 (task#73) channel selection.
//
// This drives the ONE shared function negotiate_stream_plan() from host/include/host/stream_plan.h
// — the SAME function the repo IPlugAPP_host override calls in InitAudio. Because the host and the
// oracle call the same pure function, they cannot disagree: a host that opens a different count or
// first-channel than this oracle asserts is a wiring defect, and an oracle that drifts from the
// function the host compiles is impossible (there is only one stream_plan.h in the build).
//
// The @Codex 8B3 product criteria for the oracle:
//   * 0/1/2 input × 2/3/4/8 output capability matrix,
//   * output selection CONSUMES R (non-contiguous / duplicate / R-out-of-range REJECTS — L=1,R=3
//     must be an OUTPUT-INVALID, never a silently-"succeeded" 4),
//   * first-channel end-degrade-to-2 (an 8-out device opened from an end legal pair opens 2),
//   * non-contiguous / duplicate / out-of-range INPUT selection rejects (no implicit copy),
//   * input-off (0-in) is a VALID output-only plan, never a failure,
//   * every VALID plan is one of the six legal APP configs (is_legal_io), never over the cap.
//
// This is a pure function (no allocation, no state), so it needs no allocator probe: the whole
// negotiation is a branch on ints. Every check is an observable-behaviour assertion on the plan.

#include "mini_test.h"

#include <host/stream_plan.h>

namespace {

using lunar24::host::StreamPlan;
using lunar24::host::StreamPlanStatus;
using lunar24::host::is_legal_io;
using lunar24::host::negotiate_stream_plan;

// The max channel data the iPlug2 APP declares (config.h PLUG_CHANNEL_IO on the APP_API branch,
// "0-2 1-2 2-2 0-4 1-4 2-4"). A valid plan NEVER exceeds it, so it can never exceed the iPlug2
// channel data / over-read a smaller device.
constexpr int kMaxIn = 2;
constexpr int kMaxOut = 4;

struct Expect {
  int firstIn;        // expected 0-indexed first input channel.
  int firstOut;       // expected 0-indexed first output channel.
  int openIn;         // expected 0 / 1 / 2.
  int openOut;        // expected 0 / 2 / 4.
  StreamPlanStatus status;
};

// Drive one negotiation and check the whole plan. Any mismatch is an observable fail.
void check(int devIn, int devOut, int selInL, int selInR, int selOutL, int selOutR,
           const Expect& e, const char* label) {
  (void)label;  // human-readable case name; the CHECKs carry the actual assertions.
  const StreamPlan p =
      negotiate_stream_plan(devIn, devOut, selInL, selInR, selOutL, selOutR);
  CHECK_EQ(p.firstIn, e.firstIn);
  CHECK_EQ(p.firstOut, e.firstOut);
  CHECK_EQ(p.openIn, e.openIn);
  CHECK_EQ(p.openOut, e.openOut);
  CHECK(p.status == e.status);
  if (p.status != StreamPlanStatus::Valid) return;  // nothing more to assert.
  // Valid plan invariants: never over the declared policy cap.
  CHECK(p.openIn >= 0 && p.openIn <= kMaxIn);
  CHECK(p.openOut >= 0 && p.openOut <= kMaxOut);
  CHECK(p.firstIn >= 0 && (p.openIn == 0 || p.firstIn + p.openIn <= devIn));
  CHECK(p.firstOut >= 0 && (p.openOut == 0 || p.firstOut + p.openOut <= devOut));
  // G1 product criterion: every VALID plan is one of the six legal APP configs the iPlug2 APP
  // declares (config.h PLUG_CHANNEL_IO "0-2 1-2 2-2 0-4 1-4 2-4"). This is what ties the negotiated
  // plan back to the parsed config: the plan can never open a config the channel data cannot hold.
  CHECK(is_legal_io(p.openIn, p.openOut));
}

// ---- input × output capability matrix ------------------------------------------------
void capability_matrix() {
  // INPUT 0 (both sides off) — a VALID output-only plan, never a failure on missing input ID.
  check(0, 4, 0, 0, 1, 2, {0, 0, 0, 4, StreamPlanStatus::Valid}, "in0/out4/12->4");
  check(0, 2, 0, 0, 1, 2, {0, 0, 0, 2, StreamPlanStatus::Valid}, "in0/out2/12->2");

  // INPUT 1 (a single valid mono L choice) — owner routes ExtOnly.
  check(1, 4, 1, 0, 1, 2, {0, 0, 1, 4, StreamPlanStatus::Valid}, "in1/out4/L/12->4");
  check(2, 2, 2, 0, 1, 2, {1, 0, 1, 2, StreamPlanStatus::Valid}, "in2/out2/L2/12->2");
  check(4, 8, 3, 0, 5, 6, {2, 4, 1, 4, StreamPlanStatus::Valid}, "in4/out8/L3/56->4");

  // INPUT 2 (an adjacent in-range pair) — owner routes Distinct, never an implicit copy.
  check(2, 4, 1, 2, 1, 2, {0, 0, 2, 4, StreamPlanStatus::Valid}, "in2/out4/12/12->4");
  check(4, 4, 3, 4, 1, 2, {2, 0, 2, 4, StreamPlanStatus::Valid}, "in4/out4/34/12->4");
  check(4, 2, 2, 3, 1, 2, {1, 0, 2, 2, StreamPlanStatus::Valid}, "in4/out2/23/12->2");
}

// ---- output first-channel end-degrade-to-2 --------------------------------------------
void output_end_degrade() {
  // An 8-out device opened from an END legal pair (channels 7,8; 6,7) degrades to 2, NEVER a
  // forced 4 (which would read past the openable run).
  check(0, 8, 0, 0, 7, 8, {0, 6, 0, 2, StreamPlanStatus::Valid}, "out8/78->2");
  check(0, 8, 0, 0, 6, 7, {0, 5, 0, 2, StreamPlanStatus::Valid}, "out8/67->2");
  // A full 8 device from 1,2 opens 4; from 5,6 there are exactly 4 left (5..8) -> 4.
  check(0, 8, 0, 0, 1, 2, {0, 0, 0, 4, StreamPlanStatus::Valid}, "out8/12->4");
  check(0, 8, 0, 0, 5, 6, {0, 4, 0, 4, StreamPlanStatus::Valid}, "out8/56->4");
  // A 3-out device only satisfies WET (2), never 4.
  check(0, 3, 0, 0, 1, 2, {0, 0, 0, 2, StreamPlanStatus::Valid}, "out3/12->2");
  check(0, 3, 0, 0, 2, 3, {0, 1, 0, 2, StreamPlanStatus::Valid}, "out3/23->2");
  // A 2-out device opens exactly its legal pair (2).
  check(0, 2, 0, 0, 1, 2, {0, 0, 0, 2, StreamPlanStatus::Valid}, "out2/12->2");
}

// ---- output selection REJECTS (consumes R; no implicit copy) -------------------------
void output_rejects() {
  // No output selected (both off) -> OUTPUT-INVALID (the WET strategy needs a pair).
  check(0, 4, 0, 0, 0, 0, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-off");
  // Incomplete: only L (R off) -> OUTPUT-INVALID.
  check(0, 4, 0, 0, 2, 0, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-monooR-off");
  // Duplicate (L==R) -> OUTPUT-INVALID.
  check(0, 4, 0, 0, 2, 2, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-dup");
  // Non-contiguous (R != L+1) -> OUTPUT-INVALID (this is the @Codex L=1,R=3 case).
  check(0, 4, 0, 0, 1, 3, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-nonadj13");
  check(0, 4, 0, 0, 2, 4, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-nonadj24");
  // R out-of-range (R > deviceOutputChans) -> OUTPUT-INVALID.
  check(0, 4, 0, 0, 4, 5, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-R-oob");
  check(0, 4, 0, 0, 1, 5, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out-R-oob2");
  // A device that cannot satisfy the WET strategy (fewer than 2 openable) has no legal pair, so
  // ANY selection on it is rejected, never silently "opened".
  check(0, 1, 0, 0, 1, 2, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out1->invalid");
  check(0, 0, 0, 0, 1, 2, {0, 0, 0, 0, StreamPlanStatus::OutputInvalid}, "out0->invalid");
}

// ---- input selection rejects (no implicit copy) --------------------------------------
void input_rejects() {
  // Duplicate (L==R) -> invalid.
  check(4, 4, 2, 2, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "dup");
  // Non-contiguous (R != L+1) -> invalid.
  check(4, 4, 1, 3, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "nonadj");
  check(4, 4, 2, 4, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "nonadj2");
  // Out-of-range (L or R beyond device) -> invalid.
  check(2, 4, 3, 0, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "L-out-of-range");
  check(2, 4, 1, 3, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "R-out-of-range");
  check(2, 4, 2, 3, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "R-out-of-range2");
  // R-only mono (L off) is not a contiguous-from-1 selection -> invalid (mono is L-anchored).
  check(4, 4, 0, 2, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "R-only");
  // A lone out-of-range L on an output-only (0-in) device -> input fails first.
  check(0, 4, 1, 0, 1, 2, {0, 0, 0, 0, StreamPlanStatus::InputInvalid}, "in0dev-L");
}

// ---- the six legal APP configs are exactly is_legal_io --------------------------------
void legal_config_set() {
  // The iPlug2 APP declares "0-2 1-2 2-2 0-4 1-4 2-4" (max 2-in/4-out). Assert the set is exactly
  // those six, and that the CAP is respected (the product criterion for the parsed config).
  const bool cap = kMaxIn == 2 && kMaxOut == 4;
  CHECK_TRUE(cap);
  CHECK_TRUE(is_legal_io(0, 2) && is_legal_io(1, 2) && is_legal_io(2, 2));
  CHECK_TRUE(is_legal_io(0, 4) && is_legal_io(1, 4) && is_legal_io(2, 4));
  // Anything outside the declared set is rejected by the admission criterion.
  CHECK_FALSE(is_legal_io(3, 2));  // 3 inputs is not a legal APP config.
  CHECK_FALSE(is_legal_io(2, 3));  // 3 outputs is not a legal APP config.
  CHECK_FALSE(is_legal_io(2, 0));  // 0 outputs is not a legal WET config.
  CHECK_FALSE(is_legal_io(0, 1));  // 1 output is not a legal WET config.
  CHECK_FALSE(is_legal_io(4, 4));  // 4 inputs exceeds the max.
}

}  // namespace

int main() {
  std::printf("== GH#4 8B3: channel plan oracle (negotiate_stream_plan) ==\n");
  capability_matrix();
  output_end_degrade();
  output_rejects();
  input_rejects();
  legal_config_set();
  return ::test::finish("host_stream_plan");
}
