// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Canonical MachineRuntimeDefinition acceptance (task#65 7C2 GH#11). The product machine
// must OWN its scheduling contracts, module table, fixed internal route set and
// ExecutionKind disposition, and expose a single stable-address runtime() — the fix for
// the "tests hand-bind product fixtures" anti-pattern.
//
// The fixed-route set is a 28-entry DATA TABLE deriving 11 unique inter-module compiler
// edges (mixer->vcf_l/r fold); the normalized routes are keyed by stable RouteId (the
// VCO-B self-edge is the sole active one). Only the actual SCC members (VCO-B, env-
// follower, preamp) carry cycle-safe contracts; default is cycle-UNSAFE. The audit API
// distinguishes "unknown module" (nullopt / nullptr) from "declared deferred" (kUnsupported
// / a real contract).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <type_traits>

#include <lunar24/core/device_state.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/unit_identity_profile.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

// Allocator-count probe (A′ "render-path zero allocation"). Provided by the dedicated
// test_machine_definition_allocator.cpp TU; counted across the whole test binary.
extern std::size_t g_allocCount;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool cond, const char* label) {
  ++g_checks;
  if (!cond) {
    std::printf("FAIL: %s\n", label);
    ++g_fail;
  }
}

namespace core = lunar24::core;

// §4.6 explicit-lifetime helper: construct + configure + rebuild + process a
// MachineRuntimeDefinition in a DEDICATED (noinline) frame, then let it be destroyed there
// (stack churn). Returns only the observed outputs — the definition itself does not escape.
// Calling it repeatedly for the same config proves construction churn introduces no hidden
// state; calling it with a DIFFERENT config (base Hz) proves the helper genuinely varies
// (its output depends on the configured VCO pitch, so the determinism check is non-vacuous).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
core::RuntimeOutput churnSnapshot(double baseHz) {
  constexpr std::uint64_t kS = 0x5EED;
  constexpr double kRate = 48000.0;
  core::MachineRuntimeDefinition d(kS, kRate);
  core::SynthRuntime& rt = d.runtime();
  rt.setVcoBaseHz(baseHz);
  rt.setVcoCvAmounts(1.0, 0.0);
  // item 1 (@Codex eaaf08cc): the canonical builder NO LONGER pins the A/B generic-CV
  // lin/exp mode. Any oracle that drives the generic CV / normalized self-edge must be
  // EXPLICIT here — never silently on the runtime's provisional default.
  rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
  rt.setControlVoltage(lunar24::registry::JackId::vco_b_v_oct_in, 0.0);
  rt.setControlVoltage(lunar24::registry::JackId::vco_a_v_oct_in, 0.0);
  void(rt.rebuild());
  core::RuntimeOutput last{};
  for (int i = 0; i < 16; ++i) last = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
  return last;  // d destroyed at scope exit -> the churn.
}

}  // namespace

