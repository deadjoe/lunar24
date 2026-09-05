// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_machine_cable_restore.cpp — task #80 (GH#12 9D C3): the DeviceState user-cable atomic
// restore into the REAL PatchGraph, driven through the agreed entry and asserted blindly against
// the restore contract.
//
// @Codex 7ce76a8b requirements, verified here through encode -> decode ->
// StandaloneAudioEngine.applyDeviceState -> processBlock (the shared test_engine_harness.h), never
// buildMachineRuntimeCandidate/processFrame directly:
//   (a) the no-cable default restores to the ORIGINAL default (no stray cable, the single active
//       normalized route intact, output deterministic/unperturbed);
//   (b) a user cable into a live route's sink OVERRIDES the active normalized route, and removing
//       it restores the route;
//   (c) a real supported-module asymmetric cable produces the expected CONTROL and AUDIO difference;
//   (d) the same state restored twice (and a positional-array re-arrangement) gives the same wire
//       set / plan / rendered output (idempotent, order-independence);
//   (e) multiple cables restore with NO loss (per-sink cardinality + per-source capacity held);
//   (f) an unsupported/deferred graph fails as a WHOLE candidate and the prior accepted owner is
//       preserved by the engine's single-commit guard (typed reject keeping state/format/plan/trace).
//
// Guardrails honored throughout (per @Codex):
//   * routeOverridden is NOT a second routing switch. check_routes already reconciled it against the
//     ACTUAL cable facts, and the restore simply reflects patch_ state; this test never re-derives a
//     route from the override bit.
//   * Sparse JackId/RouteId are never indexed by a dense index: the serialized id space has holes,
//     so every sink/source is looked up through the descriptor (find_jack / find_route), and the
//     restore loop iterates the FULL [0, kDevicePatchCapacity) and casts the slot to a JackId.
//   * cableRestoreOk_ is folded into MachineRuntimeDefinition::valid(), so ANY restore mismatch
//     (missed cable, wrong-source mis-wire, capacity-drop, stray cable) becomes a typed whole-candidate
//     rejected_graph with ZERO factory/engine change, and the fail-keeps-old-owner behavior is the
//     engine's existing single-commit guard, not a new path.

#include "mini_test.h"

#include <lunar24/core/device_state.h>       // DeviceStateV1
#include <lunar24/core/state_default.h>       // make_default_device_state
#include <lunar24/registry_ids.hpp>           // lunar24::core::JackId / RouteId full enums

#include "test_engine_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::JackId;
using lunar24::core::RouteId;
using lunar24::core::kDevicePatchCapacity;
using lunar24::core::make_default_device_state;

namespace {

// 48000 frames = 1 s @ 48 k. test_c uses a longer window because the env-follower needs time to
// attack a stage feed before the driven V/OCT jack shows its steady value.
constexpr int kFrames = 48000;
constexpr int kLongFrames = 96000;

// Set a user cable source -> sink in a state. The sink slot is the serialized JackId (NOT a dense
// index — the id space has holes); the route-override bookkeeping, where a route's sink is used,
// is applied separately via overrideRoute() so check_routes coherence holds.
void setCable(DeviceStateV1& st, JackId source, JackId sink) {
  const std::uint32_t s = static_cast<std::uint32_t>(sink);
  st.inputCable[s] = 1;
  st.cableSource[s] = source;
}

// Mark a landed route overridden because its sink now holds a user cable. check_routes requires
// routeOverridden[routeId] == 1 exactly when the route's sink has a user cable; without this the
// state is validation-incoherent.
void overrideRoute(DeviceStateV1& st, RouteId id) {
  st.routeOverridden[static_cast<std::uint32_t>(id)] = 1;
}

// Verify the LIVE user-cable bank equals the requested set exactly: cableCount matches, and every
// requested sink holds exactly ONE user cable reachable from its requested source (no stray, no
// displaced, no wrong-source wire). This is the defense against a lone connect()==true being
// treated as complete (connect() can atomically displace a prior cable at a saturated port).
bool verifyWires(const SynthRuntime* r, const DeviceStateV1& st) {
  if (r == nullptr) return false;
  std::uint32_t requested = 0;
  for (std::uint32_t i = 0; i < kDevicePatchCapacity; ++i) {
    if (st.inputCable[i] != 0u) ++requested;
  }
  if (r->cableCount() != requested) return false;
  for (std::uint32_t i = 0; i < kDevicePatchCapacity; ++i) {
    if (st.inputCable[i] == 0u) continue;
    const JackId sink = static_cast<JackId>(i);
    if (r->cableCountInto(sink) != 1u) return false;          // no displacement on the sink
    if (!r->cableConnected(st.cableSource[i], sink)) return false;  // no wrong-source / stray
  }
  return true;
}

std::vector<double> captureAll(EngineHarness& h, int frames, double feed) {
  CHECK(h.render(frames, feed));
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(frames) * 4u);
  for (int ch = 0; ch < 4; ++ch) {
    const auto& c = h.out(ch);
    v.insert(v.end(), c.begin(), c.end());
  }
  return v;
}

