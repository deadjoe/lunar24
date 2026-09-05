// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-③ tests for the SCC cycle-breaking / graph compiler
// (core/include/lunar24/core/graph_compiler.h; design/07 §4).
//
// These cover @Claude's 5 must-tests plus his 3 additions and the boundary set.
//
// @Claude must-tests:
//   ① z^-1 is one SAMPLE, not one block  — a cyclic region run through a
//      test-only mini interpreter at the P2-① mixed block partition
//      (64,100,37,128,7,256,91) must yield per-sample-identical output.
//   ② feedback-edge set deterministic across plug / enum / container order —
//      the SAME topology compiled from several edge + module permutations gives a
//      bit-identical plan; the comparator can detect an order perturbation, so
//      this is a live detector, not a green light.
//   ③ module-level intrinsic latency grants NO break eligibility — a module that
//      reports a large intrinsicLatencySamples but whose specific on-cycle path
//      is zero-delay is still given a z^-1 feedback edge.
//   ④ a not-cycle-safe module (window / FFT / reverse) is REJECTED, never
//      silently admitted into a cyclic SCC.
//   ⑤ acyclic / cyclic partition is correct — a pure acyclic chain is not
//      misjudged as cyclic and a real loop is not missed.
//
// @Claude additions:
//   ⑥ both z^-1 directions are guarded —
//        (negative of "should add but didn't") and
//        (positive of "path has real >=1 sample delay -> MUST NOT add z^-1").
//   ⑦ the feedback-edge set is NOT full-mark: it is SUFFICIENT (removing it makes
//      the SCC a DAG) AND NON-REDUNDANT (re-adding any single selected edge
//      recreates a cycle). Verified independently of the algorithm that chose it.
//   ⑧ a recompile NEVER runs on the audio thread — trigger one inside an RtGuard
//      window and the heap detector must go red; the same compile off-thread is
//      clean.
//
// Boundaries: self-loop; two-node mutual feedback; multiple independent SCCs;
// nested cycles inside one SCC; a runtime topology change that forces a recompile.

#include "mini_test.h"
#include "rt_guard_test.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <lunar24/core/descriptors.h>
#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/id_types.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/patch_graph.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------- helpers ----

static std::uint32_t num(core::ModuleId v) { return static_cast<std::uint32_t>(v); }
static std::uint32_t num(core::JackId v) { return static_cast<std::uint32_t>(v); }

static constexpr std::uint32_t kMaxTestId = 1024;

// Module ids. {1}=sum(add external ramp), {2}=half(*0.5), others=passthrough.
// These are the transfer functions the mini interpreter recognises.
static constexpr core::ModuleId kA{1}, kB{2}, kC{3}, kD{4};
static constexpr core::ModuleId kWindow{50};  // NOT cycle-safe

// Jack ids, keyed to a module. Each module gets an out (odd) and an in (even).
static constexpr core::JackId A_out{101}, A_in{102};
static constexpr core::JackId B_out{201}, B_in{202};
static constexpr core::JackId C_out{301}, C_in{302};
static constexpr core::JackId D_out{401}, D_in{402};

static core::JackDescriptor mk_jack(core::JackId id, core::ModuleId module,
                                    core::PinDirection dir) {
  core::JackDescriptor j{};
  j.id = id;
  j.module = module;
  j.direction = dir;
  j.maxCables = 1u;
  return j;
}

// Fluent builder for a prepared ModuleExecutionContract. Keeps hasDirectThroughPath
// as the exact OR of the per-path canDirectThrough flags so module_contract_is_valid
// holds for every contract we build.
struct ContractBuilder {
  core::ModuleExecutionContract c;
  // A prepared module is cycle-safe by default; only a window/FFT/reverse module
  // opts out (cycSafe(false)).
  ContractBuilder() { c.allowedInCyclicSCC = true; }

  ContractBuilder& path(core::JackId inPin, core::JackId outPin, double minDel,
                        bool directThrough) {
    auto& p = c.pathDelays[c.pathDelayCount++];
    p.inPort = inPin;
    p.outPort = outPin;
    p.minCausalDelaySamples = minDel;
    p.canDirectThrough = directThrough;
    if (directThrough) c.hasDirectThroughPath = true;
    return *this;
  }
  ContractBuilder& cycSafe(bool ok) { c.allowedInCyclicSCC = ok; return *this; }
  ContractBuilder& intrinsic(std::uint32_t n) { c.intrinsicLatencySamples = n; return *this; }
};

static core::GraphModule mod(core::ModuleId id, const core::ModuleExecutionContract* c) {
  return core::GraphModule{id, c};
}

// ----------------------------------------------------------------------------
// Sufficiency / non-redundancy verifiers (@Claude addition ⑦). These RE-CHECK the
// chosen feedback set independently of the DFS that chose it, using the two cheap
// O(|edges|) invariants he named (NOT an NP-hard optimum).
// ----------------------------------------------------------------------------
using ModEdge = std::pair<std::uint32_t, std::uint32_t>;  // numeric module ids

// A directed multi-graph over module nodes has a cycle iff a DFS finds any back
// edge; a self-edge is trivially a cycle.
static bool has_cycle(const std::vector<ModEdge>& edges, std::uint32_t nodeCount) {
  std::vector<std::vector<std::uint32_t>> adj(nodeCount);
  for (const auto& e : edges) {
    if (e.first == e.second) return true;  // self-edge
    adj[e.first].push_back(e.second);
  }
  for (auto& a : adj) std::sort(a.begin(), a.end());
  std::vector<char> state(nodeCount, 0);  // 0 white, 1 gray, 2 black
  bool cyc = false;
  std::function<void(std::uint32_t)> dfs = [&](std::uint32_t u) {
    if (cyc) return;
    state[u] = 1;
    for (std::uint32_t v : adj[u]) {
      if (state[v] == 1) { cyc = true; return; }
      if (state[v] == 0) { dfs(v); if (cyc) return; }
    }
    state[u] = 2;
  };
  for (std::uint32_t u = 0; u < nodeCount && !cyc; ++u)
    if (state[u] == 0) dfs(u);
  return cyc;
}

static bool in_feedback(const core::CompiledRegion& r, core::JackId s, core::JackId t) {
  for (const auto& f : r.feedback)
    if (f.sourceJack == s && f.sinkJack == t) return true;
  return false;
}

// Remove the feedback edges from a region's edge set and check the rest is a
// DAG (sufficiency).
static bool feedback_set_is_sufficient(const core::CompiledRegion& r,
                                       const core::JackDescriptor* jacks,
                                       std::uint32_t jackCount, std::uint32_t nodeCount) {
  std::vector<ModEdge> kept;
  for (const auto& e : r.edges)
    if (!in_feedback(r, e.sourceJack, e.sinkJack)) {
      std::uint32_t sm = 0, tm = 0;
      for (std::uint32_t i = 0; i < jackCount; ++i) {
        if (jacks[i].id == e.sourceJack) sm = num(jacks[i].module);
        if (jacks[i].id == e.sinkJack) tm = num(jacks[i].module);
      }
      kept.emplace_back(sm, tm);
    }
  return !has_cycle(kept, nodeCount);
}

