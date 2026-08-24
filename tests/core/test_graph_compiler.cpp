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
  return ::test::finish("graph_compiler");
}