// Capture ONLY the `frames` freshly rendered by this call (the four output channels, WET_L/WET_R/
// DRY_A/DRY_B). The EngineHarness buffers are monotonically growing, so a same-owner sequence of
// renders must read only the newly appended segment — otherwise a later capture would include the
// earlier renders and two captures would be compared against overlapping tails.
std::vector<double> captureSegment(EngineHarness& h, int frames, double feed) {
  const std::size_t prev = h.wetL().size();
  CHECK(h.render(frames, feed));
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(frames) * 4u);
  for (int ch = 0; ch < 4; ++ch) {
    const auto& c = h.out(ch);
    v.insert(v.end(), c.begin() + static_cast<std::ptrdiff_t>(prev), c.end());
  }
  return v;
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return 1e30;
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::fmax(m, std::fabs(a[i] - b[i]));
  return m;
}

// Structural multi-cable state for (d)/(e): three cables from distinct sources to distinct
// non-route sinks, all across already-compiled (supported) modules, so the restore must carry all
// three with no loss and no override bookkeeping.
DeviceStateV1 multiCableState() {
  DeviceStateV1 s = make_default_device_state(0x4C554E4152ULL);
  setCable(s, JackId::env_follower_env_out, JackId::drone_1_cv_mod_in);
  setCable(s, JackId::vco_b_vco_out, JackId::drone_2_cv_mod_in);
  setCable(s, JackId::joystick_x_out, JackId::drone_4_cv_mod_in);
  return s;
}

// _________________________________________________________________________________________________
// (a) no-cable default == original default.

void test_no_cable_default() {
  // The no-cable default has NO user cable introduced by the restore, and the single active
  // normalized route (VCO-B self-edge) is intact (not overridden, not lost).
  EngineHarness h;
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h.runtime() != nullptr);
  CHECK(h.runtime()->cableCount() == 0u);
  CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));

  // The restore is a faithful no-op for the default: requesting 0 cables cannot perturb it, so two
  // independent loads of the same default render BIT-identical output (deterministic, unperturbed).
  EngineHarness h2;
  CHECK(h2.load(make_default_device_state(0x4C554E4152ULL)));
  const std::vector<double> o1 = captureAll(h, kFrames, 0.0);
  const std::vector<double> o2 = captureAll(h2, kFrames, 0.0);
  CHECK(maxAbsDiff(o1, o2) < 1e-12);
}

// _________________________________________________________________________________________________
// (b) on the SAME owner, a user cable into a live route's sink OVERRIDES the active normalized
// route (the user source actually replaces the self-edge), and removing it restores the route with
// no residual wire. The route's sink is vco_b.cv_in; the default self-edge (vco_b.vco_out ->
// vco_b.cv_in) is the single active normalized route. Feeding a constant signal makes the
// env-follower source non-zero, so the override is signal-observable, not just a normalizedActive
// bit: the user source drives a different waveform than the self-edge feedback, so the rendered
// output differs; after removal the output is BIT-identical to the original default (no stray wire
// left behind, the self-edge feedback restored).