// Re-adding ANY single selected feedback edge recreates a cycle (non-redundancy).
static bool feedback_set_non_redundant(const core::CompiledRegion& r,
                                       const core::JackDescriptor* jacks,
                                       std::uint32_t jackCount, std::uint32_t nodeCount) {
  for (const auto& f : r.feedback) {
    std::vector<ModEdge> edges;
    for (const auto& e : r.edges)
      if (!in_feedback(r, e.sourceJack, e.sinkJack)) {
        std::uint32_t sm = 0, tm = 0;
        for (std::uint32_t i = 0; i < jackCount; ++i) {
          if (jacks[i].id == e.sourceJack) sm = num(jacks[i].module);
          if (jacks[i].id == e.sinkJack) tm = num(jacks[i].module);
        }
        edges.emplace_back(sm, tm);
      }
    std::uint32_t fsm = 0, ftm = 0;
    for (std::uint32_t i = 0; i < jackCount; ++i) {
      if (jacks[i].id == f.sourceJack) fsm = num(jacks[i].module);
      if (jacks[i].id == f.sinkJack) ftm = num(jacks[i].module);
    }
    edges.emplace_back(fsm, ftm);
    if (!has_cycle(edges, nodeCount)) return false;  // this single edge was redundant
  }
  return true;
}

// ----------------------------------------------------------------------------
// Test-only mini DSP interpreter. Generic over the plan it is given: it walks the
// regions in order, runs an acyclic region's single module once per block and a
// cyclic region per-sample in region.modules order, and honours the compiler's
// z_inverse vs real_path decision with an internal one-sample delay line. Used to
// prove must-test ① (z^-1 is one sample, not one block).
// ----------------------------------------------------------------------------
struct MiniExec {
  std::vector<std::uint32_t> moduleOf;   // numeric jack id -> numeric module id
  std::vector<float> moduleOut;          // numeric module id -> last sample output

  explicit MiniExec(const core::JackDescriptor* jacks, std::uint32_t jackCount)
      : moduleOf(kMaxTestId, 0xFFFFFFFFu), moduleOut(kMaxTestId, 0.0f) {
    for (std::uint32_t i = 0; i < jackCount; ++i)
      if (num(jacks[i].id) < kMaxTestId) moduleOf[num(jacks[i].id)] = num(jacks[i].module);
  }

  float transfer(core::ModuleId m, float in, float ext) const {
    switch (num(m)) {
      case 1: return in + ext;   // sum: adds the external ramp
      case 2: return in * 0.5f;  // half
      default: return in;        // passthrough
    }
  }

  static int feedback_index(const core::CompiledRegion& r, core::JackId s, core::JackId t) {
    for (std::size_t i = 0; i < r.feedback.size(); ++i)
      if (r.feedback[i].sourceJack == s && r.feedback[i].sinkJack == t) return static_cast<int>(i);
    return -1;
  }

  // Runs `plan` for `frames` samples in blocks of `block`, recording the output
  // of `probe` at every sample. `ext` is the external per-sample ramp.
  std::vector<float> run(const core::CompiledGraph& plan, std::size_t frames,
                         std::size_t block, const std::vector<float>& ext,
                         core::ModuleId probe) {
    std::vector<float> result(frames);
    std::vector<std::vector<float>> delayNow(plan.regions.size());
    std::vector<std::vector<float>> delayNext(plan.regions.size());
    for (std::size_t r = 0; r < plan.regions.size(); ++r) {
      delayNow[r].assign(plan.regions[r].feedback.size(), 0.f);
      delayNext[r].assign(plan.regions[r].feedback.size(), 0.f);
    }
    std::fill(moduleOut.begin(), moduleOut.end(), 0.f);

    std::size_t s = 0;
    while (s < frames) {
      std::size_t end = std::min(frames, s + block);
      for (std::size_t n = s; n < end; ++n) {
        for (std::size_t r = 0; r < plan.regions.size(); ++r) {
          const auto& reg = plan.regions[r];
          if (reg.kind == core::RegionKind::acyclic) {
            float out = transfer(reg.modules[0], 0.f, ext[n]);
            moduleOut[num(reg.modules[0])] = out;
          } else {
            for (std::uint32_t mi = 0; mi < reg.modules.size(); ++mi) {
              std::uint32_t m = num(reg.modules[mi]);
              float in = 0.f;
              for (std::size_t ei = 0; ei < reg.edges.size(); ++ei) {
                const auto& e = reg.edges[ei];
                if (moduleOf[num(e.sinkJack)] != m) continue;
                int fb = feedback_index(reg, e.sourceJack, e.sinkJack);
                if (fb >= 0) {
                  const auto& fe = reg.feedback[static_cast<std::size_t>(fb)];
                  if (fe.delay == core::FeedbackDelay::z_inverse)
                    in += delayNow[r][static_cast<std::size_t>(fb)];
                  else
                    in += moduleOut[moduleOf[num(e.sourceJack)]];
                } else {
                  in += moduleOut[moduleOf[num(e.sourceJack)]];
                }
              }
              float out = transfer(reg.modules[mi], in, ext[n]);
              moduleOut[m] = out;
              for (std::size_t fi = 0; fi < reg.feedback.size(); ++fi)
                if (moduleOf[num(reg.feedback[fi].sourceJack)] == m)
                  delayNext[r][fi] = out;
            }
            for (std::size_t fi = 0; fi < reg.feedback.size(); ++fi)
              delayNow[r][fi] = delayNext[r][fi];
          }
        }
        result[n] = moduleOut[num(probe)];
      }
      s = end;
    }
    return result;
  }
};