int main() {
  namespace reg = lunar24::registry;
  namespace core = lunar24::core;

  std::printf("Canonical MachineRuntimeDefinition acceptance.\n");

  // ---- ownership / non-movable / non-copyable invariants ----------------------
  static_assert(!std::is_copy_constructible<core::MachineRuntimeDefinition>::value,
                "definition is non-copyable (owns stable-address pointers)");
  static_assert(!std::is_move_constructible<core::MachineRuntimeDefinition>::value,
                "definition is non-movable (moving would dangle the runtime's pointers)");
  static_assert(!std::is_copy_assignable<core::MachineRuntimeDefinition>::value,
                "definition is non-copy-assignable");

  // ---- construct at a stable address, build the strict machine ----------------
  constexpr double kSr = 48000.0;
  constexpr std::uint64_t kSeed = 0x5EED;
  core::MachineRuntimeDefinition def(kSeed, kSr);

  check(def.valid(), "definition builds the strict machine (status ok/graph_unchanged)");
  check(def.moduleCount() == core::kModuleCount, "owns exactly the 21-module inventory");

  // ---- fixed-route data table (@Codex correction 1) ---------------------------
  check(def.fixedRouteCount() == 28, "the fixed-route data table enumerates all 28 routes");
  check(def.fixedEdgeCount() == 11, "the 28-route table derives exactly 11 unique FixedEdges");
  check(def.activeRouteCount() == 1, "wires exactly the one active normalized route");

  // Category accounting across the 28 authored routes (independent per-category counts).
  std::uint32_t nInter = 0, nIntra = 0, nHost = 0, nDry = 0, nDeferred = 0;
  for (std::uint32_t i = 0; i < core::kFixedRouteCount; ++i) {
    switch (core::kFixedRoutes[i].category) {
      case core::FixedRouteCategory::kInterModule:   ++nInter;   break;
      case core::FixedRouteCategory::kIntraVcfStage: ++nIntra;   break;
      case core::FixedRouteCategory::kHostTerminal:  ++nHost;    break;
      case core::FixedRouteCategory::kDryTerminal:   ++nDry;     break;
      case core::FixedRouteCategory::kDeferred:      ++nDeferred; break;
    }
  }
  check(nInter == 12, "28-route taxonomy: 12 inter-module");
  check(nIntra == 2, "28-route taxonomy: 2 intra-VCF stage");
  check(nHost == 1, "28-route taxonomy: 1 host-terminal injection");
  check(nDry == 2, "28-route taxonomy: 2 DRY terminal output");
  check(nDeferred == 11, "28-route taxonomy: 11 declared-deferred");
  check(nInter + nIntra + nHost + nDry + nDeferred == 28,
        "the five fixed-route categories close to 28");

  // ---- disposition (point 2): every module has exactly one kind -----------------
  check(def.kindOf(core::ModuleId::vco_a) == core::ExecutionKind::kVcoA,
        "vco_a -> kVcoA");
  check(def.kindOf(core::ModuleId::vco_b) == core::ExecutionKind::kVcoB,
        "vco_b -> kVcoB");
  check(def.kindOf(core::ModuleId::vcf) == core::ExecutionKind::kVcfPath,
        "vcf -> kVcfPath (distortion is intra-vcf)");
  check(def.kindOf(core::ModuleId::mixer) == core::ExecutionKind::kMixer,
        "mixer -> kMixer");
  check(def.kindOf(core::ModuleId::preamp) == core::ExecutionKind::kPreamp,
        "preamp -> kPreamp");
  check(def.kindOf(core::ModuleId::env_follower) == core::ExecutionKind::kEnvFollower,
        "env_follower -> kEnvFollower");
  check(def.kindOf(core::ModuleId::drone_1) == core::ExecutionKind::kDroneBank &&
            def.kindOf(core::ModuleId::drone_6) == core::ExecutionKind::kDroneBank,
        "the six drones are six independent kDroneBank slots (no kind-dedup)");
  // A DEFERRED in-inventory module still HAS a disposition (kUnsupported) — distinct from
  // an unknown id.
  check(def.kindOf(core::ModuleId::keyboard) == core::ExecutionKind::kUnsupported,
        "keyboard is deferred (kUnsupported) this slice");
  // The six control sources are NOW executed (GH#11 FIXED-CANDIDATE): each has a real
  // control kind, not kUnsupported. keyboard/effector/voices remain declared-deferred.
  check(def.kindOf(core::ModuleId::lfo_a) == core::ExecutionKind::kLfo &&
            def.kindOf(core::ModuleId::lfo_b) == core::ExecutionKind::kLfo &&
            def.kindOf(core::ModuleId::envelope_a) == core::ExecutionKind::kEnvelope &&
            def.kindOf(core::ModuleId::envelope_b) == core::ExecutionKind::kEnvelope &&
            def.kindOf(core::ModuleId::joystick) == core::ExecutionKind::kJoystick &&
            def.kindOf(core::ModuleId::sequencer) == core::ExecutionKind::kSequencer,
        "six control sources are executed (kLfo/kEnvelope/kJoystick/kSequencer)");
  // Every one of the 21 modules has a disposition (no unlisted module).
  // item 5 (@Codex eaaf08cc): the old loop iterated the DENSE index 0..kModuleCount-1 and
  // checked "count matches" — that PASSES a duplicate or an omission, so it proves only a
  // lower bound, not a bijection. Rewrite to walk the GENERATED descriptors (kModules[i].id)
  // and prove the 21 ids ↔ disposition table is a true bijection: (i) every generated id
  // resolves to a disposition (no omission from disposition); (ii) every disposition-table id
  // is a real generated id (no phantom module); (iii) the 21 generated ids are pairwise
  // distinct (no duplicate). The same three-way proof is applied to the 6 normalized RouteId
  // and the 28 fixed-route stableId.
  bool everyGeneratedModuleHasKind = true;
  for (std::uint32_t i = 0; i < reg::kModuleCount; ++i)
    if (!def.kindOf(reg::kModules[i].id).has_value()) everyGeneratedModuleHasKind = false;
  check(everyGeneratedModuleHasKind, "item5: every GENERATED module id (kModules[i].id) resolves to a disposition");
  bool dispositionModuleInInventory = true;
  for (std::uint32_t i = 0; i < core::kMachineDispositionCount; ++i) {
    bool inReg = false;
    for (std::uint32_t j = 0; j < reg::kModuleCount; ++j)
      if (reg::kModules[j].id == core::kMachineDisposition[i].id) { inReg = true; break; }
    if (!inReg) dispositionModuleInInventory = false;
  }
  check(dispositionModuleInInventory, "item5: every disposition-table id is a real generated module id (no phantom)");
  bool moduleIdDup = false;
  for (std::uint32_t i = 0; i < reg::kModuleCount; ++i)
    for (std::uint32_t j = i + 1; j < reg::kModuleCount; ++j)
      if (reg::kModules[i].id == reg::kModules[j].id) moduleIdDup = true;
  check(!moduleIdDup, "item5: the 21 generated module ids are pairwise distinct (no duplicate)");
  check(core::kMachineDispositionCount == reg::kModuleCount,
        "item5: disposition count == generated module count (bijection size; a dup or omission would break it)");

  // ---- item 5: 6 normalized RouteId ↔ disposition bijection -------------------------
  bool routeAllDisposed = true;
  for (std::uint32_t i = 0; i < reg::kRouteCount; ++i) {
    bool hasDisp = false;
    for (std::uint32_t j = 0; j < core::kRouteDispositionCount; ++j)
      if (reg::kNormalizedRoutes[i].id == core::kRouteDisposition[j].id) { hasDisp = true; break; }
    if (!hasDisp) routeAllDisposed = false;
  }
  check(routeAllDisposed, "item5: every GENERATED normalized RouteId has a disposition row");
  bool routeDispositionGenerated = true;
  for (std::uint32_t i = 0; i < core::kRouteDispositionCount; ++i) {
    bool inReg = false;
    for (std::uint32_t j = 0; j < reg::kRouteCount; ++j)
      if (reg::kNormalizedRoutes[j].id == core::kRouteDisposition[i].id) { inReg = true; break; }
    if (!inReg) routeDispositionGenerated = false;
  }
  check(routeDispositionGenerated, "item5: every route-disposition id is a real generated RouteId (no phantom)");
  bool routeIdDup = false;
  for (std::uint32_t i = 0; i < reg::kRouteCount; ++i)
    for (std::uint32_t j = i + 1; j < reg::kRouteCount; ++j)
      if (reg::kNormalizedRoutes[i].id == reg::kNormalizedRoutes[j].id) routeIdDup = true;
  check(!routeIdDup, "item5: the 6 normalized RouteIds are pairwise distinct (no duplicate)");
  check(core::kRouteDispositionCount == reg::kRouteCount,
        "item5: route-disposition count == 6 normalized RouteIds (bijection size)");

  // ---- item 5: 28 fixed-route stableId uniqueness + completeness --------------------
  bool fixedStableIdDup = false;
  for (std::uint32_t i = 0; i < core::kFixedRouteCount; ++i)
    for (std::uint32_t j = i + 1; j < core::kFixedRouteCount; ++j)
      if (std::strcmp(core::kFixedRoutes[i].stableId, core::kFixedRoutes[j].stableId) == 0)
        fixedStableIdDup = true;
  check(!fixedStableIdDup, "item5: the 28 fixed-route stableIds are pairwise distinct (no duplicate)");
  bool fixedStableIdComplete = true;
  for (std::uint32_t i = 0; i < core::kFixedRouteCount; ++i)
    if (core::kFixedRoutes[i].stableId[0] == '\0') fixedStableIdComplete = false;
  check(fixedStableIdComplete, "item5: all 28 fixed-route stableIds are non-empty (no omitted id)");

  // ---- item 8 (@Codex eaaf08cc): the normalized-route evidence conflict is OBSERVED, not
  //      silently resolved. The active route's IDS (sourceJack/sinkJack == vco_b.vco_out ->
  //      vco_b.cv_in) are the AUTHORITATIVE execution identity; the registry DESCRIPTION
  //      attributes the source to "VCO A" ("vco_b.vco_out carries the VCO A normalised
  //      signal into vco_b.cv_in"). The slice executes by IDS and keeps the evidence
  //      PROVISIONAL — it does NOT change the generated registry. Pin the IDS-truth + the
  //      provisional status + the observed description mismatch so a future description->
  //      ids swap or a premature confirmation is caught.
  {
    bool activeIdsConsistent = false, activeProvisional = false, activeDescribesA = false;
    for (std::uint32_t i = 0; i < reg::kRouteCount; ++i) {
      if (reg::kNormalizedRoutes[i].id != reg::RouteId::route_vco_b_vco_out_to_cv_in) continue;
      if (reg::kNormalizedRoutes[i].sourceJack == reg::JackId::vco_b_vco_out &&
          reg::kNormalizedRoutes[i].sinkJack == reg::JackId::vco_b_cv_in)
        activeIdsConsistent = true;
      if (reg::kNormalizedRoutes[i].status == reg::EvidenceStatus::provisional) activeProvisional = true;
      if (reg::kNormalizedRoutes[i].description.find("VCO A") != std::string_view::npos) activeDescribesA = true;
    }
    check(activeIdsConsistent,
          "item8: active route IDS = vco_b.vco_out -> vco_b.cv_in (executed by source/sink, not description)");
    check(activeProvisional,
          "item8: active route evidence status is provisional (the conflict is unresolved, not baked confirmed)");
    check(activeDescribesA,
          "item8: active route DESCRIPTION attributes the source to 'VCO A' (the observed IDS-vs-description conflict)");
  }

  // ---- audit tightening (@Codex correction 5): unknown is NULL, not a masquerade ----
  check(!def.kindOf(core::ModuleId{9999}).has_value(),
        "kindOf(unknown id) is nullopt, NOT a kUnsupported masquerade");
  check(!def.kindOf(core::ModuleId{9999}) &&
            def.kindOf(core::ModuleId{9999}) != core::ExecutionKind::kUnsupported,
        "unknown id and deferred module are DIFFERENT kindOf results");
  check(def.contractOf(core::ModuleId{9999}) == nullptr,
        "contractOf(unknown id) returns nullptr, NOT a valid-looking default sentinel");

  // ---- real, owned contracts (@Codex correction 3) -------------------------------
  const core::ModuleExecutionContract* vcoB = def.contractOf(core::ModuleId::vco_b);
  check(vcoB != nullptr, "contractOf(vco_b) returns an owned contract");
  check(vcoB->allowedInCyclicSCC, "vco_b contract is cycle-safe (VCO-B self-loop)");
  check(vcoB->pathDelayCount == 1, "vco_b declares exactly one path");
  check(vcoB->pathDelays[0].inPort == reg::JackId::vco_b_cv_in &&
            vcoB->pathDelays[0].outPort == reg::JackId::vco_b_vco_out,
        "vco_b path is cv_in -> vco_out");
  check(vcoB->pathDelays[0].canDirectThrough && vcoB->pathDelays[0].minCausalDelaySamples == 0.0,
        "vco_b path is direct/min0");
  check(core::module_contract_is_valid(*vcoB), "vco_b owned contract satisfies the valid gate");

  const core::ModuleExecutionContract* envFol = def.contractOf(core::ModuleId::env_follower);
  check(envFol != nullptr, "contractOf(env_follower) returns an owned contract");
  check(envFol->allowedInCyclicSCC, "env_follower contract accepts a user return SCC");
  check(envFol->pathDelayCount == 1 &&
            envFol->pathDelays[0].inPort == core::kFixedEndpointJackSentinel &&
            envFol->pathDelays[0].outPort == reg::JackId::env_follower_env_out,
        "env_follower path is kFixedEndpointJackSentinel -> env_out");
  check(envFol->pathDelays[0].canDirectThrough && envFol->pathDelays[0].minCausalDelaySamples == 0.0,
        "env_follower path is direct/min0");
  check(core::module_contract_is_valid(*envFol), "env_follower owned contract satisfies the valid gate");

  const core::ModuleExecutionContract* preamp = def.contractOf(core::ModuleId::preamp);
  check(preamp != nullptr && preamp->allowedInCyclicSCC && preamp->pathDelayCount == 0,
        "preamp is cycle-admit only (allowedInCyclicSCC, no invented internal path)");
  check(core::module_contract_is_valid(*preamp), "preamp owned contract satisfies the valid gate");

  // Every non-SCC-member module defaults to cycle-UNSAFE.
  check(!def.contractOf(core::ModuleId::mixer)->allowedInCyclicSCC,
        "a non-SCC module (mixer) defaults to cycle-unsafe");
  check(core::module_contract_is_valid(*def.contractOf(core::ModuleId::mixer)),
        "the default mixer contract satisfies the valid gate");

  // ---- the sentinel is not any real registered descriptor ------------------------
  bool sentinelIsRealJack = false;
  for (std::uint32_t i = 0; i < reg::kJackCount; ++i) {
    if (reg::kJacks[i].id == core::kFixedEndpointJackSentinel) sentinelIsRealJack = true;
  }
  check(!sentinelIsRealJack,
        "no registry descriptor uses kFixedEndpointJackSentinel (it is an internal tag only)");

  // ---- the runtime is the product surface and carries the VCO-B self-edge ---------
  check(def.runtime().lastRebuildStatus() == core::SynthRuntime::RebuildStatus::ok,
        "runtime() rebuilt to ok");
  check(def.runtime().graphValid(), "compiled graph is valid");
  check(def.runtime().feedbackCount() >= 1,
        "the VCO-B normalized self-edge compiled into >=1 feedback edge (z^-1 break)");
  check(def.runtime().execSlotCount() >= 1, "the executor has >=1 compiled module slot");

  // ---- valid() is real after a no-op rebuild (@Codex correction 5) ---------------
  (void)def.runtime().rebuild();  // nothing dirty -> graph_unchanged
  check(def.runtime().lastRebuildStatus() == core::SynthRuntime::RebuildStatus::graph_unchanged,
        "no-op rebuild reports graph_unchanged");
  check(def.valid(), "valid() stays true on a graph_unchanged no-op rebuild");

  // ---- exact-feedback-pair primitive (@Codex correction 6, msg 02259208) ----------
  // The ONE delayed-vs-live decision is the primitive itself — it receives the query
  // (source,sink) pair and a candidate line's pair, matches EXACT (source,sink) identity,
  // and returns {matched,value}. Same source + DIFFERENT sink must be matched=false & the
  // LIVE value (a same-source NORMAL downstream does NOT own that feedback line's delay);
  // only an exact pair is matched=true & the DELAYED value. delayed(0.25) != live(0.75)
  // is a D>1-shaped state. Dropping the sink comparison (source-only matching) flips the
  // DIFFERENT-sink case to matched=true with the delayed value -> RED — the previously
  // blind spot where a bool-taking direct test stayed green and the D=1 rendered VCO-B
  // topology could not observe it.
  constexpr auto qSrc = reg::JackId::vco_b_vco_out;
  constexpr auto qSink = reg::JackId::vco_b_cv_in;
  constexpr auto otherSink = reg::JackId::vcf_cv_l_in;  // same source, different sink.
  const core::FeedbackResolve exact =
      core::feedbackSinkValue(qSrc, qSink, qSrc, qSink, 0.25, 0.75);
  check(exact.matched && exact.value == 0.25,
        "exact (src,sink) pair reads the DELAYED D-sample value");
  const core::FeedbackResolve diff =
      core::feedbackSinkValue(qSrc, qSink, qSrc, otherSink, 0.25, 0.75);
  check(!diff.matched, "same-source DIFFERENT-sink candidate is NOT an exact pair");
  check(diff.value == 0.75,
        "same-source/different-sink reads the LIVE value (source-only matching red)");
  check(exact.value != diff.value,
        "delayed and live reads are DISTINCT (D>1-shaped state discriminates)");

  // ---- oracle: canonical rendered VCO-B integration (@Codex 5f8845fe, b4e0e731 §4) ----
  // The VCO-B self-edge (vco_b.vco_out -> cv_in) is the machine's ONE active normalized
  // route. This proves the RENDERED machine routes + stages it: the compiler produces a
  // feedback line exactly matching the self-edge pair, that line's D-sample slot delivers
  // the PREVIOUS frame's published vco_out to THIS frame's cv_in (the z^-1 break), the
  // machine stays finite, and a same-seed fresh run is bit-identical (determinism).
  // @Codex f33b1f44: the exact-pair-vs-source-only discriminator CANNOT be separated by a
  // rendered trace (1-deep ring is overwritten), so it lives in the feedbackSinkValue
  // primitive (asserted above); this oracle pins routing + staging, not that discriminator.
  {
    core::MachineRuntimeDefinition d1(kSeed, kSr);
    core::MachineRuntimeDefinition d2(kSeed, kSr);
    // item 1: the self-edge drives vco_b's generic CV input — be EXPLICIT about the mode.
    d1.runtime().setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
    d2.runtime().setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
    int fb = -1;
    for (std::uint32_t i = 0; i < d1.runtime().feedbackCount(); ++i) {
      if (d1.runtime().feedbackAt(i).sourceJack == reg::JackId::vco_b_vco_out &&
          d1.runtime().feedbackAt(i).sinkJack == reg::JackId::vco_b_cv_in) {
        fb = static_cast<int>(i);
        break;
      }
    }
    check(fb >= 0, "the compiler produces a VCO-B self-edge feedback line (vco_b.vco_out -> cv_in)");
    bool finite = true, identical = true, routeOk = true;
    double prevPub = 0.0;
    for (int i = 0; i < 16; ++i) {
      // The value the self-edge feeds this frame's vco_b.cv_in (read from the D-sample line,
      // BEFORE the frame publishes a new vco_out).
      const double g = d1.runtime().feedbackAt(fb).buf[d1.runtime().feedbackAt(fb).writePos];
      const core::RuntimeOutput o1 = d1.runtime().processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
      const core::RuntimeOutput o2 = d2.runtime().processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
      if (!std::isfinite(o1.dryB) || !std::isfinite(o2.dryB)) finite = false;
      if (o1.dryB != o2.dryB) identical = false;
      if (i > 0 && g != prevPub) routeOk = false;  // D=1: this frame's cv_in == last frame's vco_out.
      prevPub = o1.dryB;
    }
    check(finite, "canonical VCO-B self-edge renders finite output (no NaN/Inf)");
    check(identical, "VCO-B self-edge render is bit-identical across a same-seed fresh run");
    check(routeOk, "the self-edge D-sample line delivers the previous frame's vco_out to cv_in");
  }

  // ---- oracle: same-sample real path (b4e0e731 §4.2) ------------------------------
  // A real source->sink PRODUCT edge (env_follower.env_out -> drone_2.cv_mod_in via a user
  // patch cable), driven by the graph — NEVER setControlVoltage (the §5 negative ③/⑤). The
  // producer (env_follower, fed by preamp) runs EARLIER in the plan order, publishes env_out
  // at sample N, and the drone_2 group resolves its shared CV MOD the SAME sample. A
  // frame-front global resolve (resolve once before the frame loop, §5 negative ②) would
  // read the PREVIOUS frame's env_out — one sample late — so the per-frame equality below
  // is the detector. driveGraph must be true (the compiled graph, not a hand-set voltage,
  // feeds the sink).
  {
    core::MachineRuntimeDefinition d(kSeed, kSr);
    d.runtime().setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
    check(d.runtime().connect(reg::JackId::env_follower_env_out, reg::JackId::drone_2_cv_mod_in),
          "connect env_follower.env_out -> drone_2.cv_mod_in (real product edge)");
    check(d.runtime().rebuild(), "same-sample plan rebuilds ok");
    bool sameSample = true;
    for (int i = 0; i < 16; ++i) {
      const double ext = 0.5 + 0.25 * (i % 4);  // deterministic varying EXT.AUDIO drive.
      (void)d.runtime().processFrame(core::RuntimeInputs{ext, ext}, /*driveGraph=*/true);
      const double droneMod = d.runtime().droneGroupModCv(1);  // drone_2 == classic group 1.
      const double envOut = d.runtime().controlVoltageAt(reg::JackId::env_follower_env_out);
      if (std::fabs(droneMod - envOut) > 1e-12) sameSample = false;
    }
    check(sameSample, "drone_2 cv_mod resolves env_follower.env_out the SAME sample (no 1-frame lag)");
  }

  // ---- oracle: canonical CV-MOD product behavior (Gap 2, @Codex 864b2d24) ----
  // The block above reads droneGroupModCv(1) — the shared group CV the bank multiplies
  // (the live PRODUCT value that actually detunes the generators, not a shadow). @Codex
  // wants RENDERED proof that the CV audibly reaches the WET output, plus a MOD-off
  // baseline, on the canonical machine with an ASYMMETRIC classic-group-1 (drone_2)
  // generator config:
  //   (1) MOD-on (gen0 amount>0, gens 1-4 amount=0): a canonical env_follower.env_out ->
  //       drone_2.cv_mod_in cable MUST change the rendered WET vs the no-cable run. A
  //       skipped resolve leaves the cable inert -> with-cable == no-cable -> RED.
  //   (2) MOD-off (all gens amount=0): the SAME cable must leave WET bit-identical
  //       (design/07 §7 MOD-off ignores CV + env). A "MOD-off responds" bug -> RED.
  //   Frame-front regression reds the timing probe above (same-sample equality); (1)+(2)
  //   ground the CV->audio flow. Together they are the product oracle.
  {
    auto renderDroneMod = [&](bool wired, double amount, core::RuntimeOutput* seq, std::size_t n) {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      rt.setDroneMod(1, 0, amount);   // asymmetric: group 1 gen 0 carries the MOD amount.
      rt.setDroneMod(1, 1, 0.0);      // gens 1-4 are MOD-off.
      rt.setDroneMod(1, 2, 0.0);
      rt.setDroneMod(1, 3, 0.0);
      rt.setDroneMod(1, 4, 0.0);
      if (wired) {
        check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::drone_2_cv_mod_in),
              "G2 wire env_follower.env_out -> drone_2.cv_mod_in");
        check(rt.rebuild(), "G2 CV-MOD cable plan rebuilds ok");
      }
      for (std::size_t i = 0; i < n; ++i)
        seq[i] = rt.processFrame(core::RuntimeInputs{0.5 + 0.25 * double(i % 4), 0.5 + 0.25 * double(i % 4)}, true);
    };
    constexpr std::size_t kN = 16;
    auto sameWet = [](const core::RuntimeOutput* a, const core::RuntimeOutput* b, std::size_t n) {
      for (std::size_t i = 0; i < n; ++i)
        if (a[i].wetL != b[i].wetL || a[i].wetR != b[i].wetR) return false;
      return true;
    };
    core::RuntimeOutput onWire[kN], onNoCable[kN];
    renderDroneMod(true, 1.0, onWire, kN);
    renderDroneMod(false, 1.0, onNoCable, kN);
    check(!sameWet(onWire, onNoCable, kN),
          "G2 MOD-on generator responds to the canonical cable (rendered WET differs with vs without)");
    core::RuntimeOutput offWire[kN], offNoCable[kN];
    renderDroneMod(true, 0.0, offWire, kN);
    renderDroneMod(false, 0.0, offNoCable, kN);
    check(sameWet(offWire, offNoCable, kN),
          "G2 MOD-off generator ignores the cable (rendered WET bit-identical with vs without)");
  }

  // ---- oracle: CV-MOD SAME-SAMPLE rendered differential (G2b, @Codex c30e1b47 Fix 1) ----
  // The frame-front regression above still reds only the droneGroupModCv same-sample
  // INSPECTOR. Add a differential on the actual WET output of three identical machines (same
  // seed, symmetric generator state):
  //   S source-clone runs the frame FIRST and publishes this-frame envOut;
  //   M manual-reference reads S.envOut_i and injects it via setDroneGroupModCv(1, envOut_i)
  //     before ITS OWN same frame (no cable);
  //   W wired machine carries the real env_follower.env_out -> drone_2.cv_mod_in cable.
  // Correct per-sample resolve => W and M give the SAME drone groupModCv this frame => W WET
  // is bit-identical to M WET on every sample. A frame-front resolve feeds W envOut_{i-1}
  // while M still gets envOut_i => W != M => RED. The asymmetric single-generator MOD carries
  // the amount; MOD-off (0.0) must stay bit-identical for both (generator ignores CV + env).
  {
    constexpr std::size_t kN = 16;
    auto sameWetX = [](const core::RuntimeOutput* a, const core::RuntimeOutput* b, std::size_t n) {
      for (std::size_t i = 0; i < n; ++i)
        if (a[i].wetL != b[i].wetL || a[i].wetR != b[i].wetR) return false;
      return true;
    };
    auto renderSameSample = [&](double amount, core::RuntimeOutput* wSeq, core::RuntimeOutput* mSeq) {
      core::MachineRuntimeDefinition s(kSeed, kSr), m(kSeed, kSr), w(kSeed, kSr);
      core::SynthRuntime& sr = s.runtime();
      core::SynthRuntime& mr = m.runtime();
      core::SynthRuntime& wr = w.runtime();
      for (auto* rt : {&sr, &mr, &wr}) {
        rt->setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
        rt->setDroneMod(1, 0, amount);   // asymmetric: group 1 gen 0 carries the MOD amount.
        rt->setDroneMod(1, 1, 0.0);      // gens 1-4 are MOD-off.
        rt->setDroneMod(1, 2, 0.0);
        rt->setDroneMod(1, 3, 0.0);
        rt->setDroneMod(1, 4, 0.0);
      }
      check(wr.connect(reg::JackId::env_follower_env_out, reg::JackId::drone_2_cv_mod_in),
            "G2b wire env_follower.env_out -> drone_2.cv_mod_in");
      check(wr.rebuild(), "G2b CV-MOD cable plan rebuilds ok");
      for (std::size_t i = 0; i < kN; ++i) {
        const double drive = 0.5 + 0.25 * double(i % 4);
        static_cast<void>(sr.processFrame(core::RuntimeInputs{drive, drive}, true));          // S publishes this-frame envOut.
        const double envNow = sr.controlVoltageAt(reg::JackId::env_follower_env_out);
        mr.setDroneGroupModCv(1, envNow);                          // M: same-frame env, injected.
        mSeq[i] = mr.processFrame(core::RuntimeInputs{drive, drive}, true);
        wSeq[i] = wr.processFrame(core::RuntimeInputs{drive, drive}, true);
      }
    };
    core::RuntimeOutput wOn[kN], mOn[kN];
    renderSameSample(1.0, wOn, mOn);
    check(sameWetX(wOn, mOn, kN),
          "G2b MOD-on the cabled W WET is bit-identical to the same-frame injected M WET "
          "(same-sample rendered differential; a frame-front resolve hands W a stale env => RED)");
    core::RuntimeOutput wOff[kN], mOff[kN];
    renderSameSample(0.0, wOff, mOff);
    check(sameWetX(wOff, mOff, kN),
          "G2b MOD-off the cable leaves W WET bit-identical to M (generator ignores CV + env)");
  }

  // ---- oracle: unsupported fail-closed (b4e0e731 §4.4) ---------------------------
  // keyboard is still declared-deferred (kUnsupported) this slice (the six control sources
  // are now EXECUTED — see above). Patching a REAL generated keyboard output jack into a
  // REAL VCF sink MUST REFUSE at rebuild() with the fixed unsupported_module status and NO
  // phantom keyboard slot — it must NOT return true and silently skip the unsupported
  // module (the §5 negative ④) nor fake the source via setControlVoltage.
  {
    core::MachineRuntimeDefinition d(kSeed, kSr);
    check(d.status() == core::SynthRuntime::RebuildStatus::ok,
          "the clean machine builds ok before the unsupported patch");
    check(d.runtime().connect(reg::JackId::keyboard_v_oct_out, reg::JackId::vcf_cv_l_in),
          "connect keyboard.v_oct_out -> vcf.cv_l_in (real generated jack)");
    check(!d.runtime().rebuild(), "rebuild REFUSES an unsupported module entering the plan");
    check(d.runtime().lastRebuildStatus() == core::SynthRuntime::RebuildStatus::unsupported_module,
          "refusal status is exactly unsupported_module (not a generic reject)");
    bool keyboardSlotted = false;
    for (std::uint32_t i = 0; i < d.runtime().execSlotCount(); ++i)
      if (d.runtime().execSlotAt(i).id == core::ModuleId::keyboard) keyboardSlotted = true;
    check(!keyboardSlotted, "no phantom keyboard execution slot (unsupported is not silently run)");
  }

  // ---- oracle: VCF CV L->R normalling (route.vcf_cv_l_to_cv_r, Gap 1 @Codex 864b2d24) ----
  // The description reads "CV L is normally connected to CV R if there is no CV signal in
  // the CV R; plugging into CV R overrides this." Graph resolve leaves an unplugged R at 0;
  // resolveVcfCv_ closes the gap. We observe ONLY the real filter state (vcfCvReadbackL/R
  // straight from PolivoksFilter — never a shadow mirror), L fed by a REAL product cable:
  //   (a) L wire only => the unplugged R falls back to the same-frame L (normalling).
  //   (b) add a DISTINCT R wire => R overrides (equals the cabled source), L unchanged.
  // Mutation "delete fallback" makes (a) read R=0 != L -> RED; "R never overrides" makes
  // (b) read R=L != the cabled source -> RED.
  {
    const double drv = 0.6;
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::vcf_cv_l_in),
            "G1 connect env_follower.env_out -> vcf.cv_l_in (L wire only)");
      check(rt.rebuild(), "G1 VCF normalling plan rebuilds ok (L only)");
      for (int i = 0; i < 8; ++i) (void)rt.processFrame(core::RuntimeInputs{drv, drv}, true);
      const double l = rt.vcfCvReadbackL(), r = rt.vcfCvReadbackR();
      check(l != 0.0, "G1 the L wire feeds a real nonzero CV into the filter (readback is live)");
      check(r == l, "G1 unplugged R falls back to the same-frame L (normalling route.vcf_cv_l_to_cv_r)");
    }
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::vcf_cv_l_in),
            "G1 connect env_follower.env_out -> vcf.cv_l_in");
      check(rt.connect(reg::JackId::drone_2_env_out, reg::JackId::vcf_cv_r_in),
            "G1 connect drone_2.env_out -> vcf.cv_r_in (distinct R wire)");
      check(rt.rebuild(), "G1 VCF plan rebuilds ok (L + R)");
      for (int i = 0; i < 8; ++i) (void)rt.processFrame(core::RuntimeInputs{drv, drv}, true);
      const double l = rt.vcfCvReadbackL(), r = rt.vcfCvReadbackR();
      const double src = rt.controlVoltageAt(reg::JackId::drone_2_env_out);
      check(r != l, "G1 the explicit R wire overrides the normalling (R differs from L)");
      check(r == src, "G1 R readback equals the real drone_2 env source value (product edge, not a shadow)");
      check(std::isfinite(l) && l == rt.controlVoltageAt(reg::JackId::env_follower_env_out),
            "G1 the L wire equals the real env_follower source (catches an R cross-wire polluting L)");
    }
  }

  // ---- oracle: host-terminal (EXT.AUDIO) injection + preamp env-feedback independence ----
  // @Codex msg 07bfb061: the §4.5 CONFIG2 divergence was NOT a design/semantic difference —
  // it was a real bug. When the canonical machine removed the legacy synthetic `kExtIn`
  // slot it did NOT migrate the host-terminal injection into the mixer resolve stage, so a
  // host EXT.AUDIO drive was silently dropped and CONFIG2 wrongly collapsed to the no-drive
  // CONFIG1. This is a DIFFERENTIAL oracle: two fresh bit-identical instances configured the
  // same way (same seed/SR/baseHz/self-edge neutralized via setVcoCvAmounts(1.0,0.0)), the
  // env_follower.env_out -> preamp.ext_source_in RETURN CABLE connected on both, differing
  // ONLY in the host EXT.AUDIO terminal value. Because both machines start bit-identical and
  // are fed identically except for the drive, any per-frame WET divergence is attributable
  // solely to the host-terminal injection into mixer ch4 — a VCO-phase or state artifact
  // cannot sneak in (frame i of machine A is compared to frame i of machine B, not to a
  // later frame).
  //   (a) preamp reads its DELAYED env (break-ring source), NOT the host EXT drive: the
  //       env_follower/preamp path must be INSENSITIVE to the drive (env values bit-equal
  //       across the two machines). If the preamp consumed the host drive, env would track it.
  //   (b) REGRESSION GUARD: the host EXT.AUDIO terminal must reach WET via mixer ch4, so a
  //       non-zero drive makes the two machines diverge in WET. If the mixer host-terminal
  //       injection were REMOVED, the drive is dropped and the machines stay bit-identical
  //       -> this assertion goes RED.
  {
    constexpr double kBaseHz = 220.0;
    core::MachineRuntimeDefinition d0(kSeed, kSr);  // host drive = 0.0
    core::MachineRuntimeDefinition dN(kSeed, kSr);  // host drive = 0.5
    auto config = [&](core::SynthRuntime& rt) {
      rt.setVcoBaseHz(kBaseHz);
      rt.setVcoCvAmounts(1.0, 0.0);
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      rt.setControlVoltage(reg::JackId::vco_b_v_oct_in, 0.0);
      rt.setControlVoltage(reg::JackId::vco_a_v_oct_in, 0.0);
      return rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in);
    };
    check(config(d0.runtime()), "connect return cable env_follower.env_out -> preamp.ext_source_in (drive=0.0 instance)");
    check(config(dN.runtime()), "connect return cable env_follower.env_out -> preamp.ext_source_in (drive=0.5 instance)");
    check(d0.runtime().rebuild() && dN.runtime().rebuild(), "host-terminal differential oracle rebuilds ok");
    bool diverged = false, envSensitive = false;
    for (int i = 0; i < 64; ++i) {
      const core::RuntimeOutput o0 = d0.runtime().processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
      const core::RuntimeOutput o1 = dN.runtime().processFrame(core::RuntimeInputs{0.5, 0.5}, /*driveGraph=*/true);
      if (o0.wetL != o1.wetL || o0.wetR != o1.wetR) diverged = true;
      const double e0 = d0.runtime().controlVoltageAt(reg::JackId::env_follower_env_out);
      const double e1 = dN.runtime().controlVoltageAt(reg::JackId::env_follower_env_out);
      if (e0 != e1) envSensitive = true;
    }
    // (a) preamp reads delayed env, not host EXT.
    check(!envSensitive,
          "with the return cable the env_follower/preamp path is INSENSITIVE to host drive (preamp reads delayed env, not host EXT)");
    // (b) regression guard: host-terminal injection into mixer ch4 must be live.
    check(diverged,
          "a non-zero host drive changes WET with the return cable connected (host-terminal injection into mixer ch4 is live)");
  }

  // ---- §4.6 lifetime: explicit construct/rebuild/process across stack churn -------------
  // NOT an ASan-only inference. MachineRuntimeDefinition is non-copyable/non-movable and
  // owns its SynthRuntime at a STABLE ADDRESS. Two explicit proofs:
  //  (i)  a runtime() reference obtained from a definition stays VALID after many other
  //       definitions are constructed & destroyed in nested (churned) frames — the early
  //       runtime is not aliased, invalidated, or stomped by construction churn;
  //  (ii) construct/rebuild/process is bit-deterministic across repeated construction churn —
  //       a fresh definition built in a recycled frame reproduces identical output.
  {
    core::MachineRuntimeDefinition d0(kSeed, kSr);
    core::SynthRuntime& r0 = d0.runtime();
    r0.setVcoBaseHz(220.0);
    r0.setVcoCvAmounts(1.0, 0.0);
    r0.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
    r0.setControlVoltage(reg::JackId::vco_b_v_oct_in, 0.0);
    r0.setControlVoltage(reg::JackId::vco_a_v_oct_in, 0.0);
    // churn: build + tear down many definitions in nested frames, recycling the stack region.
    for (int i = 0; i < 8; ++i) {
      core::MachineRuntimeDefinition tmp(kSeed, kSr);
      tmp.runtime().setVcoBaseHz(220.0);
      void(tmp.runtime().rebuild());
    }
    // (i) r0 (d0's runtime) is still alive + rebuildable AFTER that churn.
    check(r0.rebuild(),
          "a pre-churn runtime() reference stays valid + rebuildable after nested definition churn");
    const core::RuntimeOutput ro = r0.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
    check(std::isfinite(ro.wetL) && std::isfinite(ro.wetR) && std::isfinite(ro.dryA) &&
              std::isfinite(ro.dryB),
          "the pre-churn runtime still renders finite output (stable-address ownership, no aliasing)");
    // (ii) determinism across construction churn, via the noinline helper (destroy + rebuild
    //      a fresh definition in a dedicated frame each call).
    const core::RuntimeOutput c1 = churnSnapshot(220.0);
    const core::RuntimeOutput c2 = churnSnapshot(220.0);
    check(c1.wetL == c2.wetL && c1.wetR == c2.wetR && c1.dryA == c2.dryA && c1.dryB == c2.dryB,
          "construct/rebuild/process output is bit-identical across repeated construction churn");
    const core::RuntimeOutput c3 = churnSnapshot(440.0);
    check(c3.wetL != c1.wetL || c3.wetR != c1.wetR || c3.dryA != c1.dryA || c3.dryB != c1.dryB,
          "the churn helper is non-vacuous (a different VCO baseHz changes the output)");
  }

  // ================= A′ migration (msg 96361090): canonical product oracles ==============
  // test_machine_definition.cpp is the sole canonical product acceptance. The blocks below
  // carry the product conclusions that the engine-level fixture (test_machine_runtime.cpp)
  // previously owned, re-expressed on the canonical MachineRuntimeDefinition — the ONE
  // product machine, NEVER the synthetic makeRuntime fixture.
  //
  // A′ bullet ①: full chain / four outputs / plan order + repatch effect.
  {
    constexpr double baseHz = 220.0;
    core::MachineRuntimeDefinition dA(kSeed, kSr);
    core::MachineRuntimeDefinition dB(kSeed, kSr);  // fresh same-seed -> reproducibility.
    auto cfg = [&](core::SynthRuntime& rt) {
      rt.setVcoBaseHz(baseHz);
      rt.setVcoCvAmounts(1.0, 0.0);  // neutralize the VCO-B self-edge contribution (route stays).
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      rt.setControlVoltage(reg::JackId::vco_b_v_oct_in, 0.0);
      rt.setControlVoltage(reg::JackId::vco_a_v_oct_in, 0.0);
    };
    cfg(dA.runtime());
    cfg(dB.runtime());
    check(dA.runtime().rebuild() && dB.runtime().rebuild(),
          "A′① canonical full chain rebuilds ok (A + fresh B)");
    bool finite = true, identical = true, nonSilent = false;
    for (int i = 0; i < 32; ++i) {
      const core::RuntimeOutput oA = dA.runtime().processFrame(core::RuntimeInputs{0.5, 0.5}, /*driveGraph=*/true);
      const core::RuntimeOutput oB = dB.runtime().processFrame(core::RuntimeInputs{0.5, 0.5}, /*driveGraph=*/true);
      if (!std::isfinite(oA.wetL) || !std::isfinite(oA.wetR) ||
          !std::isfinite(oA.dryA) || !std::isfinite(oA.dryB)) finite = false;
      if (oA.wetL != oB.wetL || oA.wetR != oB.wetR ||
          oA.dryA != oB.dryA || oA.dryB != oB.dryB) identical = false;
      if (oA.wetL != 0.0 || oA.dryA != 0.0 || oA.dryB != 0.0) nonSilent = true;
    }
    check(finite, "A′① full chain: all four outputs are finite");
    check(identical, "A′① full chain: bit-identical across a same-seed fresh run (reproducibility)");
    check(nonSilent, "A′① full chain: a nonzero host drive produces a non-silent signal");

    // Plan order: the compiled plan orders source-before-consumer for the fixed chain.
    auto slotIdx = [&](core::ModuleId id) {
      for (std::uint32_t i = 0; i < dA.runtime().execSlotCount(); ++i)
        if (dA.runtime().execSlotAt(i).id == id) return static_cast<int>(i);
      return -1;
    };
    const int iD1 = slotIdx(core::ModuleId::drone_1);
    const int iMix = slotIdx(core::ModuleId::mixer);
    const int iVcf = slotIdx(core::ModuleId::vcf);
    const int iPre = slotIdx(core::ModuleId::preamp);
    const int iEnv = slotIdx(core::ModuleId::env_follower);
    check(iD1 >= 0 && iMix >= 0 && iVcf >= 0 && iPre >= 0 && iEnv >= 0,
          "A′① all fixed-chain modules are slotted in the plan");
    check(iD1 < iMix, "A′① plan order: drone before mixer (drone->mixer edge)");
    check(iMix < iVcf, "A′① plan order: mixer before vcf (mixer->vcf edge)");
    check(iPre < iMix, "A′① plan order: preamp before mixer (preamp->mixer edge)");
    check(iPre < iEnv, "A′① plan order: preamp before env_follower (preamp->env edge)");

    // Repatch effect, bit-differential (fresh same-config machine per cable state, so the
    // VCO phase is identical between comparisons — no frame-phase tautology).
    auto renderWetFp = [&](bool cabled) {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      cfg(d.runtime());
      if (cabled)
        check(d.runtime().connect(reg::JackId::env_follower_env_out,
                                  reg::JackId::preamp_ext_source_in),
              "A′① connect env_follower.env_out -> preamp.ext_source_in (return cable)");
      check(d.runtime().rebuild(), "A′① repatch rebuilds ok");
      double fp = 0.0;
      for (int i = 0; i < 8; ++i)
        fp += d.runtime().processFrame(core::RuntimeInputs{0.6, 0.6}, /*driveGraph=*/true).wetL;
      return fp;
    };
    const double fpNo = renderWetFp(false);
    const double fpYes = renderWetFp(true);
    const double fpNo2 = renderWetFp(false);
    check(fpNo2 == fpNo, "A′① repatch: the no-cable render is deterministic (same fp twice)");
    check(fpNo != fpYes, "A′① repatch: connecting the env->preamp return cable changes WET");
  }

  // A′ bullet ②: block partition / reproducibility + render-path zero allocation.
  {
    constexpr std::size_t kN = 32;
    double extArr[kN];
    for (std::size_t i = 0; i < kN; ++i) extArr[i] = 0.5 + 0.25 * (i % 4);
    core::RuntimeInputs extArrRi[kN];
    for (std::size_t i = 0; i < kN; ++i) extArrRi[i] = core::RuntimeInputs{extArr[i], extArr[i]};
    // Two FRESH, identically-configured instances: frame-loop and processBlock must each
    // run from the same initial state, or an accumulation leak would (wrongly) split them.
    auto newSimilar = [&](core::MachineRuntimeDefinition& d) {
      d.runtime().setVcoBaseHz(220.0);
      d.runtime().setVcoCvAmounts(1.0, 0.0);
      d.runtime().setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      d.runtime().setControlVoltage(reg::JackId::vco_b_v_oct_in, 0.0);
      d.runtime().setControlVoltage(reg::JackId::vco_a_v_oct_in, 0.0);
      check(d.runtime().rebuild(), "A′② canonical machine rebuilds ok");
    };
    core::RuntimeOutput frameOut[kN];
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      newSimilar(d);
      const std::size_t before = g_allocCount;
      for (std::size_t i = 0; i < kN; ++i) frameOut[i] = d.runtime().processFrame(core::RuntimeInputs{extArr[i], extArr[i]}, true);
      check(g_allocCount == before, "A′② per-frame render loop allocates nothing (zero-alloc)");
    }
    core::RuntimeOutput blockOut[kN];
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      newSimilar(d);
      const std::size_t before = g_allocCount;
      d.runtime().processBlock(extArrRi, kN, blockOut, true);
      check(g_allocCount == before, "A′② processBlock renders with zero allocation");
    }
    bool same = true;
    for (std::size_t i = 0; i < kN; ++i)
      if (frameOut[i].wetL != blockOut[i].wetL || frameOut[i].wetR != blockOut[i].wetR ||
          frameOut[i].dryA != blockOut[i].dryA || frameOut[i].dryB != blockOut[i].dryB)
        same = false;
    check(same, "A′② frame-loop and processBlock renders are bit-identical (partition invariance)");
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      newSimilar(d);
      check(d.runtime().rebuild() &&
                d.runtime().lastRebuildStatus() == core::SynthRuntime::RebuildStatus::graph_unchanged,
            "A′② render does not mutate/recompile the plan (plan stable, no hidden directive)");
    }

    // Probe non-vacuity: ::operator new(8) is a plain call the optimizer MUST perform (its
    // g_allocCount++ global store is externally visible), unlike a `new char[N]` whose
    // non-escaping pointer gets scalar-replaced away. Mirrors the engine-level probe.
    {
      g_allocCount = 0;
      void* mem = ::operator new(8);
      const bool detected = g_allocCount > 0;
      ::operator delete(mem);
      check(detected, "A′② allocator probe is non-vacuous (detects a deliberate allocation)");
      g_allocCount = 0;  // leave a clean baseline for any later render-window measurement.
    }
  }

  // A′ bullet ③+⑤: the fixed+pluggable preamp/env cycle partition invariance + a
  // representative classic drone group gate / ENV-OUT-CMOD / reproducibility snapshot
  // on the canonical machine.
  {
    constexpr std::size_t kP = 256;
    // item 7 (@Codex eaaf08cc): use NON-cycle-aligned chunk sizes. 64/128/256 are all
    // multiples of the drive's 4-sample period (drv = 0.5 + 0.25*(i%4)), so a block-boundary
    // bug that shifts the phase by a multiple of 4 is completely masked. 37 and 91 break that
    // alignment; each still partitions kP=256 exactly (37*6+34/ 91+91+74/ 128+128).
    constexpr std::size_t kBlocks35[3] = {37, 91, 128};
    double drv[kP];
    for (std::size_t i = 0; i < kP; ++i) drv[i] = 0.5 + 0.25 * (i % 4);
    core::RuntimeInputs drvRi[kP];
    for (std::size_t i = 0; i < kP; ++i) drvRi[i] = core::RuntimeInputs{drv[i], drv[i]};
    auto same35 = [&](const core::RuntimeOutput* a, const core::RuntimeOutput* b) {
      for (std::size_t i = 0; i < kP; ++i)
        if (a[i].wetL != b[i].wetL || a[i].wetR != b[i].wetR ||
            a[i].dryA != b[i].dryA || a[i].dryB != b[i].dryB) return false;
      return true;
    };

    // ③ preamp/env EXACT cycle partition-invariance: the env_follower->preamp return cable
    // added over a nonzero drive must render identically under 37/91/128 (the fixed+pluggable
    // cycle is a partition-invariant DSP path, not a block-size-sensitive one). Item 7 also
    // fixes the drive-offset bug: each chunk must read drv[b..], NOT drv[0..] from frame b.
    auto renderCycle = [&](std::size_t block, core::RuntimeOutput* seq) {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
            "A′③ connect env->preamp return cable (partition cycle)");
      for (std::size_t b = 0; b < kP; b += block) {
        const std::size_t n = (kP - b) < block ? (kP - b) : block;
        rt.processBlock(drvRi + b, n, seq + b, /*driveGraph=*/true);  // offset the drive, was drv.
      }
    };
    core::RuntimeOutput cyc[3][kP] = {};
    for (int i = 0; i < 3; ++i) renderCycle(kBlocks35[i], cyc[i]);
    check(same35(cyc[0], cyc[1]) && same35(cyc[1], cyc[2]),
          "A′③ env->preamp feedback cycle output is invariant across 37/91/128 (partition, non-aligned chunks)");
    bool cycNonZero = false;
    for (std::size_t i = 0; i < kP; ++i)
      if (cyc[0][i].wetL != 0.0 || cyc[0][i].wetR != 0.0) { cycNonZero = true; break; }
    check(cycNonZero, "A′③ env->preamp cycle render is non-silent (partition compare is non-vacuous)");

    // ⑤ representative classic drone group gate on the canonical machine: gating a classic
    // voice group ON vs OFF changes the 4-channel output, and the ON config is reproducible
    // (same seed twice -> bit-identical). Deliberately a whole-group gate (setDroneGroupGate)
    // so we exercise the joined classic gate path, not a per-voice shortcut.
    auto renderGate = [&](bool gateOn, core::RuntimeOutput* seq) {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      rt.setDroneGroupGate(/*voiceGroup=*/0, gateOn);
      for (std::size_t i = 0; i < kP; ++i) seq[i] = rt.processFrame(core::RuntimeInputs{drv[i], drv[i]}, /*driveGraph=*/true);
    };
    core::RuntimeOutput gOn[2][kP], gOff[kP];
    renderGate(true, gOn[0]);
    renderGate(true, gOn[1]);
    renderGate(false, gOff);
    check(same35(gOn[0], gOn[1]), "A′⑤ classic drone gate ON is reproducible (same seed twice, bit-identical)");
    bool gateLive = false;
    for (std::size_t i = 0; i < kP; ++i)
      if (gOn[0][i].wetL != gOff[i].wetL || gOn[0][i].wetR != gOff[i].wetR ||
          gOn[0][i].dryA != gOff[i].dryA || gOn[0][i].dryB != gOff[i].dryB) { gateLive = true; break; }
    check(gateLive, "A′⑤ classic drone group gate ON changes the output (the joined gate is live, not a shadow)");

    // item 7 (@Codex eaaf08cc): the old A′③ only asserted "non-silent + partition". Pin WHICH
    // edge the compiler broke and the break delay: the env_follower->preamp cycle must be
    // broken EXACTLY on env_follower.env_out -> preamp.ext_source_in, inserted by z_inverse
    // (a single one-sample delay, delaySamples == 1), and the line must be active. A wrong
    // break edge (e.g. the preamp->env_follower leg) or a non-z^-1 delay passes silently
    // under the old "non-silent" test — this pins the real contract.
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
            "item7: connect env->preamp return cable (feedback-edge pin)");
      check(rt.rebuild(), "item7: env->preamp cycle rebuilds ok");
      bool found = false, zInverse = false;
      for (std::uint32_t i = 0; i < rt.feedbackCount(); ++i) {
        const auto& l = rt.feedbackAt(i);
        if (l.sourceJack == reg::JackId::env_follower_env_out &&
            l.sinkJack == reg::JackId::preamp_ext_source_in) {
          found = true;
          if (l.delaySamples == 1.0) zInverse = true;
        }
      }
      check(found, "item7: the cycle break edge is env_follower.env_out -> preamp.ext_source_in");
      check(zInverse, "item7: the break edge is z^-1 (delaySamples == 1, an exact one-sample delay)");
    }

    // item 6 (@Codex eaaf08cc): the classic ENV OUT writes the LIVE source bank. Gate a
    // classic group and read back the product-published env volts from the source bank
    // (droneEnvOutVolts(0) == cvAt_(envOutJack_)). A SKIPPED publish (the old bug) would
    // leave the bank at 0 -> these go RED. We also require the env volts to CHANGE across
    // frames (a live envelope, not a frozen constant).
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setDroneGroupGate(0, true);
      bool envLive = false, envSteady = true, envConsistent = true;
      for (std::size_t i = 0; i < kP; ++i) {
        (void)rt.processFrame(core::RuntimeInputs{drv[i], drv[i]}, /*driveGraph=*/true);
        const double ev = rt.droneEnvOutVolts(0);
        if (ev != 0.0) envLive = true; else envSteady = false;   // the classic drone VCA is OPEN (level==1.0) -> full-scale every frame.
        if (ev != rt.controlVoltageAt(reg::JackId::drone_1_env_out)) envConsistent = false;
      }
      check(envLive, "item6: classic group 0 ENV OUT is written to the LIVE source bank (non-zero env volts rendered)");
      check(envSteady, "item6: classic ENV OUT is a steady, full-scale group-level DC (the open VCA gain === 1.0 is published EVERY frame, not a one-off glitch)");
      check(envConsistent, "item6: droneEnvOutVolts(0) == controlVoltageAt(drone_1_env_out) (same source-bank write, not a shadow)");
    }

    // ⑤b (@Codex 864b2d24): the kindOf(vco_a)==kVcoA disposition assert above is a TABLE
    // check (prone to inspector/primitive-only). Add a RENDERED canonical-output criterion
    // on dryA: VCO-A must actually VOICE (dryA nonzero at some frame) AND respond to its own
    // pitch drive independently of VCO-B. Two identical free-running oscillators at the same
    // base produce dryA==dryB legitimately, so we DIFFERENTIALIZE them: pitch VCO-B one real
    // product cable (env_follower.env_out -> vco_b.v_oct_in) up from A's base, then dryA and
    // dryB must DIVERGE somewhere (two real voice paths, not one shared/echo slot). A wrong
    // canonical ModuleId owner (vco_a disposed to kVcoB) makes the vco_a slot run the kVcoB
    // step, which writes dryB_ not dryA_ => dryA stays 0 => RED. Canonical VCO-B stays live.
    {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
      rt.setVcoBaseHz(110.0);
      bool aVoice = false, bVoice = false, independent = false, finite = true;
      check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::vco_b_v_oct_in),
            "⑤b wire env_follower.env_out -> vco_b.v_oct_in (real pitch cable)");
      check(rt.rebuild(), "⑤b pitch-cable plan rebuilds ok");
      for (std::size_t i = 0; i < kP; ++i) {
        const core::RuntimeOutput o = rt.processFrame(core::RuntimeInputs{drv[i], drv[i]}, true);
        if (o.dryA != 0.0) aVoice = true;
        if (o.dryB != 0.0) bVoice = true;
        if (o.dryA != o.dryB) independent = true;
        if (!std::isfinite(o.dryA) || !std::isfinite(o.dryB)) finite = false;
      }
      check(finite, "⑤b VCO-A/B outputs stay finite under a real pitch cable");
      check(aVoice, "⑤b VCO-A voices a real dryA output (wrong ModuleId owner leaves vco_a silent => RED)");
      check(bVoice, "⑤b VCO-B voices a real dryB output (the B voice is independently live)");
      check(independent, "⑤b dryA and dryB diverge under a distinct pitch (separate A/B voices, no echo/shared slot)");
    }

    // ③b (@Codex 864b2d24): upgrade neg-③ from the pure feedbackSinkValue helper to a
    // RUNTIME-excited discriminator. Warm up WITHOUT the env->preamp return cable using a
    // small and a large drive so the LIVE env bank holds DIFFERENT nonzero values (the
    // warm-up env is real product output, not a fabricated hand-set). Then connect the
    // return + rebuild — rebuild FRESH-zeroes the break edge's own D-sample buffer
    // (buf[k]=0, writePos=0, machine_runtime.h:1091) — and read the preamp's ACTUAL resolved
    // input on the first post-rebuild frame. Correct per-edge delay => the preamp reads 0.0
    // in BOTH magnitudes (the freshly-zeroed D-sample erases the warm-up env difference). A
    // "reads live, not per-edge-delay" bug hands the preamp the stale nonzero env, which
    // SCALES with the warm-up drive => RED.
    {
      struct WarmUpProbe { double warmEnv; double firstPost; };
      auto probe = [&](double warmDrv) -> WarmUpProbe {
        core::MachineRuntimeDefinition d(kSeed, kSr);
        core::SynthRuntime& rt = d.runtime();
        rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
        for (int i = 0; i < 12; ++i) (void)rt.processFrame(core::RuntimeInputs{warmDrv, warmDrv}, true);  // no return cable yet.
        const double warmEnv = rt.controlVoltageAt(reg::JackId::env_follower_env_out);
        check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
              "③b connect env_follower.env_out -> preamp.ext_source_in after warm-up");
        check(rt.rebuild(), "③b return-cable plan rebuilds ok");
        (void)rt.processFrame(core::RuntimeInputs{warmDrv, warmDrv}, true);  // first post-rebuild frame.
        return WarmUpProbe{warmEnv, rt.preampResolvedInput()};
      };
      const WarmUpProbe plo = probe(0.15), phi = probe(0.85);
      check(plo.warmEnv != 0.0, "③b warm-up yields a NONZERO live env (small drive) — discriminator is live");
      check(phi.warmEnv != 0.0, "③b warm-up yields a NONZERO live env (large drive)");
      check(plo.warmEnv != phi.warmEnv,
            "③b warm-up env scales with drive (small != large) so delayed-vs-live is DISTINCT");
      check(plo.firstPost == 0.0,
            "③b first-post-rebuild preamp reads the DELAYED zeroed D-sample (small warm-up)");
      check(phi.firstPost == 0.0,
            "③b first-post-rebuild preamp reads the zeroed D-sample (large warm-up) — a live read scales with env => RED");
    }

    // ③c (@Codex c30e1b47, Fix 2): ③b's firstPost==0.0 is STILL a getter-only discriminator
    // (preampResolvedInput() + a pure primitive). Add a TRUE equivalent on rendered WET.
    // Two same-seed machines warmed up with the SAME drive/frame count (no return cable).
    // @Codex ce765d6d rework — the earlier "both call rebuild()" symmetric reset was NOT actually
    // symmetric: the no-cable side's rebuild() early-returned as graph_unchanged (cached no-op,
    // machine_runtime.h:1451) and did NOT reset, so the VCO-B self-edge kept its warmup value while
    // the cable side's real reset zeroed it => dryB A≠B at non-zero CV depth (localized to
    // feedback-init, not wrong-slot/scheduling). Fix: BOTH sides force a REAL rebuild. A keeps the
    // return cable (connect sets dirty <=> real reset, VCO-B feedback zeroed). B does connect→
    // disconnect (dirty, ends NO return cable) so it too real-rebuilds and zeroes its VCO-B feedback.
    // BOTH sides are now confirmed to actually reset VCO-B self-edge feedback, so the return-cable
    // per-edge-delay discriminator is a clean single-variable claim WITHOUT the cv_amt=0 isolation
    // (that isolation is cancelled per @Codex ce765d6d).
    // Both then process the SAME first frame with ext=0. Correct per-edge delay: A's preamp
    // reads the freshly-reset D-sample (=0), B's preamp reads the ext terminal (=0) => both
    // feed the preamp 0 => ALL FOUR outputs are bit-identical. A live-read bug hands A's
    // preamp the nonzero env => a real preamp->mixer->VCF WET that diverges from B on the
    // RENDERED output => RED (not merely a getter).
    {
      auto wetDifferential = [&](bool wireReturn, core::RuntimeOutput* seq, std::size_t n) {
        core::MachineRuntimeDefinition d(kSeed, kSr);
        core::SynthRuntime& rt = d.runtime();
        rt.setVcoControlModes(core::VcoControlMode::kExponential, core::VcoControlMode::kExponential);
        // @Codex ce765d6d: NO cv_amt=0 isolation — compare at the default non-zero CV depth.
        for (int i = 0; i < 12; ++i) (void)rt.processFrame(core::RuntimeInputs{0.5 + 0.25 * double(i % 4), 0.5 + 0.25 * double(i % 4)}, true);
        if (wireReturn) {
          // A: connect the return cable (sets graphDirty_ => a REAL reset zeroes VCO-B feedback).
          check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
                "③c A connect env_follower.env_out -> preamp.ext_source_in after warm-up");
        } else {
          // B: force a REAL rebuild too, but END with no return cable. connect then disconnect the
          // same edge so graphDirty_ is set; rebuild() then actually resets (a graph_unchanged
          // no-op would NOT reset and would keep B's warmup self-edge value => dryB A≠B).
          check(rt.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
                "③c B connect env_follower.env_out -> preamp.ext_source_in (to set dirty)");
          check(rt.disconnect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in),
                "③c B disconnect the return edge (dirty set, end no return cable)");
        }
        check(rt.rebuild(), "③c plan rebuilds ok");
        // @Codex ce765d6d: confirm BOTH sides actually rebuilt (real reset, not graph_unchanged) so
        // VCO-B self-edge feedback is zeroed identically at non-zero CV depth. Line [0] is the VCO-B
        // self-edge (src=vco_b_vco_out -> sink=vco_b_cv_in) in both, and a real reset leaves it zeroed.
        check(rt.feedbackCount() >= 1, "③c a feedback line exists (VCO-B self-edge)");
        { const auto& l = rt.feedbackAt(0);
          check(l.buf[l.writePos] == 0.0,
                "③c rebuild actually reset the VCO-B self-edge buffer (a graph_unchanged no-op would keep the warmup value)"); }
        for (std::size_t i = 0; i < n; ++i) seq[i] = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, true);  // ext=0 first frame.
      };
      auto sameAll4 = [](const core::RuntimeOutput* a, const core::RuntimeOutput* b, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
          if (a[i].wetL != b[i].wetL || a[i].wetR != b[i].wetR ||
              a[i].dryA != b[i].dryA || a[i].dryB != b[i].dryB) return false;
        return true;
      };
      core::RuntimeOutput aWet[1], bWet[1];
      wetDifferential(true, aWet, 1);    // A: return + rebuild (the FIRST post frame reads the zeroed D-sample).
      wetDifferential(false, bWet, 1);   // B: no cable (the FIRST post frame reads the ext terminal, =0).
      check(sameAll4(aWet, bWet, 1),
            "③c correct per-edge delay: A and B both feed the preamp 0 on the SAME first frame => all four "
            "rendered outputs bit-identical (a live read feeds A a stale env => real preamp->mixer->VCF "
            "WET diverges from B on the rendered output => RED)");
    }
  }

  // A′ bullet ⑥: GH#6 VCF identity config, fail-closed, L/R isolation through canonical vcf_path.
  {
    core::MachineRuntimeDefinition d(kSeed, kSr);
    core::SynthRuntime& rt = d.runtime();
    core::CalibrationState calib{};
    calib.vcfLeftTrim = 1.0f;
    calib.vcfRightTrim = 1.0f;
    core::DeviceStateV1 state{};
    state.identityModelVersion = 1u;
    state.identitySeed.seed = kSeed;
    state.calibration = calib;
    check(rt.configureVcfIdentity(state.identityModelVersion, state.identitySeed.seed,
                                  state.calibration),
          "A′⑥ canonical VCF identity accepts a v1 DeviceStateV1 triple");
    check(rt.vcfIdentityConfigured(), "A′⑥ canonical VCF identity: configured true");
    const auto prof = core::deriveVcfIdentityProfile(kSeed, 1u);
    check(rt.vcfInputDrive(0) == prof.left.vcfDrive, "A′⑥ canonical VCF L drive == derived L vcfDrive");
    check(rt.vcfInputDrive(1) == prof.right.vcfDrive, "A′⑥ canonical VCF R drive == derived R vcfDrive");
    check(rt.vcfPathStagingGain(1) == prof.right.pathGain,
          "A′⑥ R staging gain is the RIGHT domain (steal-L-to-R would red)");

    // Fail-closed: unknown version + invalid trims rejected; a valid prior config is preserved.
    check(!rt.configureVcfIdentity(2u, kSeed, calib), "A′⑥ unknown identity version (v2) is rejected");
    check(rt.vcfIdentityConfigured(), "A′⑥ a rejected config preserves the prior valid identity");
    core::CalibrationState bad{};
    bad.vcfLeftTrim = 0.0f;
    bad.vcfRightTrim = 1.0f;
    check(!rt.configureVcfIdentity(1u, kSeed, bad), "A′⑥ a zero left trim is rejected");

    // L/R isolation through the rendered WET path. Same seed/version/calib across the two
    // instances; ONLY the LEFT trim differs -> L wet moves, R stays bit-identical.
    auto renderIdenWet = [&](double leftTrim, double rightTrim) {
      core::MachineRuntimeDefinition dd(kSeed, kSr);
      core::CalibrationState c{};
      c.vcfLeftTrim = static_cast<float>(leftTrim);
      c.vcfRightTrim = static_cast<float>(rightTrim);
      check(dd.runtime().configureVcfIdentity(1u, kSeed, c), "A′⑥ identity config for L/R oracle");
      double l = 0.0, r = 0.0;
      for (int i = 0; i < 256; ++i) {
        const core::RuntimeOutput o = dd.runtime().processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
        l += o.wetL;
        r += o.wetR;
      }
      return std::pair<double, double>(l, r);
    };
    const auto a = renderIdenWet(1.0, 1.0);
    const auto b = renderIdenWet(0.4, 1.0);
    check(std::fabs(a.first - b.first) > 1e-4, "A′⑥ changing the LEFT trim moves the L wet path");
    check(a.second == b.second, "A′⑥ changing the LEFT trim leaves R wet path bit-identical (isolation)");

    // Partition invariance through the canonical vcf_path: an identity-configured machine
    // must render identically via the per-frame loop and processBlock (vcf window is a
    // deterministic audio stage, not a block-size-sensitive one).
    constexpr std::size_t kN6 = 256;
    double drv6[kN6];
    for (std::size_t i = 0; i < kN6; ++i) drv6[i] = 0.2 + 0.1 * (i % 3);
    core::RuntimeInputs drv6Ri[kN6];
    for (std::size_t i = 0; i < kN6; ++i) drv6Ri[i] = core::RuntimeInputs{drv6[i], drv6[i]};
    auto idenSimilar = [&](core::MachineRuntimeDefinition& dd) {
      core::CalibrationState c{};
      c.vcfLeftTrim = 1.0f;
      c.vcfRightTrim = 1.0f;
      check(dd.runtime().configureVcfIdentity(1u, kSeed, c), "A′⑥ VCF identity config (partition oracle)");
    };
    core::RuntimeOutput idenFrame[kN6];
    {
      core::MachineRuntimeDefinition dd(kSeed, kSr);
      idenSimilar(dd);
      for (std::size_t i = 0; i < kN6; ++i) idenFrame[i] = dd.runtime().processFrame(core::RuntimeInputs{drv6[i], drv6[i]}, true);
    }
    core::RuntimeOutput idenBlock[kN6];
    {
      core::MachineRuntimeDefinition dd(kSeed, kSr);
      idenSimilar(dd);
      dd.runtime().processBlock(drv6Ri, kN6, idenBlock, /*driveGraph=*/true);
    }
    bool idenSame = true;
    for (std::size_t i = 0; i < kN6; ++i)
      if (idenFrame[i].wetL != idenBlock[i].wetL || idenFrame[i].wetR != idenBlock[i].wetR ||
          idenFrame[i].dryA != idenBlock[i].dryA || idenFrame[i].dryB != idenBlock[i].dryB)
        idenSame = false;
    check(idenSame, "A′⑥ VCF identity render is block-partition invariant (frame-loop == processBlock)");
  }

  // A′ bullet ④: EventTimebase -> drone_3 dispatch sample timing through the canonical machine.
  {
    constexpr std::size_t kTot = 256;
    constexpr std::size_t kEventSample = 100;
    constexpr std::size_t kBlocks[3] = {64, 128, 256};
    static const core::RuntimeInputs kSilence[kTot] = {core::RuntimeInputs{0.0, 0.0}};
    auto pitchEvent = [&]() {
      core::ControlEvent ev{};
      ev.kind = core::ControlEventKind::parameter;
      ev.parameter = core::ParameterId::drone_3_pitch;
      // 0.5 is the DEFAULT applied_to_dsp value of drone_3.pitch (registry initial=0.5), and
      // task#78 now pre-applies the full default state in the definition ctor — so a 0.5
      // event is a no-op and the script would look vacuous. Use a value off default so the
      // event still exercises the dispatch (sample-timing under test), while the applied
      // baseline stays reflected.
      ev.value = 0.85;
      ev.source = 1;
      ev.producerSequence = 1;
      core::TimedControlEvent te{};
      te.event = ev;
      te.sample = kEventSample;
      return te;
    };
    // Render the CANONICAL machine's 4-channel output over kTot frames in `block` chunks,
    // optionally enqueueing the pitch event. Both paths go through the identical ctor
    // rebuild, so the ONLY difference is the event. We read the full RuntimeOutput
    // (the canonical observable) — NOT drone3Channel(), which is a block-latched
    // modulation value and therefore not a per-frame sample-timing signal.
    auto renderOut = [&](std::size_t block, core::RuntimeOutput* seq, bool withEvent) {
      core::MachineRuntimeDefinition d(kSeed, kSr);
      core::SynthRuntime& rt = d.runtime();
      if (withEvent) static_cast<void>(rt.enqueueControlEvent(pitchEvent()));
      for (std::size_t b = 0; b < kTot; b += block) {
        const std::size_t n = (kTot - b) < block ? (kTot - b) : block;
        rt.processBlock(kSilence, n, seq + b, /*driveGraph=*/true);
      }
    };
    auto sameOut = [&](const core::RuntimeOutput* a, const core::RuntimeOutput* b) {
      for (std::size_t i = 0; i < kTot; ++i)
        if (a[i].wetL != b[i].wetL || a[i].wetR != b[i].wetR ||
            a[i].dryA != b[i].dryA || a[i].dryB != b[i].dryB) return false;
      return true;
    };
    core::RuntimeOutput base[3][kTot] = {};
    for (int bi = 0; bi < 3; ++bi) renderOut(static_cast<std::size_t>(kBlocks[bi]), base[bi], false);
    check(sameOut(base[0], base[1]) && sameOut(base[1], base[2]),
          "A′④ no-event baseline output is buffer-invariant (64/128/256)");
    core::RuntimeOutput ev[3][kTot] = {};
    for (int bi = 0; bi < 3; ++bi) renderOut(static_cast<std::size_t>(kBlocks[bi]), ev[bi], true);
    check(sameOut(ev[0], ev[1]) && sameOut(ev[1], ev[2]),
          "A′④ scripted output is buffer-invariant (event acts at the SAME sample)");
    check(!sameOut(ev[0], base[0]),
          "A′④ event script differs from the empty script (drone_3 dispatch exercised, not vacuous)");

    // Sample timing, differential: the event has NO effect before its scheduled sample and
    // an effect at/after it. Compare the event script against the no-event script frame by
    // frame (the canonical drone_3 may not default to silence, so we never assume a zero
    // preamble).
    const core::RuntimeOutput* s = ev[2];   // block 256 == one block.
    const core::RuntimeOutput* e = base[2];
    bool beforeNoEffect = true, atAfterEffect = false;
    for (std::size_t i = 0; i < kTot; ++i) {
      const bool differsAt = s[i].wetL != e[i].wetL || s[i].wetR != e[i].wetR ||
                             s[i].dryA != e[i].dryA || s[i].dryB != e[i].dryB;
      if (i < kEventSample) { if (differsAt) beforeNoEffect = false; }
      else if (differsAt) atAfterEffect = true;
    }
    check(beforeNoEffect, "A′④ the pitch event does not change output before its scheduled sample");
    check(atAfterEffect, "A′④ the pitch event changes output at/after the scheduled sample");
  }

  std::printf("\n[%s] %d checks, %d failed\n", g_fail == 0 ? "PASS" : "FAIL", g_checks,
              g_fail);
  return g_fail == 0 ? 0 : 1;
}