void test_override_same_owner_replaces_self_edge() {
  constexpr double kFeed = 0.6;
  EngineHarness h;

  // active A = default: the self-edge route is live, zero user cables.
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h.runtime() != nullptr);
  CHECK(h.runtime()->cableCount() == 0u);
  CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  const std::vector<double> base = captureSegment(h, kLongFrames, kFeed);

  // Cable env_follower.env_out -> vco_b.cv_in on the SAME owner overrides the self-edge. check_routes
  // coherence requires the override bit set when the route's sink holds a user cable.
  DeviceStateV1 wired = make_default_device_state(0x4C554E4152ULL);
  setCable(wired, JackId::env_follower_env_out, JackId::vco_b_cv_in);
  overrideRoute(wired, RouteId::route_vco_b_vco_out_to_cv_in);
  CHECK(h.load(wired));
  CHECK(verifyWires(h.runtime(), wired));
  CHECK(h.runtime()->cableCount() == 1u);
  CHECK(h.runtime()->cableConnected(JackId::env_follower_env_out, JackId::vco_b_cv_in));
  CHECK(h.runtime()->cableCountInto(JackId::vco_b_cv_in) == 1u);
  // The compiled effective edge at the sink is now the USER cable, not the self-edge: the normalized
  // route is no longer active. (effectiveEdge == cableConnected || normalizedActive; the cable is
  // present and the route is off, so the sink is driven by the user source.)
  CHECK_FALSE(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  const std::vector<double> wiredOut = captureSegment(h, kLongFrames, kFeed);
  // The user source REPLACED the self-edge: the rendered output (WET+DRY) clearly differs from the
  // self-edge feedback. This is not merely a flag flip — the driven signal changed.
  CHECK(maxAbsDiff(base, wiredOut) > 5e-3);

  // Remove the cable on the SAME owner: the route is restored and NO residual wire remains.
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h.runtime()->cableCount() == 0u);
  CHECK(h.runtime()->cableCountInto(JackId::vco_b_cv_in) == 0u);   // no residual user cable
  CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  const std::vector<double> restored = captureSegment(h, kLongFrames, kFeed);
  CHECK(maxAbsDiff(base, restored) < 1e-12);   // exactly the original default — no stray wire/feedback
}

// _________________________________________________________________________________________________
// (c) a real supported-module asymmetric cable produces the expected control + audio difference.
//
// An asymmetric cable into an INPUT on a supported module (env_follower.env_out -> drone_2.cv_mod_in)
// is the canonical proof. We hold drone_2's MOD amount (drone_2.mod_1) at 1.0 in BOTH the reference
// and the wired machine, so the ONLY difference between the two states is the cable itself; any
// control/audio delta is then attributable to the cable, not the (shared) MOD parameter.
//
// Why drone CV-MOD rather than vco_a.v_oct_in: vco_a's v_oct input port is an internal modulation
// input, NOT a published control source, so controlVoltageAt() reports 0 on it even when driven.
// droneGroupModCv(voiceGroup) is the runtime's canonical readback for a drone MOD cable — it reads
// the shared group CV the bank multiplies — and droneChannel() is the per-voice rendered audio, so a
// MOD cable moves BOTH observables. This mirrors the existing MOD-on oracle but through the full
// encode -> decode -> applyDeviceState -> processBlock path, never the direct runtime setter/connect.