static bool result_identical(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

// ----------------------------------------------------------------------------
// Canonical serialization + comparator (must-test ②).
// ----------------------------------------------------------------------------
static std::string serialize(const core::CompiledGraph& g) {
  std::string s;
  s += "M" + std::to_string(g.moduleCount) + ";";
  for (const auto& r : g.regions) {
    s += (r.kind == core::RegionKind::acyclic) ? "acy;" : "cyc;";
    for (auto m : r.modules) s += "m" + std::to_string(num(m)) + ",";
    s += "|";
    for (const auto& e : r.edges)
      s += "e" + std::to_string(num(e.sourceJack)) + ">" + std::to_string(num(e.sinkJack)) + ",";
    s += "|";
    for (const auto& f : r.feedback)
      s += "f" + std::to_string(num(f.sourceJack)) + ">" + std::to_string(num(f.sinkJack)) +
           ":" + (f.delay == core::FeedbackDelay::z_inverse ? "z" : "r") +
           std::to_string(f.delaySamples) + ",";
    s += ";";
  }
  return s;
}

static bool plans_bit_identical(const core::CompiledGraph& a, const core::CompiledGraph& b) {
  return serialize(a) == serialize(b);
}

// A concrete order perturbation: reverse each region's edge / feedback / module
// lists. Used only to prove the comparator IS order-sensitive, so an order-
// dependent compiler would be caught (must-test ② negative).
static core::CompiledGraph buggy_reorder_edges(const core::CompiledGraph& in) {
  core::CompiledGraph out;
  out.moduleCount = in.moduleCount;
  out.regions = in.regions;
  for (auto& r : out.regions) {
    std::reverse(r.modules.begin(), r.modules.end());
    std::reverse(r.edges.begin(), r.edges.end());
    std::reverse(r.feedback.begin(), r.feedback.end());
  }
  return out;
}

// ----------------------------------------------------------------------------
// must-test ①: z^-1 is one sample, not one block.
// ----------------------------------------------------------------------------
static void z_inverse_is_one_sample_not_one_block() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);

  ContractBuilder ba;
  ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb;
  bb.path(B_in, B_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};

  const core::PatchEdge edges[] = {
      {A_out, B_in},   // A -> B
      {B_out, A_in},   // B -> A (closes the loop)
  };
  const std::uint32_t kEdgeCount = sizeof(edges) / sizeof(edges[0]);

  core::CompileResult res =
      core::compile_graph(jacks, kJackCount, edges, kEdgeCount, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.moduleCount, 2u);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 2u);
  // The feedback edge B_out -> A_in is a ONE-sample z^-1, never a block delay.
  CHECK_EQ(reg.feedback.size(), 1u);
  CHECK_TRUE(reg.feedback[0].sourceJack == B_out);
  CHECK_TRUE(reg.feedback[0].sinkJack == A_in);
  CHECK_TRUE(reg.feedback[0].delay == core::FeedbackDelay::z_inverse);
  CHECK_EQ(reg.feedback[0].delaySamples, 1.0);
  // Sufficiency + non-redundancy hold for the chosen set.
  CHECK_TRUE(feedback_set_is_sufficient(reg, jacks, kJackCount, kMaxTestId));
  CHECK_TRUE(feedback_set_non_redundant(reg, jacks, kJackCount, kMaxTestId));

  // Drive the plan through the mini interpreter at the P2-① mixed partition.
  const std::size_t kFrames = 256;
  const std::size_t kBlocks[] = {64u, 100u, 37u, 128u, 7u, 256u, 91u};
  std::vector<float> ext(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) ext[i] = static_cast<float>(i % 101) * 0.01f;

  MiniExec exec(jacks, kJackCount);
  std::vector<float> ref = exec.run(res.graph, kFrames, kBlocks[0], ext, kB);
  for (std::size_t b = 1; b < sizeof(kBlocks) / sizeof(kBlocks[0]); ++b) {
    std::vector<float> got = exec.run(res.graph, kFrames, kBlocks[b], ext, kB);
    CHECK_TRUE(result_identical(ref, got));  // per-sample identical across block size
  }

  // Negative: a ONE-BLOCK delay (not one sample) WOULD make output depend on the
  // block size. Simulate a buggy executor that delays by `block` samples.
  auto buggy = [&](std::size_t block) {
    std::vector<float> r(kFrames);
    std::deque<float> hist(block, 0.f);
    float aIn = 0.f, aOut = 0.f, bOut = 0.f;
    // A = sum, then B = half, feedback B->A delayed `block` samples.
    for (std::size_t n = 0; n < kFrames; ++n) {
      aIn = hist.front();
      hist.pop_front();
      aOut = aIn + ext[n];
      bOut = aOut * 0.5f;
      hist.push_back(bOut);
      r[n] = bOut;
    }
    return r;
  };
  std::vector<float> b64 = buggy(64u);
  std::vector<float> b88 = buggy(88u);
  CHECK_FALSE(result_identical(b64, b88));  // the one-block bug IS detectable
}

// ----------------------------------------------------------------------------
// must-test ⑤: acyclic / cyclic partition.
// ----------------------------------------------------------------------------
static void acyclic_not_misjudged_and_cyclic_not_missed() {
  // Pure acyclic chain A -> B: two acyclic regions, no cyclic region.
  {
    const core::JackDescriptor jacks[] = {
        mk_jack(A_out, kA, core::PinDirection::output),
        mk_jack(B_in, kB, core::PinDirection::input),
    };
    ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
    ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
    const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
    const core::PatchEdge edges[] = {{A_out, B_in}};

    core::CompileResult res = core::compile_graph(jacks, 2, edges, 1, mods, 2);
    CHECK_TRUE(res.status == core::CompileStatus::ok);
    CHECK_EQ(res.graph.moduleCount, 2u);
    CHECK_EQ(res.graph.regions.size(), 2u);
    for (const auto& r : res.graph.regions)
      CHECK_TRUE(r.kind == core::RegionKind::acyclic);  // no false cyclic
    // Condensation-DAG order: A region before B region.
    CHECK_TRUE(res.graph.regions[0].modules[0] == kA);
    CHECK_TRUE(res.graph.regions[1].modules[0] == kB);
  }

  // Two-node mutual feedback A <-> B: one cyclic region, feedback present.
  {
    const core::JackDescriptor jacks[] = {
        mk_jack(A_out, kA, core::PinDirection::output),
        mk_jack(A_in, kA, core::PinDirection::input),
        mk_jack(B_out, kB, core::PinDirection::output),
        mk_jack(B_in, kB, core::PinDirection::input),
    };
    ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
    ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
    const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
    const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

    core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
    CHECK_TRUE(res.status == core::CompileStatus::ok);
    CHECK_EQ(res.graph.regions.size(), 1u);
    CHECK_TRUE(res.graph.regions[0].kind == core::RegionKind::cyclic);  // not missed
    CHECK_EQ(res.graph.regions[0].feedback.size(), 1u);
  }
}

// ----------------------------------------------------------------------------
// must-test ②: feedback set deterministic across order.
// ----------------------------------------------------------------------------
static void feedback_set_deterministic_across_order() {
  // A loop (A <-> B) plus an acyclic tail (A -> C). Topology is fixed; we vary the
  // edge order and the module-table order across permutations.
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
      mk_jack(C_out, kC, core::PinDirection::output),
      mk_jack(C_in, kC, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
  ContractBuilder bc; bc.path(C_in, C_out, 0.0, true);

  const core::PatchEdge e1{A_out, B_in};
  const core::PatchEdge e2{B_out, A_in};
  const core::PatchEdge e3{A_out, C_in};
  const core::PatchEdge perm1[] = {e1, e2, e3};
  const core::PatchEdge perm2[] = {e3, e1, e2};
  const core::PatchEdge perm3[] = {e2, e3, e1};
  const std::uint32_t k3 = 3;

  const core::GraphModule modABC[] = {mod(kA, &ba.c), mod(kB, &bb.c), mod(kC, &bc.c)};
  const core::GraphModule modCBA[] = {mod(kC, &bc.c), mod(kB, &bb.c), mod(kA, &ba.c)};

  core::CompileResult r1 = core::compile_graph(jacks, kJackCount, perm1, k3, modABC, 3);
  core::CompileResult r2 = core::compile_graph(jacks, kJackCount, perm2, k3, modABC, 3);
  core::CompileResult r3 = core::compile_graph(jacks, kJackCount, perm3, k3, modCBA, 3);

  CHECK_TRUE(r1.status == core::CompileStatus::ok);
  CHECK_TRUE(plans_bit_identical(r1.graph, r2.graph));
  CHECK_TRUE(plans_bit_identical(r1.graph, r3.graph));  // edge AND module-order independent

  // Negative: the comparator is genuinely order-sensitive. Perturbing the edge list
  // MUST make the comparison fail, so an order-dependent compiler could not pass.
  core::CompiledGraph perturbed = buggy_reorder_edges(r1.graph);
  CHECK_FALSE(plans_bit_identical(r1.graph, perturbed));
}

// ----------------------------------------------------------------------------
// must-test ③ + addition ⑥ (both z^-1 directions).
// ----------------------------------------------------------------------------
static void module_latency_grants_no_break_eligibility() {
  // B reports a LARGE intrinsic latency (999) but its specific on-cycle path
  // (B_in -> B_out) is zero-delay: the compiler must STILL add z^-1.
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true).intrinsic(999u);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

  core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_TRUE(res.graph.regions[0].kind == core::RegionKind::cyclic);
  CHECK_EQ(res.graph.regions[0].feedback.size(), 1u);
  // The large module-wide latency must NOT waive the z^-1.
  CHECK_TRUE(res.graph.regions[0].feedback[0].delay == core::FeedbackDelay::z_inverse);

  // Negative: a compiler that DID honour module-level latency would have marked
  // this edge real_path. Show that the decision we depend on is a live detector.
  bool buggy_waives = res.graph.regions[0].feedback[0].delay == core::FeedbackDelay::real_path;
  CHECK_FALSE(buggy_waives);  // the real compiler does NOT waive; a buggy one would be red
}

