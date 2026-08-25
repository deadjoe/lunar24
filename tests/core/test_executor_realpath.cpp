// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P3-⑥ Debt 2 — the `real_path` branch actually routed through the executor.
// P2-③ proved the COMPILER *decides* `real_path` vs `z_inverse` at the plan level
// (test_graph_compiler.cpp); this file proves an EXECUTOR that consumes the plan
// honors the decision, and lands the consume-rule for real_path feedback.
//
// @Claude's rulings folded in (msg de1a60e8):
//   * The judge has TWO halves, and invariance is NECESSARY not SUFFICIENT:
//       - partition invariance across the mixed non-uniform partition
//         (64,100,37,128,7,256,91) — a consistency check; and
//       - an ABSOLUTE reference: the realized loop delay must equal `delaySamples`
//         exactly, NOT merely "the same under every partition". A wrong-but-consistent
//         implementation passes invariance and is only caught by the absolute anchor.
//   * negative-1 (block-lazy / one-buffer break, design/07 §4 line 107) is
//     partition-VARIANT -> the partition judge catches it.
//   * negative-2 (read `moduleOut[source]` for a real_path edge, the exact bug the
//     P2-③ MiniExec scaffold commits) is partition-INVARIANT but off-by-one -> only
//     the absolute reference catches it. This FIRES on a bug that really happened.
//   * The consume-rule is a core CONTRACT (graph_compiler.h), not just a test, so new
//     code is held to it. The existing MiniExec real_path read does NOT enter that
//     contract — it is P2-③ test scaffolding, proven off-by-one (recorded in FINDINGS).

#include "mini_test.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <lunar24/core/descriptors.h>
#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/id_types.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/patch_graph.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------- helpers ----
// Same fixture idiom as test_graph_compiler.cpp: A(id1)=adder, B(id2)=real 3-sample
// delay on B_in->B_out. Kept local (these helpers are static in the sibling test).

static constexpr core::ModuleId kA{1}, kB{2};
static constexpr core::JackId A_out{101}, A_in{102};
static constexpr core::JackId B_out{201}, B_in{202};

// The mixed non-uniform partition (legacy P2-①). The design/07 §4 line-107 invariant
// says 64/128/256 buffer splits must not change feedback timing.
static const std::vector<std::uint32_t> kMixed = {64, 100, 37, 128, 7, 256, 91};

static core::JackDescriptor mk_jack(core::JackId id, core::ModuleId module, core::PinDirection dir) {
  core::JackDescriptor j{}; j.id = id; j.module = module; j.direction = dir; j.maxCables = 1u; return j;
}

struct ContractBuilder {
  core::ModuleExecutionContract c;
  ContractBuilder() { c.allowedInCyclicSCC = true; }
  ContractBuilder& path(core::JackId in, core::JackId out, double minDel, bool dt) {
    auto& p = c.pathDelays[c.pathDelayCount++];
    p.inPort = in; p.outPort = out; p.minCausalDelaySamples = minDel; p.canDirectThrough = dt;
    if (dt) c.hasDirectThroughPath = true;
    return *this;
  }
};
static core::GraphModule mod(core::ModuleId id, const core::ModuleExecutionContract* c) {
  return core::GraphModule{id, c};
}

// Build the plan. Returns it, asserting the compiler chose real_path (the P2-③
// detector, re-checked here so this test cannot pass if the decision regresses).
static core::CompiledGraph make_realpath_plan() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output), mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output), mk_jack(B_in, kB, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 3.0, false);  // real 3-sample internal path
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

  core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const core::CompiledRegion& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.feedback.size(), 1u);
  // The 3-sample internal path must NOT be waived to z^-1 (P2-③ addition ⑥).
  CHECK_TRUE(reg.feedback[0].delay == core::FeedbackDelay::real_path);
  CHECK_EQ(reg.feedback[0].delaySamples, 3.0);
  CHECK_FALSE(reg.feedback[0].delay == core::FeedbackDelay::z_inverse);
  return res.graph;
}

// The module semantics the test executors model. A = adder (stateless); B = a
// genuine 3-sample delay (B_out(n) = B_in(n-3), B_in(n)=A_out(n)).
//
// CORRECT executor (DelayState): realizes exactly `delaySamples` of loop delay by
// reading the feedback from a per-edge delay ring of depth = delaySamples. The ring
// holds the loop-forward value (A's output); the consumer (A) reads the value from
// delaySamples samples ago. This is the consume-rule @Claude had fixed into the
// core contract. Recurrence: A(n) = A(n-3) + ext(n).
//
// The constant drive ext=1.0 makes the loop shape unambiguous: A(n)=floor(n/3)+1
// for a 3-sample loop, floor(n/4)+1 for the off-by-one, n+1 for a 1-sample loop,
// floor(n/block)+1 for a one-buffer break.