void test_supported_asymmetric_cable_difference() {
  // Stage feed so the env-follower is non-zero; identical across ref/wired so ANY difference is due
  // solely to the cable (env_follower.env_out -> drone_2.cv_mod_in, a supported asymmetric MOD edge).
  constexpr double kFeed = 0.5;

  // Reference state: drone_2.mod_1 = 1.0 but NO cable — the MOD CV group sees no source, so the group
  // CV is 0 and the drone_2 voice is unmodulated.
  DeviceStateV1 refState = make_default_device_state(0x4C554E4152ULL);
  refState.parameters[static_cast<std::uint32_t>(lunar24::core::ParameterId::drone_2_mod_1)] = 1.0;

  EngineHarness ref;
  CHECK(ref.load(refState));
  CHECK(ref.runtime() != nullptr);

  // Wired state: the SAME MOD amount plus the asymmetric env -> drone_2 cv_mod cable. The only delta
  // from refState is the cable, so both differences below come from the cable.
  DeviceStateV1 wired = refState;
  setCable(wired, JackId::env_follower_env_out, JackId::drone_2_cv_mod_in);

  // Establish active A (default) first so a restore-regression reject keeps a valid runtime (default)
  // and the checks below go RED cleanly rather than dereferencing a never-accepted engine.
  EngineHarness h;
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h.load(wired));
  CHECK(verifyWires(h.runtime(), wired));
  CHECK(h.runtime()->cableConnected(JackId::env_follower_env_out, JackId::drone_2_cv_mod_in));
  CHECK(h.runtime()->cableCountInto(JackId::drone_2_cv_mod_in) == 1u);

  // CONTROL difference: group-1 shared MOD CV. Un-wired the group sees nothing (== 0); wired it is
  // driven by the non-zero env-follower through the same feed. drone_2 == classic group 1.
  double refCtrl = 0.0, wiredCtrl = 0.0;
  std::vector<double> refVoice, wiredVoice;
  CHECK(ref.renderSampled(kLongFrames, kFeed, [&](const SynthRuntime& r) {
    refCtrl = std::fmax(refCtrl, std::fabs(r.droneGroupModCv(1)));
    refVoice.push_back(r.droneChannel(1));   // drone_2 == classic voice index 1
  }));
  CHECK(h.renderSampled(kLongFrames, kFeed, [&](const SynthRuntime& r) {
    wiredCtrl = std::fmax(wiredCtrl, std::fabs(r.droneGroupModCv(1)));
    wiredVoice.push_back(r.droneChannel(1));
  }));
  CHECK(wiredCtrl > 1e-3);                    // the drone group MOD CV is now driven
  CHECK(std::fabs(wiredCtrl - refCtrl) > 1e-4);  // differs from the un-wired group CV (ref == 0)

  // AUDIO difference: the same cable delivers the group CV into the drone_2 voice through the held
  // MOD amount, so the rendered per-voice waveform changes. Both renders feed the same signal with the
  // same MOD amount, so the delta is from the cable alone; the drone_2 voice is the observable, not the
  // mixed WET bus (which dilutes it across the other unmodulated voices).
  CHECK(refVoice.size() == static_cast<std::size_t>(kLongFrames));
  CHECK(wiredVoice.size() == refVoice.size());
  CHECK(maxAbsDiff(refVoice, wiredVoice) > 5e-2);
}

// _________________________________________________________________________________________________
// (d) EQUIVALENT state construction restores deterministically: the SAME wire set given the same
// bytes (a positional re-arrangement is a genuinely different DeviceState object that still encodes
// the same wire set, NOT a different insertion order), and the SAME state restored repeatedly on the
// SAME owner gives the same wire set / rendered output. The state is positional, so the two fill
// orders below are byte-identical for the wire fields — the claim is "equivalent state construction
// is deterministically restored", not "insertion order is irrelevant".