// addition ⑥(b): a real >=1-sample on-cycle path must NOT be given an extra z^-1.
static void real_path_delay_not_waived_to_z_inverse() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 3.0, false);  // real 3-sample internal path
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

  core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions[0].feedback.size(), 1u);
  // The 3-sample internal path carries the delay: MUST NOT add z^-1 here.
  CHECK_TRUE(res.graph.regions[0].feedback[0].delay == core::FeedbackDelay::real_path);
  CHECK_EQ(res.graph.regions[0].feedback[0].delaySamples, 3.0);
  // And that a wrong z_inverse would be caught.
  CHECK_FALSE(res.graph.regions[0].feedback[0].delay == core::FeedbackDelay::z_inverse);
}

// ----------------------------------------------------------------------------
// must-test ④: not-cycle-safe module is rejected.
// ----------------------------------------------------------------------------
static void cycle_unsafe_module_rejected() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kWindow, core::PinDirection::output),
      mk_jack(B_in, kWindow, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bw; bw.path(B_in, B_out, 0.0, true).cycSafe(false);  // window/FFT class
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kWindow, &bw.c)};
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

  core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
  // MUST reject, never silently admit the not-cycle-safe module.
  CHECK_TRUE(res.status == core::CompileStatus::cycle_unsafe_module);
  CHECK_EQ(res.graph.moduleCount, 0u);   // graph is empty on rejection
  CHECK_EQ(res.graph.regions.size(), 0u);

  // A window-type module is fine in a pure acyclic graph (no rejection).
  const core::PatchEdge edges2[] = {{A_out, B_in}};
  core::CompileResult res2 = core::compile_graph(jacks, 4, edges2, 1, mods, 2);
  CHECK_TRUE(res2.status == core::CompileStatus::ok);
}

// ----------------------------------------------------------------------------
// addition ⑦: sufficient + non-redundant, verified across several topologies.
// ----------------------------------------------------------------------------
static void feedback_set_sufficient_and_non_redundant() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
      mk_jack(C_out, kC, core::PinDirection::output),
      mk_jack(C_in, kC, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
  ContractBuilder bc; bc.path(C_in, C_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c), mod(kC, &bc.c)};

  // Strictly-connected A-B-C (A->B, B->C, C->A, plus chord A->C): ONE SCC with
  // more than one internal cycle, so the chosen set must break all of them.
  const core::PatchEdge edges[] = {
      {A_out, B_in}, {B_out, C_in}, {C_out, A_in}, {A_out, C_in}};
  core::CompileResult res =
      core::compile_graph(jacks, kJackCount, edges, 4, mods, 3);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 3u);
  CHECK_TRUE(!reg.feedback.empty());
  CHECK_TRUE(feedback_set_is_sufficient(reg, jacks, kJackCount, kMaxTestId));
  CHECK_TRUE(feedback_set_non_redundant(reg, jacks, kJackCount, kMaxTestId));
}

// ----------------------------------------------------------------------------
// Boundary: self-loop.
// ----------------------------------------------------------------------------
static void self_loop_compiles() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c)};
  const core::PatchEdge edges[] = {{A_out, A_in}};  // a module patched into itself

  core::CompileResult res = core::compile_graph(jacks, 2, edges, 1, mods, 1);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 1u);
  CHECK_EQ(reg.feedback.size(), 1u);
  CHECK_TRUE(reg.feedback[0].sourceJack == A_out);
  CHECK_TRUE(reg.feedback[0].sinkJack == A_in);
  CHECK_TRUE(reg.feedback[0].delay == core::FeedbackDelay::z_inverse);
  CHECK_TRUE(feedback_set_is_sufficient(reg, jacks, 2, kMaxTestId));
  CHECK_TRUE(feedback_set_non_redundant(reg, jacks, 2, kMaxTestId));
}

// ----------------------------------------------------------------------------
// Boundary: multiple independent SCCs.
// ----------------------------------------------------------------------------
static void multiple_independent_sccs() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
      mk_jack(C_out, kC, core::PinDirection::output),
      mk_jack(C_in, kC, core::PinDirection::input),
      mk_jack(D_out, kD, core::PinDirection::output),
      mk_jack(D_in, kD, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
  ContractBuilder bc; bc.path(C_in, C_out, 0.0, true);
  ContractBuilder bd; bd.path(D_in, D_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c),
                                    mod(kC, &bc.c), mod(kD, &bd.c)};
  // Two disjoint loops A<->B and C<->D.
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in},
                                   {C_out, D_in}, {D_out, C_in}};

  core::CompileResult res = core::compile_graph(jacks, 8, edges, 4, mods, 4);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 2u);
  for (const auto& r : res.graph.regions) {
    CHECK_TRUE(r.kind == core::RegionKind::cyclic);
    CHECK_EQ(r.modules.size(), 2u);
    CHECK_TRUE(feedback_set_is_sufficient(r, jacks, 8, kMaxTestId));
    CHECK_TRUE(feedback_set_non_redundant(r, jacks, 8, kMaxTestId));
  }
}

// ----------------------------------------------------------------------------
// Boundary: runtime topology change (epoch-driven recompile).
// ----------------------------------------------------------------------------
static void runtime_topology_change_redecomposes() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};

  // Graph #1: acyclic chain A -> B.
  const core::PatchEdge edgesAcyclic[] = {{A_out, B_in}};
  core::CompileResult res1 =
      core::compile_graph(jacks, 4, edgesAcyclic, 1, mods, 2);
  CHECK_TRUE(res1.status == core::CompileStatus::ok);
  CHECK_EQ(res1.graph.regions.size(), 2u);
  bool anyCyclic1 = false;
  for (const auto& r : res1.graph.regions)
    if (r.kind == core::RegionKind::cyclic) anyCyclic1 = true;
  CHECK_FALSE(anyCyclic1);

  // Graph #2: adding B -> A closes the loop. Recompile (the call the audio thread
  // makes when the epoch changes) must now yield a cyclic SCC.
  const core::PatchEdge edgesCyclic[] = {{A_out, B_in}, {B_out, A_in}};
  core::CompileResult res2 =
      core::compile_graph(jacks, 4, edgesCyclic, 2, mods, 2);
  CHECK_TRUE(res2.status == core::CompileStatus::ok);
  CHECK_TRUE(res2.graph.regions.size() >= 1u);
  CHECK_TRUE(res2.graph.regions[0].kind == core::RegionKind::cyclic);
  CHECK_FALSE(plans_bit_identical(res1.graph, res2.graph));  // plan actually changed
}

