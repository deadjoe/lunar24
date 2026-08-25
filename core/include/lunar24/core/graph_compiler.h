// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GraphCompiler (design/07 §4 "Feedback 的确定性规则"): turns a PatchGraph
// connection-facts snapshot into an immutable execution plan. This is the P2-③
// SCC cycle-breaking deliverable.
//
// The compiler DEPENDS on the connection-facts layer (patch_graph.h produces the
// canonical PatchEdge[] set) and on the per-module scheduling contract
// (module_execution_contract.h declares the path-level causal facts). It never
// owns a PatchGraph and the PatchGraph never needs a complete CompiledGraph.
//
// What it produces:
//   * SCC decomposition of the inter-module signal graph, computed only when the
//     topology changes (PatchGraph.epoch is the trigger). Acyclic regions run as
//     a block; a cyclic region runs per-sample (design/07 §4 line 103-111).
//   * A DETERMINISTIC feedback-edge set per cyclic region — the back edges of a
//     DFS that orders nodes by ModuleId and adjacency by (sourceJack, sinkJack),
//     so the same topology always yields the same set, independent of plug order
//     (design/07 §4: "恢复同一 DeviceState 必须得到同一结果"). The selected set
//     is both SUFFICIENT (removing it makes the region a DAG) and NON-REDUNDANT
//     (re-adding any single selected edge recreates a cycle).
//   * A delay decision per feedback edge, consulting ONLY the path-level contract
//     (ModulePathDelay with a specific inPort->outPort). A feedback edge whose
//     specific cycle path has minCausalDelaySamples>=1 and !canDirectThrough is
//     marked real_path and gets NO extra z^-1. Otherwise it is z_inverse (exactly
//     one one-sample delay). The module-level intrinsicLatencySamples is
//     deliberately NOT consulted (it is scheduling info, not path evidence).
//   * Rejection (cycle_unsafe_module) when a cyclic SCC contains a module whose
//     contract says allowedInCyclicSCC==false (window / FFT / reverse modules
//     until they expose a cycle-safe interface) — never silently turned into a
//     block delay.
//
// The compiler allocates (std::vector) and MUST NOT run on the audio thread; the
// audio thread only holds a non-owning handle/epoch and the plan is reclaimed
// off-a-non-audio-thread (design/07 §5). The RtGuard detector in the test suite
// proves a recompile inside an RT window would be caught.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <lunar24/core/descriptors.h>
#include <lunar24/core/id_types.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/patch_graph.h>