void test_restore_deterministic() {
  // Three cables from distinct sources to distinct non-route sinks (multiCableState).
  const DeviceStateV1 s = multiCableState();

  // Establish active A (default) first in each engine, mirroring a host that always holds a prior
  // accepted state; a restore-regression reject then keeps a valid runtime (default) instead of a
  // never-accepted engine (runtime()==nullptr), so the checks go RED cleanly.
  EngineHarness h1;
  CHECK(h1.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h1.load(s));
  CHECK(verifyWires(h1.runtime(), s));
  const std::vector<double> o1 = captureAll(h1, kFrames, 0.0);

  // The SAME wire set written in a different array-fill order (write sink 49 before 43 before 46):
  // because the DeviceState is positional, this yields byte-identical wire bytes to `s` — it is an
  // equivalent state construction, not a different insertion order.
  DeviceStateV1 s2 = make_default_device_state(0x4C554E4152ULL);
  setCable(s2, JackId::joystick_x_out, JackId::drone_4_cv_mod_in);
  setCable(s2, JackId::env_follower_env_out, JackId::drone_1_cv_mod_in);
  setCable(s2, JackId::vco_b_vco_out, JackId::drone_2_cv_mod_in);

  EngineHarness h2;
  CHECK(h2.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h2.load(s2));
  CHECK(verifyWires(h2.runtime(), s2));
  const std::vector<double> o2 = captureAll(h2, kFrames, 0.0);

  // Same wire set twice (a positional re-arrangement is the same requested set that restores the
  // same wire set / output) — the restore is deterministic for an equivalent state.
  CHECK(maxAbsDiff(o1, o2) < 1e-12);

  // The SAME state restored repeatedly on the SAME owner gives the same wire set + output.
  EngineHarness h3;
  CHECK(h3.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h3.load(s));
  CHECK(verifyWires(h3.runtime(), s));
  const std::vector<double> r1 = captureSegment(h3, kFrames, 0.0);
  CHECK(h3.load(s));                                  // same bytes again on the same owner
  CHECK(verifyWires(h3.runtime(), s));
  const std::vector<double> r2 = captureSegment(h3, kFrames, 0.0);
  CHECK(maxAbsDiff(r1, r2) < 1e-12);
}

// _________________________________________________________________________________________________
// (e) multiple cables restore with NO loss.

void test_multi_cable_no_loss() {
  const DeviceStateV1 s = multiCableState();
  // Establish active A (default) first so a restore-regression reject keeps a valid runtime (default)
  // and the checks below go RED cleanly rather than dereferencing a never-accepted engine.
  EngineHarness h;
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h.load(s));
  CHECK(h.runtime() != nullptr);
  CHECK(verifyWires(h.runtime(), s));                      // all 3 present, per-sink cardinality == 1
  CHECK(h.runtime()->cableCount() == 3u);
  CHECK(h.runtime()->cableConnected(JackId::env_follower_env_out, JackId::drone_1_cv_mod_in));
  CHECK(h.runtime()->cableConnected(JackId::vco_b_vco_out, JackId::drone_2_cv_mod_in));
  CHECK(h.runtime()->cableConnected(JackId::joystick_x_out, JackId::drone_4_cv_mod_in));
}

// _________________________________________________________________________________________________
// (f) an unsupported/deferred graph fails as a WHOLE candidate with a TYPED reject, and the prior
// accepted owner A is preserved by the single-commit guard (state/format/plan/trace -> the exact
// same audio, and the exact same definition object). The helper load() collapses a codec failure and
// a factory rejection into a bare false, so we ALSO surface the typed StateApplyStatus and the
// recorded validation result: RejectedGraph (not RejectedInvalidState) carries validation().ok==true,
// proving the state VALIDATED and only the GRAPH failed — never an ambiguous "just false".