// ----------------------------------------------------------------------------
// addition ⑧: recompile must NOT run on the audio thread.
// ----------------------------------------------------------------------------
static void recompile_not_on_audio_thread() {
  const core::JackDescriptor jacks[] = {
      mk_jack(A_out, kA, core::PinDirection::output),
      mk_jack(A_in, kA, core::PinDirection::input),
      mk_jack(B_out, kB, core::PinDirection::output),
      mk_jack(B_in, kB, core::PinDirection::input),
  };
  ContractBuilder ba; ba.path(A_in, A_out, 0.0, true);
  ContractBuilder bb; bb.path(B_in, B_out, 0.0, true);
  const core::GraphModule mods[] = {mod(kA, &ba.c), mod(kB, &bb.c)};
  const core::PatchEdge edges[] = {{A_out, B_in}, {B_out, A_in}};

  // Positive control: a control-thread recompile allocates nothing in an RT
  // window, so the heap detector stays at 0.
  rt::reset();
  {
    core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
    CHECK_TRUE(res.status == core::CompileStatus::ok);
  }
  CHECK_EQ(rt::g_heap_in_rt.load(), 0L);

  // Negative control: trigger the recompile inside an RT window. It allocates
  // (vectors / CompiledGraph), and the operator-new override must flag it red.
  rt::reset();
  {
    rt::RtGuard guard;  // models the audio callback
    core::CompileResult res = core::compile_graph(jacks, 4, edges, 2, mods, 2);
    CHECK_TRUE(res.status == core::CompileStatus::ok);
  }
  CHECK_TRUE(rt::g_heap_in_rt.load() > 0L);  // recompile in RT is DETECTED (red)
}

// ----------------------------------------------------------------------------
// GH#7: a cycle whose break edge is reachable through BOTH a delayed and a
// parallel direct input is ALGEBRAIC, so the break edge must be z^-1. The old
// compiler only consulted the single DFS entrance edge (the delayed input) and
// wrongly credited a real 3-sample path. Note: module B carries the "sum" and
// "half" transfer semantics only for the mini interpreter; the compiler decision
// here is purely contract-driven, so reusing module ids {1}/{2} is fine.
// ----------------------------------------------------------------------------
static void parallel_direct_path_gets_z_inverse() {
  constexpr core::ModuleId mA{1}, mB{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{103}, mA, core::PinDirection::output),  // A_out1 -> B delayed
      mk_jack(core::JackId{104}, mA, core::PinDirection::output),  // A_out2 -> B direct
      mk_jack(core::JackId{105}, mA, core::PinDirection::input),   // A_in  <- B_out
      mk_jack(core::JackId{203}, mB, core::PinDirection::input),   // B delayed
      mk_jack(core::JackId{204}, mB, core::PinDirection::input),   // B direct
      mk_jack(core::JackId{205}, mB, core::PinDirection::output),  // B_out
  };
  // A: passes both outputs through direct. B: delayed path is 3 samples, NOT
  // direct-through; the parallel direct path IS direct-through. The B->A break
  // edge must be z^-1 because the direct branch keeps the cycle algebraic.
  ContractBuilder ba;
  ba.path(core::JackId{105}, core::JackId{103}, 0.0, true);
  ba.path(core::JackId{105}, core::JackId{104}, 0.0, true);
  ContractBuilder bb;
  bb.path(core::JackId{203}, core::JackId{205}, 3.0, false);
  bb.path(core::JackId{204}, core::JackId{205}, 0.0, true);  // parallel DIRECT -> algebraic
  const core::GraphModule mods[] = {mod(mA, &ba.c), mod(mB, &bb.c)};
  const core::PatchEdge edges[] = {
      {core::JackId{103}, core::JackId{203}},  // A_out1 -> B delayed
      {core::JackId{104}, core::JackId{204}},  // A_out2 -> B direct
      {core::JackId{205}, core::JackId{105}},  // B_out  -> A_in (closes the loop)
  };

  core::CompileResult res =
      core::compile_graph(jacks, 6, edges, 3, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);

  const core::CompiledRegion* cyc = nullptr;
  for (const auto& r : res.graph.regions)
    if (r.kind == core::RegionKind::cyclic) { cyc = &r; break; }
  CHECK_TRUE(cyc != nullptr);
  CHECK_EQ(cyc->feedback.size(), 1u);
  const auto& fe = cyc->feedback[0];
  CHECK_TRUE(fe.sourceJack == core::JackId{205});
  CHECK_TRUE(fe.sinkJack == core::JackId{105});
  // MUST be z^-1: the reachable parallel direct input makes the minimum cycle
  // delay zero, so design/07 §4 forbids crediting the delayed-only real path.
  CHECK_TRUE(fe.delay == core::FeedbackDelay::z_inverse);
  CHECK_EQ(fe.delaySamples, 1.0);
}

// ----------------------------------------------------------------------------
// GH#7: compile_graph() must enforce module_contract_is_valid() as a real
// admission gate. A present-but-malformed contract (a canDirectThrough path
// carrying a >0 min delay — which the validator must reject) is rejected with a
// fixed invalid_module_contract status and an empty graph, never silently
// admitted just because the module sits in an acyclic region.
// ----------------------------------------------------------------------------
static void malformed_contract_rejected_at_compile_entry() {
  constexpr core::ModuleId mA{1}, mB{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{103}, mA, core::PinDirection::output),  // A_out
      mk_jack(core::JackId{105}, mA, core::PinDirection::input),   // A_in
      mk_jack(core::JackId{203}, mB, core::PinDirection::input),   // B_in
      mk_jack(core::JackId{205}, mB, core::PinDirection::output),  // B_out
  };
  ContractBuilder ba; ba.path(core::JackId{105}, core::JackId{103}, 0.0, true);
  // Contradiction: cannotDirectThrough && minDelay>0 AND canDirectThrough —
  // module_contract_is_valid rejects this path.
  ContractBuilder bb; bb.path(core::JackId{203}, core::JackId{205}, 1.0, true);
  const core::GraphModule mods[] = {mod(mA, &ba.c), mod(mB, &bb.c)};
  const core::PatchEdge edges[] = {{core::JackId{103}, core::JackId{203}}};  // A -> B, acyclic

  core::CompileResult res =
      core::compile_graph(jacks, 4, edges, 1, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::invalid_module_contract);
  CHECK_EQ(res.graph.moduleCount, 0u);
  CHECK_EQ(res.graph.regions.size(), 0u);
}

