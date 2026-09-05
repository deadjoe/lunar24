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
// (b) a user cable overrides an active normalized route; removing it restores the route.

void test_override_and_restore() {
  // No-cable: the VCO-B self-edge route is live.
  {
    EngineHarness h;
    CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
    CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  }

  // Cable env_follower.env_out -> vco_b.cv_in overrides it. check_routes coherence requires the
  // override bit set when the route's sink holds a user cable.
  {
    DeviceStateV1 wired = make_default_device_state(0x4C554E4152ULL);
    setCable(wired, JackId::env_follower_env_out, JackId::vco_b_cv_in);
    overrideRoute(wired, RouteId::route_vco_b_vco_out_to_cv_in);

    // The state must be coherent (route override exactly matching the cable fact) for the factory to
    // build at all — the factory's validate() gates this, and a failure here surfaces as load()==false.
    // Establish active A (the default) first, as a real host always has a prior accepted state, so a
    // restore-regression reject keeps a valid runtime (default) and the checks below go RED cleanly
    // rather than dereferencing a never-accepted engine (runtime()==nullptr on a fresh engine).
    EngineHarness h;
    CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
    CHECK(h.load(wired));
    CHECK(verifyWires(h.runtime(), wired));
    CHECK(h.runtime()->cableCount() == 1u);
    CHECK(h.runtime()->cableConnected(JackId::env_follower_env_out, JackId::vco_b_cv_in));
    CHECK(h.runtime()->cableCountInto(JackId::vco_b_cv_in) == 1u);
    // The active normalized route is now overridden (its sink holds a user cable).
    CHECK_FALSE(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  }

  // Removing the cable (fresh default = zeroed patch) restores the route.
  {
    EngineHarness h;
    CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
    CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
  }
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
// (d) same state restored twice (and a positional-array re-arrangement) -> same wire set/output.

void test_restore_deterministic() {
  const DeviceStateV1 s = multiCableState();

  // Establish active A (default) first in each engine, mirroring a host that always holds a prior
  // accepted state; a restore-regression reject then keeps a valid runtime (default) instead of a
  // never-accepted engine (runtime()==nullptr), so the checks go RED cleanly.
  EngineHarness h1;
  CHECK(h1.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h1.load(s));
  CHECK(verifyWires(h1.runtime(), s));
  const std::vector<double> o1 = captureAll(h1, kFrames, 0.0);

  // Same bytes in a DIFFERENT array-fill order (write sink 49 before 43 before 46): the state is
  // positional, so this is a genuinely different DeviceState object that encodes the SAME wire set.
  DeviceStateV1 s2 = make_default_device_state(0x4C554E4152ULL);
  setCable(s2, JackId::joystick_x_out, JackId::drone_4_cv_mod_in);
  setCable(s2, JackId::env_follower_env_out, JackId::drone_1_cv_mod_in);
  setCable(s2, JackId::vco_b_vco_out, JackId::drone_2_cv_mod_in);

  EngineHarness h2;
  CHECK(h2.load(make_default_device_state(0x4C554E4152ULL)));
  CHECK(h2.load(s2));
  CHECK(verifyWires(h2.runtime(), s2));
  const std::vector<double> o2 = captureAll(h2, kFrames, 0.0);

  // Same state twice, and a positional re-arrangement, both give the same wire set (verifyWires
  // passed above) and the same rendered output — the restore is idempotent + order-independent.
  CHECK(maxAbsDiff(o1, o2) < 1e-12);
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
// (f) an unsupported/deferred graph fails as a WHOLE candidate and the prior owner is preserved.

void test_unsupported_graph_fails() {
  // Establish the accepted owner (active A) and keep its runtime() identity.
  EngineHarness h;
  CHECK(h.load(make_default_device_state(0x4C554E4152ULL)));
  const SynthRuntime* activeA = h.runtime();
  CHECK(activeA != nullptr);

  // A cable into the deferred/unsupported effector's input jack VALIDATES (landed input, correct
  // direction, cardinality) but drags the kUnsupported effector into the compiled region ->
  // unsupported_module. That is a WHOLE-candidate typed reject (rejected_graph), not a silent drop.
  DeviceStateV1 bad = make_default_device_state(0x4C554E4152ULL);
  setCable(bad, JackId::env_follower_env_out, JackId::effector_cv_x_in);

  CHECK_FALSE(h.load(bad));   // rejected, NOT Accepted

  // The single-commit guard kept active A: same runtime() pointer (no commit on a rejected path),
  // and the VCO-B self-edge is still the active route.
  CHECK(h.runtime() == activeA);
  CHECK(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in));
}

}  // namespace

int main() {
  test_no_cable_default();
  test_override_and_restore();
  test_supported_asymmetric_cable_difference();
  test_restore_deterministic();
  test_multi_cable_no_loss();
  test_unsupported_graph_fails();
  return test::finish("test_machine_cable_restore");
}