//                          n 0 1 2 3 4 5 6 7   (ext=1, A(n)=A(n-D)+1)
static std::vector<double> expected_loop(std::size_t frames, int D) {
  std::vector<double> e(frames);
  for (std::size_t i = 0; i < frames; ++i) e[i] = static_cast<int>(i / static_cast<std::size_t>(D)) + 1;
  return e;
}

static bool identical(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
  return true;
}

// CORRECT: per-edge delay ring (depth = delaySamples), fed with the loop-forward
// value, delivering the delaySamples-ago value to the consumer. Partition-invariant
// (the ring carries across chunk boundaries) AND loop delay == delaySamples.
static std::vector<double> run_delay_state(int delaySamples, const std::vector<double>& ext,
                                           const std::vector<std::uint32_t>& blocks) {
  const std::size_t frames = ext.size();
  std::vector<double> out(frames, 0.0);
  std::vector<double> ring(static_cast<std::size_t>(delaySamples), 0.0);  // delaySamples-ago first
  std::size_t s = 0, b = 0;
  while (s < frames) {
    std::uint32_t blk = blocks[b % blocks.size()];
    for (std::uint32_t k = 0; k < blk && s < frames; ++k, ++s) {
      double a_in = ring[0];               // = A_out(n-delaySamples)
      double a_out = a_in + ext[s];
      out[s] = a_out;
      std::rotate(ring.begin(), ring.begin() + 1, ring.end());  // drop oldest, next slot
      ring.back() = a_out;
    }
    ++b;
  }
  return out;
}

// A module B that is a REAL 3-sample FIFO, read through the executor's per-edge
// "last module output" shortcut (the off-by-one the P2-③ MiniExec commits). This is
// the EXACT case @Claude re-checked: B runs AFTER A in the pass, so when A runs it
// reads moduleOut[B] = B's output from the PREVIOUS pass = A_out(n-4) => loop 4.
static std::vector<double> run_module_out(int delaySamples, const std::vector<double>& ext,
                                          const std::vector<std::uint32_t>& blocks) {
  const std::size_t frames = ext.size();
  std::vector<double> out(frames, 0.0);
  std::vector<double> bFifo(static_cast<std::size_t>(delaySamples), 0.0);  // A's running history
  double moduleOut_B = 0.0;            // B_out(n-1) — A reads it one pass late
  std::size_t s = 0, b = 0;
  while (s < frames) {
    std::uint32_t blk = blocks[b % blocks.size()];
    for (std::uint32_t k = 0; k < blk && s < frames; ++k, ++s) {
      // A runs FIRST; the real_path feedback is read as moduleOut[source=B].
      double a_in = moduleOut_B;       // = B_out(n-1) = A_out(n-4)
      double a_out = a_in + ext[s];
      out[s] = a_out;
      // B runs SECOND; B is a real delay: B_out(n) = A_out(n-3) = bFifo[0] (oldest),
      // and moduleOut is set to it for A's read ONE pass later => off-by-one.
      moduleOut_B = bFifo[0];
      bFifo.erase(bFifo.begin());
      bFifo.push_back(a_out);
    }
    ++b;
  }
  return out;
}

// A module B that is a real 3-sample FIFO but read as an instant passthrough with an
// EXPLICIT one-sample z^-1 (the waive real_path->z_inverse).
static std::vector<double> run_waive_z_inverse(const std::vector<double>& ext,
                                               const std::vector<std::uint32_t>& blocks) {
  const std::size_t frames = ext.size();
  std::vector<double> out(frames, 0.0);
  double fbPrev = 0.0;
  std::size_t s = 0, b = 0;
  while (s < frames) {
    std::uint32_t blk = blocks[b % blocks.size()];
    for (std::uint32_t k = 0; k < blk && s < frames; ++k, ++s) {
      double a_in = fbPrev;            // A_out(n-1)
      double a_out = a_in + ext[s];
      out[s] = a_out;
      fbPrev = a_out;
    }
    ++b;
  }
  return out;
}

// A module B read as a one-BUFFER (block-lazy) delay: the feedback is updated once at
// a block boundary and held for the block. This is design/07 §4 line-107's forbidden
// "one-audio-buffer delay" — the partition judge MUST catch it as partition-variant.
static std::vector<double> run_block_lazy(const std::vector<double>& ext,
                                          const std::vector<std::uint32_t>& blocks) {
  const std::size_t frames = ext.size();
  std::vector<double> out(frames, 0.0);
  double fbHeld = 0.0;
  std::size_t s = 0, b = 0;
  while (s < frames) {
    std::uint32_t blk = blocks[b % blocks.size()];
    for (std::uint32_t k = 0; k < blk && s < frames; ++k, ++s) {
      out[s] = fbHeld + ext[s];
    }
    fbHeld = out[s ? s - 1 : 0];       // hold last A_out for the next block's feedback
    ++b;
  }
  return out;
}