// ----------------------------------------------------------------------------
// GH#14: independent executable-order verifier. region.modules must be a valid
// topological order of the region's NON-selected-feedback module dependency edges
// — i.e. for every pluggable region edge that is NOT a selected feedback (break)
// edge, the source module must appear at a strictly earlier position than the sink
// module. Selected feedback edges are deliberately EXCLUDED (they are the
// cycle-break edges, which run BACKWARD in the chosen execution order). This does
// NOT re-run the compiler's own topological algorithm; it only checks the reported
// order is consistent with the dependency facts, so it cannot self-prove a buggy
// compiler that happens to sort the same way.
// ----------------------------------------------------------------------------
static bool executable_order_is_topological(const core::CompiledRegion& r,
                                            const core::JackDescriptor* jacks,
                                            std::uint32_t jackCount) {
  auto posOf = [&](core::ModuleId m) -> int {
    for (std::size_t i = 0; i < r.modules.size(); ++i)
      if (r.modules[i] == m) return static_cast<int>(i);
    return -1;
  };
  auto isFeedback = [&](core::JackId s, core::JackId t) -> bool {
    for (const auto& f : r.feedback)
      if (f.sourceJack == s && f.sinkJack == t) return true;
    return false;
  };
  auto jackModule = [&](core::JackId j) -> core::ModuleId {
    for (std::uint32_t i = 0; i < jackCount; ++i)
      if (jacks[i].id == j) return jacks[i].module;
    return core::ModuleId{0};
  };
  for (const auto& e : r.edges) {
    if (isFeedback(e.sourceJack, e.sinkJack)) continue;  // break edge excluded
    const core::ModuleId sm = jackModule(e.sourceJack);
    const core::ModuleId tm = jackModule(e.sinkJack);
    const int sp = posOf(sm), tp = posOf(tm);
    if (sp < 0 || tp < 0) return false;  // both end modules must be in the region
    if (!(sp < tp)) return false;        // source must precede sink
  }
  return true;
}

// ----------------------------------------------------------------------------
// GH#14 tie-break independent reference (NOT the compiler's algorithm): exhaustively
// enumerate every valid topological order of the region's non-feedback pluggable
// dependency edges and return the LEXICOGRAPHICALLY-smallest one by ModuleId (the
// design/07 + GH#14 rule: among the currently-ready nodes, pick the smallest
// ModuleId). count receives the number of distinct valid orders, so a caller can
// assert a genuine tie (>=2). Because this is a brute-force reference it cannot
// self-prove a compiler that happens to share the same greedy rule.
// ----------------------------------------------------------------------------
static std::vector<core::ModuleId> min_topological_order(
    const core::CompiledRegion& r, const core::JackDescriptor* jacks,
    std::uint32_t jackCount, std::uint32_t& count) {
  count = 0;
  std::vector<core::ModuleId> ids = r.modules;
  std::sort(ids.begin(), ids.end(),
            [](core::ModuleId a, core::ModuleId b) { return num(a) < num(b); });
  const std::size_t n = ids.size();
  auto indexOf = [&](core::ModuleId m) -> int {
    for (std::size_t i = 0; i < n; ++i)
      if (ids[i] == m) return static_cast<int>(i);
    return -1;
  };
  auto isFeedback = [&](core::JackId s, core::JackId t) {
    for (const auto& f : r.feedback)
      if (f.sourceJack == s && f.sinkJack == t) return true;
    return false;
  };
  auto jackModIdx = [&](core::JackId j) -> int {
    core::ModuleId m{0};
    for (std::uint32_t i = 0; i < jackCount; ++i)
      if (jacks[i].id == j) m = jacks[i].module;
    return indexOf(m);
  };
  std::vector<std::vector<std::size_t>> suc(n);
  std::vector<std::uint32_t> indeg(n, 0);
  std::vector<char> selfDep(n, 0);
  for (const auto& e : r.edges) {
    if (isFeedback(e.sourceJack, e.sinkJack)) continue;
    const int s = jackModIdx(e.sourceJack), t = jackModIdx(e.sinkJack);
    if (s < 0 || t < 0) return {};
    if (s == t) {
      selfDep[static_cast<std::size_t>(s)] = 1;  // non-feedback self dep => a cycle
      continue;
    }
    suc[static_cast<std::size_t>(s)].push_back(static_cast<std::size_t>(t));
    ++indeg[static_cast<std::size_t>(t)];
  }
  for (std::size_t i = 0; i < n; ++i)
    if (selfDep[i]) return {};  // a non-feedback self dependency admits no DAG order
  // Collapse duplicate forward edges (same pair, redundant constraint) so indeg
  // counts match the logical dependency set the enumerator walks.
  for (auto& a : suc) {
    std::sort(a.begin(), a.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
  }

  std::vector<core::ModuleId> best;
  std::vector<std::size_t> cur;
  std::vector<char> used(n, 0);
  std::function<void()> rec = [&]() {
    if (cur.size() == n) {
      ++count;
      std::vector<core::ModuleId> cand;
      cand.reserve(n);
      for (std::size_t i : cur) cand.push_back(ids[i]);
      if (best.empty()) {
        best = cand;
      } else {
        bool less = false;
        for (std::size_t i = 0; i < n; ++i) {
          if (cand[i] != best[i]) {
            less = num(cand[i]) < num(best[i]);
            break;
          }
        }
        if (less) best = cand;
      }
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (used[i] || indeg[i] != 0) continue;
      used[i] = 1;
      cur.push_back(i);
      for (std::size_t w : suc[i]) --indeg[w];
      rec();
      for (std::size_t w : suc[i]) ++indeg[w];
      cur.pop_back();
      used[i] = 0;
    }
  };
  rec();
  return best;
}

// ----------------------------------------------------------------------------
// GH#14: cyclic SCC execution order. The compiler must report region.modules in
// the executable DAG order AFTER the selected feedback edge(s) are removed — NOT
// the numeric-ascending ModuleId order the old implementation returned.
//
// Counterexample: three distinct module ids, cycle 0 -> 2 -> 1 -> 0. The selected
// (break) edge is 1 -> 0; removing it leaves the feed-forward DAG 0 -> 2 -> 1, so
// region.modules MUST be {0, 2, 1}. The old numeric sort returned {0, 1, 2} — the
// bug GH#14 records (this makes the test RED on the old implementation).
// ----------------------------------------------------------------------------
static void cyclic_execution_order_counterexample() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);

  // All three legal/prepared/cycle-allowed, each direct-through on its own path.
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c), mod(m2, &b2.c)};

  // cycle 0 -> 2 -> 1 -> 0 (the 1 -> 0 edge is the loop-closing break candidate).
  const core::PatchEdge edges[] = {
      {core::JackId{101}, core::JackId{122}},  // 0 -> 2
      {core::JackId{121}, core::JackId{112}},  // 2 -> 1
      {core::JackId{111}, core::JackId{102}},  // 1 -> 0
  };

  core::CompileResult res = core::compile_graph(jacks, kJackCount, edges, 3, mods, 3);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 3u);

  // The single selected break edge must be the 1 -> 0 pluggable edge.
  CHECK_EQ(reg.feedback.size(), 1u);
  CHECK_TRUE(reg.feedback[0].sourceJack == core::JackId{111});  // module 1 out
  CHECK_TRUE(reg.feedback[0].sinkJack == core::JackId{102});    // module 0 in

  // Executable order is the feed-forward DAG after removing that break edge.
  const core::ModuleId expected[] = {m0, m2, m1};
  bool orderOk = true;
  for (std::int32_t i = 0; i < 3; ++i)
    if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
  CHECK_TRUE(orderOk);

  // Independent topological consistency (does NOT copy the compiler algorithm).
  CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
}