void test_unsupported_graph_typed_reject_preserves_a() {
  // The bad state: env_follower.env_out -> effector.cv_x_in. It VALIDATES (landed input, correct
  // direction, cardinality) but drags the kUnsupported effector into the compiled region -> a strict
  // plan rejects it as an unsupported_module. That is a GRAPH rejection, not a state/format one.
  DeviceStateV1 bad = make_default_device_state(0x4C554E4152ULL);
  setCable(bad, JackId::env_follower_env_out, JackId::effector_cv_x_in);

  // Establish the accepted owner (active A). NOTE the harness contract: a runtime() pointer is valid
  // only until the next COMMIT (= the next LOAD that accepts). A REJECTED load does not commit, but a
  // mutated build that wrongfully ACCEPTS the bad state DOES commit and would release the old
  // definition. So we never dereference `activeA` after any load — identity is proved by pointer-VALUE
  // comparison (safe even if the object was released) + the twin render below.
  EngineHarness hA;
  CHECK(hA.load(make_default_device_state(0x4C554E4152ULL)));
  const SynthRuntime* activeA = hA.runtime();
  CHECK(activeA != nullptr);
  CHECK(activeA->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));   // deref BEFORE any load

  // (i) TYPED reject on the real owner: RejectedGraph (a collapsed false, or an error-family bucket,
  // would not distinguish graph-from-state). validation().ok==true proves the state VALIDATED — the
  // factory is refusing to build the graph, not to accept the state. StateValidationResult exposes
  // `.ok/.family/.field` (no method); for a RejectedGraph outcome `.ok==true` is the precise claim.
  CHECK_FALSE(hA.load(bad));
  CHECK(hA.applyStatus() == StandaloneAudioEngine::StateApplyStatus::RejectedGraph);
  CHECK(hA.validation().ok);

  // (ii) A preserved: after the rejected path the owner STILL holds the SAME definition object
  // (pointer-value identity — no commit happened on a reject). We value-compare only; no deref.
  CHECK(hA.runtime() == activeA);

  // (iii) Pair-A comparison: the owner that ATTEMPTED (and rejected) the bad state renders the SAME
  // audio as a twin A that NEVER attempted it — the reject left the definition/plan/adapter/state
  // (hence the output) untouched. A fresh default (twin) also confirms the self-edge route is live,
  // which is the "A is intact" behaviour (identity + twin coverage together).
  EngineHarness hRef;
  CHECK(hRef.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(hRef.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  const std::vector<double> aOut = captureSegment(hA, kFrames, 0.0);
  const std::vector<double> refOut = captureSegment(hRef, kFrames, 0.0);
  CHECK(maxAbsDiff(aOut, refOut) < 1e-12);

  // (iv) An ILLEGAL state (sink is an OUTPUT jack -> cable_direction) rejects as RejectedInvalidState
  // and keeps A. This is a VALIDATION rejection (before the factory), so it can never commit in any
  // build; re-derive identity by re-reading the current runtime and value-comparing.
  {
    DeviceStateV1 illegal = make_default_device_state(0x4C554E4152ULL);
    setCable(illegal, JackId::joystick_x_out, JackId::vco_b_vco_out);   // sink is an output
    CHECK_FALSE(hA.load(illegal));
    CHECK(hA.applyStatus() == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState);
    CHECK(hA.runtime() == activeA);
  }

  // (v) An OVER-CAPACITY state (a maxCables==1 source drives two sinks -> cable_cardinality) rejects
  // as RejectedInvalidState and keeps A.
  {
    DeviceStateV1 crowded = make_default_device_state(0x4C554E4152ULL);
    setCable(crowded, JackId::env_follower_env_out, JackId::drone_1_cv_mod_in);
    setCable(crowded, JackId::env_follower_env_out, JackId::drone_2_cv_mod_in);  // source over-subscribed
    CHECK_FALSE(hA.load(crowded));
    CHECK(hA.applyStatus() == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState);
    CHECK(hA.runtime() == activeA);
  }
}

}  // namespace

int main() {
  test_no_cable_default();
  test_override_same_owner_replaces_self_edge();
  test_supported_asymmetric_cable_difference();
  test_restore_deterministic();
  test_multi_cable_no_loss();
  test_unsupported_graph_typed_reject_preserves_a();
  return test::finish("test_machine_cable_restore");
}