namespace lunar24::core {

// One module and its prepared scheduling contract (nullptr = unknown: treated as
// not cycle-safe and as having no declared path delay). Supplied by the caller
// (the control/build side) from each module's real prepared ModuleExecutionContract.
struct GraphModule {
  ModuleId id;
  const ModuleExecutionContract* contract;
};

enum class RegionKind : std::uint8_t { acyclic, cyclic };

// How a selected feedback edge is broken.
enum class FeedbackDelay : std::uint8_t {
  z_inverse,  // no real path delay -> insert exactly one z^-1 one-sample delay
  real_path,  // the specific cycle path already has >=1 sample causal delay
};

// A signal edge inside a compiled cyclic region.
struct CompiledEdge {
  JackId sourceJack;
  JackId sinkJack;
};

// A feedback edge the compiler selected to break a cycle in a cyclic region.
struct CompiledFeedbackEdge {
  JackId sourceJack;
  JackId sinkJack;
  FeedbackDelay delay;
  double delaySamples;  // 1.0 for z_inverse; real min-causal-delay for real_path
};

// One execution region. An acyclic region is a single module run at block rate.
// A cyclic region is run per-sample, and carries its internal edges + the chosen
// feedback set (so the tests can re-verify sufficiency / non-redundancy of the
// break set independently of the algorithm that chose it).
struct CompiledRegion {
  RegionKind kind;
  std::vector<ModuleId> modules;                 // deterministic execution order
  std::vector<CompiledEdge> edges;               // cyclic region only
  std::vector<CompiledFeedbackEdge> feedback;    // cyclic region only
};

// --- Executor consume-rule for real_path feedback (design/07 §4, P3-⑥ Debt 2) ---
//
// `FeedbackDelay::real_path` on an edge with `delaySamples==D` means the specific
// on-cycle path already carries D samples of genuine causal delay; the compiler
// grants it NO extra z^-1. An EXECUTOR consuming the plan MUST honor that declared
// delay by realizing exactly D samples of latency on the feedback return path.
//
// RULE: for a real_path feedback edge the executor must read the value it delivers
// to the consuming module from a PER-EDGE delay line of depth D — the line that
// holds the loop-forward values traversing that edge — and hand the sending module
// the value from D samples ago. It must NOT read the source module's "last written
// output" buffer. Reading the source's last output is off-by-one for any D>1: the
// source ran after the consumer in the per-sample pass, so the consumer reads the
// previous pass's value (loop delay D+1, not D). Reading the last output is exactly
// how the P2-③ MiniExec test scaffold consumes real_path, and it is wrong; it is
// test scaffolding, not part of this contract.
//
// The judge has two halves, and partition invariance is NECESSARY but not SUFFICIENT:
//   * Partition invariance — the mixed non-uniform partition (64,100,37,128,7,256,91)
//     must reproduce the sequential per-sample sequence (design/07 §4 line 107). This
//     fires only on a one-buffer / block-lazy break.
//   * An ABSOLUTE reference — the realized loop delay must equal `delaySamples`
//     exactly, not merely be "the same under every partition". A wrong-but-consistent
//     executor (off-by-one delay, or a spuriously waived z^-1) passes invariance and
//     is caught only by the absolute anchor.
// tests/core/test_executor_realpath.cpp encodes this as the correct executor plus two
// negatives (block-lazy partition-variant; moduleOut off-by-one with a reference
// compare that fires at n=delaySamples).

// Immutable execution plan. The audio thread holds a non-owning handle/epoch to
// this; a new plan is built off a non-audio thread and published lock-free, and
// an old plan is reclaimed off the audio thread (design/07 §5).
struct CompiledGraph {
  std::vector<CompiledRegion> regions;           // in condensation-DAG execution order
  std::uint32_t moduleCount = 0;                 // number of distinct modules compiled
};

enum class CompileStatus : std::uint8_t { ok, cycle_unsafe_module };

struct CompileResult {
  CompileStatus status = CompileStatus::ok;
  CompiledGraph graph;  // meaningful only when status == ok
};

namespace detail {

inline std::uint32_t jid(JackId v) { return static_cast<std::uint32_t>(v); }
inline std::uint32_t mid(ModuleId v) { return static_cast<std::uint32_t>(v); }

inline ModuleId jack_module(const JackDescriptor* jacks, std::uint32_t jackCount, JackId j) {
  for (std::uint32_t i = 0; i < jackCount; ++i)
    if (jacks[i].id == j) return jacks[i].module;
  return ModuleId{0};  // unknown sentinel
}

inline std::uint32_t lower_bound_module(const ModuleId* ids, std::uint32_t n, ModuleId v) {
  std::uint32_t lo = 0, hi = n;
  while (lo < hi) {
    std::uint32_t m = (lo + hi) / 2;
    if (mid(ids[m]) < mid(v))
      lo = m + 1;
    else
      hi = m;
  }
  return lo;
}

// Path-level delay decision for one feedback edge. Only the specific
// (inPort -> outPort) path in a module's declaration carries real-delay credit;
// the module-wide intrinsicLatencySamples is deliberately not consulted.
inline void decide_feedback_delay(const ModuleExecutionContract* c, JackId inPort,
                                  JackId outPort, CompiledFeedbackEdge& fe) {
  fe.delay = FeedbackDelay::z_inverse;
  fe.delaySamples = 1.0;
  if (c == nullptr) return;
  for (std::uint32_t i = 0; i < c->pathDelayCount; ++i) {
    const ModulePathDelay& p = c->pathDelays[i];
    if (p.inPort == inPort && p.outPort == outPort) {
      if (p.minCausalDelaySamples >= 1.0 && !p.canDirectThrough) {
        fe.delay = FeedbackDelay::real_path;
        fe.delaySamples = p.minCausalDelaySamples;
      }
      return;  // found the specific path; decision made
    }
  }
}

}  // namespace detail

// Compile a patch snapshot into an execution plan. `jacks` maps a JackId to its
// owning module; `edges` is the canonical effective-edge set from PatchGraph;
// `modules` supplies each module's prepared scheduling contract.
//
// Returns cycle_unsafe_module (with an empty graph) if any cyclic region includes
// a module that is not allowed in a cyclic SCC. This is a control-thread call;
// it is the caller's responsibility to keep it off the audio thread.
inline CompileResult compile_graph(const JackDescriptor* jacks, std::uint32_t jackCount,
                                   const PatchEdge* edges, std::uint32_t edgeCount,
                                   const GraphModule* modules, std::uint32_t moduleCount) {
  CompileResult result;

  // ---- 0. Distinct modules touched by the graph --------------------------
  std::vector<ModuleId> mods;
  mods.reserve(edgeCount * 2);
  for (std::uint32_t i = 0; i < edgeCount; ++i) {
    mods.push_back(detail::jack_module(jacks, jackCount, edges[i].source));
    mods.push_back(detail::jack_module(jacks, jackCount, edges[i].sink));
  }
  std::sort(mods.begin(), mods.end(), [](ModuleId a, ModuleId b) { return detail::mid(a) < detail::mid(b); });
  mods.erase(std::unique(mods.begin(), mods.end()), mods.end());
  const std::uint32_t M = static_cast<std::uint32_t>(mods.size());
  result.graph.moduleCount = M;
  if (M == 0) return result;

  // Module id -> prepared contract (linear over the (small) supplied list).
  std::vector<const ModuleExecutionContract*> cts(M, nullptr);
  for (std::uint32_t i = 0; i < M; ++i)
    for (std::uint32_t k = 0; k < moduleCount; ++k)
      if (modules[k].id == mods[i]) { cts[i] = modules[k].contract; break; }

  // ---- 1. Inter-module graph --------------------------------------------
  struct AutoEdge { std::uint32_t src, snk; JackId sj, tj; };
  std::vector<AutoEdge> crossed;
  std::vector<std::vector<AutoEdge>> selfLoops(M);
  crossed.reserve(edgeCount);
  for (std::uint32_t i = 0; i < edgeCount; ++i) {
    ModuleId sm = detail::jack_module(jacks, jackCount, edges[i].source);
    ModuleId tm = detail::jack_module(jacks, jackCount, edges[i].sink);
    std::uint32_t si = detail::lower_bound_module(mods.data(), M, sm);
    std::uint32_t ti = detail::lower_bound_module(mods.data(), M, tm);
    AutoEdge e{si, ti, edges[i].source, edges[i].sink};
    if (si == ti)
      selfLoops[si].push_back(e);
    else
      crossed.push_back(e);
  }
  // Adjacency per source node, sorted by (sourceJack, sinkJack) -> deterministic.
  std::vector<std::vector<std::uint32_t>> adj(M);
  for (std::uint32_t i = 0; i < crossed.size(); ++i) adj[crossed[i].src].push_back(i);
  for (auto& a : adj)
    std::sort(a.begin(), a.end(), [&](std::uint32_t x, std::uint32_t y) {
      if (crossed[x].sj != crossed[y].sj) return detail::jid(crossed[x].sj) < detail::jid(crossed[y].sj);
      return detail::jid(crossed[x].tj) < detail::jid(crossed[y].tj);
    });

  // ---- 2. Tarjan SCC (deterministic: node index order + sorted adjacency) --
  std::vector<int> index(M, -1), low(M, 0);
  std::vector<char> onStack(M, 0);
  std::vector<std::uint32_t> stack;
  std::vector<std::vector<std::uint32_t>> sccs;
  int nextIndex = 0;
  std::function<void(std::uint32_t)> dfs = [&](std::uint32_t v) {
    index[v] = low[v] = nextIndex++;
    stack.push_back(v);
    onStack[v] = 1;
    for (std::uint32_t ei : adj[v]) {
      std::uint32_t w = crossed[ei].snk;
      if (index[w] < 0) {
        dfs(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] == index[v]) {
      std::vector<std::uint32_t> comp;
      while (true) {
        std::uint32_t w = stack.back();
        stack.pop_back();
        onStack[w] = 0;
        comp.push_back(w);
        if (w == v) break;
      }
      sccs.push_back(std::move(comp));
    }
  };
  for (std::uint32_t v = 0; v < M; ++v)
    if (index[v] < 0) dfs(v);

  const std::uint32_t C = static_cast<std::uint32_t>(sccs.size());
  std::vector<std::uint32_t> compId(M, 0);
  for (std::uint32_t c = 0; c < C; ++c)
    for (std::uint32_t v : sccs[c]) compId[v] = c;

  // ---- 3. Cyclic classification + not-cycle-safe rejection ----------------
  std::vector<char> cyclic(C, 0);
  bool rejected = false;
  for (std::uint32_t c = 0; c < C; ++c) {
    bool cyc = sccs[c].size() > 1;
    for (std::uint32_t v : sccs[c])
      if (!selfLoops[v].empty()) cyc = true;
    cyclic[c] = cyc;
    if (cyc)
      for (std::uint32_t v : sccs[c])
        if (cts[v] == nullptr || !cts[v]->allowedInCyclicSCC) rejected = true;
  }
  if (rejected) {
    result.status = CompileStatus::cycle_unsafe_module;
    result.graph = CompiledGraph{};
    return result;
  }

  // ---- 4. Condensation DAG + deterministic topological order ---------------
  std::vector<std::vector<std::uint32_t>> cadj(C);
  for (std::uint32_t i = 0; i < crossed.size(); ++i) {
    std::uint32_t a = compId[crossed[i].src];
    std::uint32_t b = compId[crossed[i].snk];
    if (a == b) continue;
    bool dup = false;
    for (std::uint32_t j : cadj[a])
      if (j == b) { dup = true; break; }
    if (!dup) cadj[a].push_back(b);
  }
  for (auto& a : cadj) std::sort(a.begin(), a.end());

  std::vector<std::uint32_t> indeg(C, 0);
  for (std::uint32_t a = 0; a < C; ++a)
    for (std::uint32_t b : cadj[a]) indeg[b]++;
  std::vector<char> used(C, 0);
  std::vector<std::uint32_t> topo;
  for (std::uint32_t _ = 0; _ < C; ++_) {
    int pick = -1;
    for (std::uint32_t c = 0; c < C; ++c)
      if (!used[c] && indeg[c] == 0) { pick = static_cast<int>(c); break; }
    if (pick < 0) break;  // condensation of a DAG always reaches C here
    used[static_cast<std::uint32_t>(pick)] = 1;
    topo.push_back(static_cast<std::uint32_t>(pick));
    for (std::uint32_t b : cadj[static_cast<std::uint32_t>(pick)]) --indeg[b];
  }

  // ---- 5. Build regions ---------------------------------------------------
  for (std::uint32_t c : topo) {
    CompiledRegion region;
    region.modules.clear();
    region.edges.clear();
    region.feedback.clear();
    if (!cyclic[c]) {
      region.kind = RegionKind::acyclic;
      region.modules.push_back(mods[sccs[c][0]]);
    } else {
      region.kind = RegionKind::cyclic;
      std::vector<std::uint32_t> members = sccs[c];
      std::sort(members.begin(), members.end());  // canonical per-sample order
      for (std::uint32_t v : members) region.modules.push_back(mods[v]);

      std::vector<char> inScc(M, 0);
      for (std::uint32_t v : members) inScc[v] = 1;
      std::vector<std::vector<std::uint32_t>> sadj(M);
      for (std::uint32_t i = 0; i < crossed.size(); ++i)
        if (inScc[crossed[i].src] && inScc[crossed[i].snk]) sadj[crossed[i].src].push_back(i);
      for (std::uint32_t v : members)
        std::sort(sadj[v].begin(), sadj[v].end(), [&](std::uint32_t x, std::uint32_t y) {
          if (crossed[x].sj != crossed[y].sj) return detail::jid(crossed[x].sj) < detail::jid(crossed[y].sj);
          return detail::jid(crossed[x].tj) < detail::jid(crossed[y].tj);
        });

      std::vector<int> stIndex(M, -1);
      std::vector<char> onSccStack(M, 0);
      std::vector<std::uint32_t> stk, parentEdge(M, std::numeric_limits<std::uint32_t>::max());
      int dIdx = 0;
      std::function<void(std::uint32_t)> fdfs = [&](std::uint32_t v) {
        stIndex[v] = dIdx++;
        stk.push_back(v);
        onSccStack[v] = 1;
        for (const AutoEdge& sl : selfLoops[v]) {  // self-loop is always a feedback edge
          CompiledFeedbackEdge fe;
          fe.sourceJack = sl.sj;
          fe.sinkJack = sl.tj;
          detail::decide_feedback_delay(cts[v], sl.tj, sl.sj, fe);  // path: in=sink, out=source
          region.feedback.push_back(fe);
        }
        for (std::uint32_t ei : sadj[v]) {
          std::uint32_t w = crossed[ei].snk;
          if (stIndex[w] < 0) {
            parentEdge[w] = ei;
            fdfs(w);
          } else if (onSccStack[w]) {
            CompiledFeedbackEdge fe;
            fe.sourceJack = crossed[ei].sj;
            fe.sinkJack = crossed[ei].tj;
            // Cycle re-enters the SOURCE module v via the tree edge that reached it.
            JackId inPort = crossed[parentEdge[v]].tj;
            detail::decide_feedback_delay(cts[v], inPort, crossed[ei].sj, fe);
            region.feedback.push_back(fe);
          }
        }
        stk.pop_back();
        onSccStack[v] = 0;
      };
      for (std::uint32_t v : members)
        if (stIndex[v] < 0) fdfs(v);

      for (std::uint32_t i = 0; i < crossed.size(); ++i)
        if (inScc[crossed[i].src] && inScc[crossed[i].snk])
          region.edges.push_back(CompiledEdge{crossed[i].sj, crossed[i].tj});
      for (std::uint32_t v : members)
        for (const AutoEdge& sl : selfLoops[v])
          region.edges.push_back(CompiledEdge{sl.sj, sl.tj});
      std::sort(region.edges.begin(), region.edges.end(),
                [](const CompiledEdge& a, const CompiledEdge& b) {
                  if (a.sourceJack != b.sourceJack) return detail::jid(a.sourceJack) < detail::jid(b.sourceJack);
                  return detail::jid(a.sinkJack) < detail::jid(b.sinkJack);
                });
    }
    result.graph.regions.push_back(std::move(region));
  }

  result.status = CompileStatus::ok;
  return result;
}

}  // namespace lunar24::core