// ----------------------------------------------------------------------------
// GH#14: tie-break among multiple legal topo orders. The diamond 0->1,0->2,1->3,
// 2->3,3->0 has TWO valid feed-forward orders once 3->0 is broken ([0,1,2,3] and
// [0,2,1,3]); the compiler must deterministically pick the one that resolves each
// ready set by smallest ModuleId -> [0,1,2,3]. Verified against an independent
// brute-force minimiser (NOT the compiler's greedy rule).
// ----------------------------------------------------------------------------
static void cyclic_execution_order_tiebreak() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2}, m3{3};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
      mk_jack(core::JackId{131}, m3, core::PinDirection::output),
      mk_jack(core::JackId{132}, m3, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  ContractBuilder b3; b3.path(core::JackId{132}, core::JackId{131}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c),
                                    mod(m2, &b2.c), mod(m3, &b3.c)};
  const core::PatchEdge edges[] = {{core::JackId{101}, core::JackId{112}},
                                   {core::JackId{101}, core::JackId{122}},
                                   {core::JackId{111}, core::JackId{132}},
                                   {core::JackId{121}, core::JackId{132}},
                                   {core::JackId{131}, core::JackId{102}}};
  core::CompileResult res = core::compile_graph(jacks, kJackCount, edges, 5, mods, 4);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 4u);
  CHECK_EQ(reg.feedback.size(), 1u);
  CHECK_TRUE(reg.feedback[0].sourceJack == core::JackId{131});
  CHECK_TRUE(reg.feedback[0].sinkJack == core::JackId{102});

  // The compiler picks min-ModuleId tie-break: [0,1,2,3], not [0,2,1,3].
  const core::ModuleId expected[] = {m0, m1, m2, m3};
  bool orderOk = true;
  for (std::int32_t i = 0; i < 4; ++i)
    if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
  CHECK_TRUE(orderOk);

  // Independent brute-force minimiser agrees, AND there is a genuine tie (>=2
  // valid orders) so this really exercises the tie-break rule.
  std::uint32_t count = 0;
  const std::vector<core::ModuleId> minOrder =
      min_topological_order(reg, jacks, kJackCount, count);
  CHECK_TRUE(count >= 2u);
  CHECK_TRUE(minOrder.size() == 4u);
  bool minMatches = true;
  for (std::size_t i = 0; i < 4; ++i)
    if (minOrder[i] != expected[i]) minMatches = false;
  CHECK_TRUE(minMatches);

  CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
}

// ----------------------------------------------------------------------------
// GH#14 criterion ⑥c extension: the executable order (not just the feedback set)
// is independent of patch-edge insertion order. Every permutation of the
// counterexample cycle yields modules {0,2,1} and feedback {1->0}.
// ----------------------------------------------------------------------------
static void cyclic_execution_order_insertion_independent() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c), mod(m2, &b2.c)};
  const core::PatchEdge base[] = {{core::JackId{101}, core::JackId{122}},  // 0 -> 2
                                  {core::JackId{121}, core::JackId{112}},  // 2 -> 1
                                  {core::JackId{111}, core::JackId{102}}}; // 1 -> 0
  const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                          {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const auto& p : perm) {
    const core::PatchEdge es[3] = {base[p[0]], base[p[1]], base[p[2]]};
    core::CompileResult res = core::compile_graph(jacks, kJackCount, es, 3, mods, 3);
    CHECK_TRUE(res.status == core::CompileStatus::ok);
    CHECK_EQ(res.graph.regions.size(), 1u);
    const auto& reg = res.graph.regions[0];
    CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
    CHECK_EQ(reg.modules.size(), 3u);
    const core::ModuleId expected[] = {m0, m2, m1};
    bool orderOk = true;
    for (std::int32_t i = 0; i < 3; ++i)
      if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
    CHECK_TRUE(orderOk);
    CHECK_EQ(reg.feedback.size(), 1u);
    CHECK_TRUE(reg.feedback[0].sourceJack == core::JackId{111});
    CHECK_TRUE(reg.feedback[0].sinkJack == core::JackId{102});
    CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
  }
}

// ----------------------------------------------------------------------------
// GH#14: a self-loop is a selected feedback edge and must be excluded from the
// forward dependency set, even when its module also participates in a cross-cycle.
// Module 0 self-loops (101->110) and forms a 2-module SCC with module 1
// (0->1, 1->0). Both the self-loop and the 1->0 cross back-edge are feedback; the
// only remaining forward edge is 0->1, so the executable order is [0,1].
// ----------------------------------------------------------------------------
static void cyclic_execution_order_self_loop_cross() {
  constexpr core::ModuleId m0{0}, m1{1};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{110}, m0, core::PinDirection::input),  // self-loop sink
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0;
  b0.path(core::JackId{110}, core::JackId{101}, 0.0, true);  // self-loop path
  b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);  // cross-in path
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c)};
  const core::PatchEdge edges[] = {{core::JackId{101}, core::JackId{110}},  // 0 self
                                   {core::JackId{101}, core::JackId{112}},  // 0 -> 1
                                   {core::JackId{111}, core::JackId{102}}}; // 1 -> 0
  core::CompileResult res = core::compile_graph(jacks, kJackCount, edges, 3, mods, 2);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 2u);
  const core::ModuleId expected[] = {m0, m1};
  bool orderOk = true;
  for (std::int32_t i = 0; i < 2; ++i)
    if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
  CHECK_TRUE(orderOk);

  // Both the self-loop AND the cross back-edge are selected as z^-1 feedback.
  CHECK_EQ(reg.feedback.size(), 2u);
  bool sawSelfLoop = false, sawCrossBack = false;
  for (const auto& f : reg.feedback) {
    CHECK_TRUE(f.delay == core::FeedbackDelay::z_inverse);
    if (f.sourceJack == core::JackId{101} && f.sinkJack == core::JackId{110})
      sawSelfLoop = true;
    if (f.sourceJack == core::JackId{111} && f.sinkJack == core::JackId{102})
      sawCrossBack = true;
  }
  CHECK_TRUE(sawSelfLoop);
  CHECK_TRUE(sawCrossBack);

  // A self-loop left in the forward dependency set would violate source<pos<sink
  // for the same module, so this verifier catches it.
  CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
  CHECK_TRUE(feedback_set_is_sufficient(reg, jacks, kJackCount, kMaxTestId));
  CHECK_TRUE(feedback_set_non_redundant(reg, jacks, kJackCount, kMaxTestId));
}