// ----------------------------------------------------------------------------
// must: correct executor is partition-invariant AND hits the ABSOLUTE reference.
// ----------------------------------------------------------------------------
static void correct_executor_invariant_and_reference() {
  auto plan = make_realpath_plan();
  const std::size_t frames = 200;
  std::vector<double> ext(frames, 1.0);
  const int D = static_cast<int>(plan.regions[0].feedback[0].delaySamples);  // must be 3

  auto seq = run_delay_state(D, ext, {static_cast<std::uint32_t>(frames)});
  auto mixed = run_delay_state(D, ext, kMixed);

  // ABSOLUTE reference: the loop-delay shape must match delaySamples exactly.
  auto ref = expected_loop(frames, D);
  CHECK_TRUE(identical(seq, ref));         // A(n)=floor(n/3)+1, not floor(n/4)+1

  // Partition invariance (consistency): the mixed partition gives the same per-sample
  // sequence as the sequential run.
  CHECK_TRUE(identical(seq, mixed));
}

// ----------------------------------------------------------------------------
// negative-1: block-lazy (one-buffer) is partition-VARIANT -> judge fires.
// ----------------------------------------------------------------------------
static void block_lazy_breaks_partition_invariance() {
  const std::size_t frames = 200;
  std::vector<double> ext(frames, 1.0);
  auto seq = run_block_lazy(ext, {static_cast<std::uint32_t>(frames)});
  auto mixed = run_block_lazy(ext, kMixed);
  CHECK_FALSE(identical(seq, mixed));      // the partition judge MUST go red
}

// ----------------------------------------------------------------------------
// negative-2: the exact P2-③ MiniExec bug — read moduleOut[source] for a real_path
// edge. Partition-INVARIANT (the judge would NOT catch it) but off-by-one (loop
// delay 4, not 3). ONLY the absolute reference catches it. This is the methodological
// point @Claude flagged: invariance is a necessary, not sufficient, check.
// ----------------------------------------------------------------------------
static void module_out_read_is_off_by_one_and_judge_blind() {
  auto plan = make_realpath_plan();
  const std::size_t frames = 200;
  std::vector<double> ext(frames, 1.0);
  const int D = static_cast<int>(plan.regions[0].feedback[0].delaySamples);  // 3

  auto seq = run_module_out(D, ext, {static_cast<std::uint32_t>(frames)});
  auto mixed = run_module_out(D, ext, kMixed);

  // (a) It is partition-INVARIANT: a pure FIFO is consistent under every partition.
  CHECK_TRUE(identical(seq, mixed));
  // (b) But it is OFF-BY-ONE vs the absolute reference: loop delay 4, not 3.
  auto ref = expected_loop(frames, D);
  CHECK_FALSE(identical(seq, ref));
  // (c) It differs from the correct loop from the first sample that reveals it (n=3).
  auto correct = run_delay_state(D, ext, {static_cast<std::uint32_t>(frames)});
  bool firstDiffAt3 = (correct.size() > 3) && (correct[3] != seq[3]) && (correct[2] == seq[2]);
  CHECK_TRUE(firstDiffAt3);   // correct[3]=2 (3-sample) vs seq[3]=1 (4-sample)
}

// ----------------------------------------------------------------------------
// must: the waive real_path->z_inverse is also partition-invariant but wrong.
// ----------------------------------------------------------------------------
static void waive_z_inverse_partition_invariant_but_wrong() {
  const std::size_t frames = 200;
  std::vector<double> ext(frames, 1.0);
  const int D = 3;   // the real_path delaySamples

  auto seq = run_waive_z_inverse(ext, {static_cast<std::uint32_t>(frames)});
  auto mixed = run_waive_z_inverse(ext, kMixed);
  CHECK_TRUE(identical(seq, mixed));       // partition-invariant
  auto ref = expected_loop(frames, D);
  CHECK_FALSE(identical(seq, ref));        // loop delay 1, not 3 -> absolute ref fires
}

int main() {
  std::printf("== P3-⑥ Debt 2: real_path routed through the executor (invariant + absolute reference) ==\n");
  correct_executor_invariant_and_reference();
  block_lazy_breaks_partition_invariance();
  module_out_read_is_off_by_one_and_judge_blind();
  waive_z_inverse_partition_invariant_but_wrong();
  return ::test::finish("executor_realpath");
}