// ----------------------------------------------------------------------------
// GH#14: a fixed (cat-1) internal edge is NOT selectable as feedback and is NEVER
// dropped from the forward dependency set. Adding the fixed edge 2->1 to the
// diamond constrains the topo order (from [0,1,2,3] to [0,2,1,3]) while the
// feedback stays on the pluggable 3->0 edge (no JackId{0} sentinel enters
// region.feedback).
// ----------------------------------------------------------------------------
static void cyclic_execution_order_mixed_fixed_pluggable() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2}, m3{3};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
      mk_jack(core::JackId{131}, m3, core::PinDirection::output),
      mk_jack(core::JackId{132}, m3, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  ContractBuilder b3; b3.path(core::JackId{132}, core::JackId{131}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c),
                                    mod(m2, &b2.c), mod(m3, &b3.c)};
  const core::PatchEdge edges[] = {{core::JackId{101}, core::JackId{112}},
                                   {core::JackId{101}, core::JackId{122}},
                                   {core::JackId{111}, core::JackId{132}},
                                   {core::JackId{121}, core::JackId{132}},
                                   {core::JackId{131}, core::JackId{102}}};
  const core::FixedEdge fe[] = {{m2, m1, "fixed.demo_2_to_1"}};
  core::CompileResult res =
      core::compile_graph(jacks, kJackCount, edges, 5, mods, 4, fe, 1);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 4u);

  // The fixed 2->1 dependency forces 2 to be scheduled before 1 -> [0,2,1,3],
  // NOT the no-fixed tie-break [0,1,2,3]. A compiler that ignored the fixed edge
  // in its topo sort would return [0,1,2,3] here (see negative ②).
  const core::ModuleId expected[] = {m0, m2, m1, m3};
  bool orderOk = true;
  for (std::int32_t i = 0; i < 4; ++i)
    if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
  CHECK_TRUE(orderOk);

  // Feedback is ONLY the pluggable 3->0 edge; the fixed edge's JackId{0}
  // sentinel must never appear as a selected feedback source/sink.
  CHECK_EQ(reg.feedback.size(), 1u);
  CHECK_TRUE(reg.feedback[0].sourceJack == core::JackId{131});
  CHECK_TRUE(reg.feedback[0].sinkJack == core::JackId{102});
  for (const auto& f : reg.feedback) {
    CHECK_FALSE(f.sourceJack == core::JackId{0});
    CHECK_FALSE(f.sinkJack == core::JackId{0});
  }
  CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
}

// ----------------------------------------------------------------------------
// GH#14 defensive fail-closed: if removing the selected feedback edges still
// leaves a cycle (e.g. a fixed edge completes the loop and is not selectable as
// feedback), the compiler MUST NOT emit a partial/incorrect order. It returns
// invalid_execution_order with an empty graph instead of an infinite loop or a
// wrong region.modules.
// ----------------------------------------------------------------------------
static void cyclic_execution_order_fail_closed() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c), mod(m2, &b2.c)};
  // Pluggable cycle 0->2, 2->1, 1->0 breaks on the 1->0 cable, but the FIXED edge
  // 1->0 also exists and is never removed, so a directed cycle survives.
  const core::PatchEdge edges[] = {{core::JackId{101}, core::JackId{122}},
                                   {core::JackId{121}, core::JackId{112}},
                                   {core::JackId{111}, core::JackId{102}}};
  const core::FixedEdge fe[] = {{m1, m0, "fixed.demo_1_to_0"}};
  core::CompileResult res =
      core::compile_graph(jacks, kJackCount, edges, 3, mods, 3, fe, 1);
  CHECK_TRUE(res.status == core::CompileStatus::invalid_execution_order);
  CHECK_EQ(res.graph.moduleCount, 0u);
  CHECK_EQ(res.graph.regions.size(), 0u);
}

// ----------------------------------------------------------------------------
// GH#14 parallel-path preservation: a module pair {1,2} may carry BOTH a
// selected feedback cable (1->2, a back edge) and an independent forward cable
// (2->1). When the compiler removes the selected feedback edge it must drop ONLY
// that cable -- the forward 2->1 stays a real dependency and forces 2 before 1.
// This is the "parallel path" detector: a buggy fix that dropped every
// same-module-pair edge (the undirected interpretation of "same module pair")
// would remove 2->1 too and return {0,1,2} (negative ③).
// ----------------------------------------------------------------------------
static void cyclic_execution_order_parallel_path() {
  constexpr core::ModuleId m0{0}, m1{1}, m2{2};
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{101}, m0, core::PinDirection::output),
      mk_jack(core::JackId{102}, m0, core::PinDirection::input),
      mk_jack(core::JackId{111}, m1, core::PinDirection::output),
      mk_jack(core::JackId{112}, m1, core::PinDirection::input),
      mk_jack(core::JackId{121}, m2, core::PinDirection::output),
      mk_jack(core::JackId{122}, m2, core::PinDirection::input),
  };
  const std::uint32_t kJackCount = sizeof(jacks) / sizeof(jacks[0]);
  ContractBuilder b0; b0.path(core::JackId{102}, core::JackId{101}, 0.0, true);
  ContractBuilder b1; b1.path(core::JackId{112}, core::JackId{111}, 0.0, true);
  ContractBuilder b2; b2.path(core::JackId{122}, core::JackId{121}, 0.0, true);
  const core::GraphModule mods[] = {mod(m0, &b0.c), mod(m1, &b1.c), mod(m2, &b2.c)};
  // Cycle 0->2->1->0 with module pair {1,2} carrying BOTH a back cable 1->2 and a
  // forward cable 2->1. Removing the feedback edges (1->0, 1->2) leaves 0->2->1.
  const core::PatchEdge edges[] = {{core::JackId{101}, core::JackId{122}},
                                   {core::JackId{121}, core::JackId{112}},
                                   {core::JackId{111}, core::JackId{102}},
                                   {core::JackId{111}, core::JackId{122}}};
  core::CompileResult res =
      core::compile_graph(jacks, kJackCount, edges, 4, mods, 3);
  CHECK_TRUE(res.status == core::CompileStatus::ok);
  CHECK_EQ(res.graph.regions.size(), 1u);
  const auto& reg = res.graph.regions[0];
  CHECK_TRUE(reg.kind == core::RegionKind::cyclic);
  CHECK_EQ(reg.modules.size(), 3u);

  // The forward 2->1 is preserved: 2 schedules before 1 -> {0,2,1}. A buggy fix
  // that removed every same-module-pair edge would return {0,1,2} (negative ③).
  const core::ModuleId expected[] = {m0, m2, m1};
  bool orderOk = true;
  for (std::int32_t i = 0; i < 3; ++i)
    if (reg.modules[static_cast<std::size_t>(i)] != expected[i]) orderOk = false;
  CHECK_TRUE(orderOk);

  // Both back cables 1->0 and 1->2 are selected feedback; the forward 2->1 is not.
  CHECK_EQ(reg.feedback.size(), 2u);
  CHECK_TRUE(executable_order_is_topological(reg, jacks, kJackCount));
}

int main() {
  z_inverse_is_one_sample_not_one_block();
  acyclic_not_misjudged_and_cyclic_not_missed();
  feedback_set_deterministic_across_order();
  module_latency_grants_no_break_eligibility();
  real_path_delay_not_waived_to_z_inverse();
  cycle_unsafe_module_rejected();
  feedback_set_sufficient_and_non_redundant();
  self_loop_compiles();
  multiple_independent_sccs();
  runtime_topology_change_redecomposes();
  recompile_not_on_audio_thread();
  parallel_direct_path_gets_z_inverse();
  malformed_contract_rejected_at_compile_entry();
  cyclic_execution_order_counterexample();
  cyclic_execution_order_tiebreak();
  cyclic_execution_order_insertion_independent();
  cyclic_execution_order_self_loop_cross();
  cyclic_execution_order_mixed_fixed_pluggable();
  cyclic_execution_order_fail_closed();
  cyclic_execution_order_parallel_path();
  return ::test::finish("graph_compiler");
}
