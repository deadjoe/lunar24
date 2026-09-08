// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// MachineRuntimeDefinition: the production-owned canonical machine builder.
//
// This is the answer to the "tests hand-bind product fixtures" anti-pattern (@Codex
// 7C2 B′ msg cc5043dd point 1): the product machine owns its OWN scheduling
// contracts, its module table, its fixed internal route set, its ExecutionKind
// disposition, and the resulting SynthRuntime — and the tests consume the machine
// through `definition.runtime()` instead of re-wiring the registry by hand.
//
// The fixed route set is a DATA TABLE derived from the registry manifest's
// `requiredFixedRoutes` (28 routes, @Codex correction 1), NOT a hardcoded edge list:
// the definition owns its `fixedEdges_[]` member and fills it from that table, so
// every compiler-edge is traceable to an authored manifest route and the
// 28-route / 11-unique-edge / category accounting is itself auditable.
//
// Lifetime invariant: the SynthRuntime stores POINTERS to this definition's
// contracts_ / modules_ / fixedEdges_ / routes arrays for the runtime's WHOLE
// lifetime (machine_runtime.h stores modules_ + each mods[i].contract as caller-owned
// pointers). The definition is therefore NON-COPYABLE and NON-MOVABLE and must be
// held at a STABLE address for its whole life: a copied or moved definition would
// leave the runtime pointing at a foreign (or dead) heap/stack location. Tests (and
// the host) construct it in a fixed-scope lvalue and never move it.

#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/machine_runtime.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/state_default.h>  // make_default_device_state (the seed convenience ctor)
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// ---------------------------------------------------------------------------
// Fixed-route data table (@Codex correction 1).
//
// This mirrors the manifest's `requiredFixedRoutes` (28 routes). Each route is a
// module->module OR module->terminal OR terminal->module edge that is FIXED (never
// user-cable cardinality). Only an inter-module edge whose BOTH endpoints are ACTIVE
// compiled modules contributes to a compiler FixedEdge; the rest are recorded for the
// accounting (intra-VCF stages, host/DRY terminals, and declared-deferred provider
// routes) but are NOT compiler edges this slice. The unique edge set is derived from
// this table (mixer->vcf_l + mixer->vcf_r fold to one `mixer->vcf`), so the definition
// owns `fixedEdges_[]` rather than a hand-duplicated constexpr list.
// ---------------------------------------------------------------------------
enum class FixedRouteCategory : std::uint8_t {
  kInterModule,    // both endpoints ACTIVE modules -> contributes exactly one FixedEdge
  kIntraVcfStage,  // vcf -> distortion: handled inside the VCF path (not a compiled module)
  kHostTerminal,   // host injection (ext_audio -> mixer)
  kDryTerminal,    // DRY/WET host output terminal (vco -> out.dry, eff -> out.wet)
  kDeferred,       // endpoint(s) deferred this slice (voice->drone, dist->eff, piezzo->preamp)
};

// Sentinel ModuleId for a route endpoint that is NOT a compiled module (a host/DRY
// terminal, an intra-VCF stage, or an external input). Out of the real module range
// (kModuleCount=21); never a registry identifier.
inline constexpr ModuleId kFixedRouteNoModule =
    static_cast<ModuleId>(std::numeric_limits<std::uint32_t>::max());

struct FixedRoute {
  const char* stableId;       // manifest id, e.g. "fixed.vco_a_to_mixer"
  FixedRouteCategory category;
  ModuleId source;            // kFixedRouteNoModule when the source is not a compiled module
  ModuleId sink;              // kFixedRouteNoModule when the sink is not a compiled module
  const char* edgeName;       // canonical FixedEdge name for kInterModule routes ("" otherwise)
};

inline constexpr FixedRoute kFixedRoutes[] = {
  // 6 inter-module drone -> mixer (ACTIVE).
  {"fixed.drone_1_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_1,     ModuleId::mixer,      "fixed.drone_1_to_mixer"},
  {"fixed.drone_2_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_2,     ModuleId::mixer,      "fixed.drone_2_to_mixer"},
  {"fixed.drone_3_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_3,     ModuleId::mixer,      "fixed.drone_3_to_mixer"},
  {"fixed.drone_4_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_4,     ModuleId::mixer,      "fixed.drone_4_to_mixer"},
  {"fixed.drone_5_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_5,     ModuleId::mixer,      "fixed.drone_5_to_mixer"},
  {"fixed.drone_6_to_mixer",   FixedRouteCategory::kInterModule,  ModuleId::drone_6,     ModuleId::mixer,      "fixed.drone_6_to_mixer"},
  // 1 host-terminal injection (ext_in -> mixer); DRY/WET are host outputs.
  {"fixed.ext_audio_to_mixer", FixedRouteCategory::kHostTerminal, kFixedRouteNoModule,  ModuleId::mixer,      ""},
  // 2 inter-module VCO -> mixer.
  {"fixed.vco_a_to_mixer",     FixedRouteCategory::kInterModule,  ModuleId::vco_a,       ModuleId::mixer,      "fixed.vco_a_to_mixer"},
  {"fixed.vco_b_to_mixer",     FixedRouteCategory::kInterModule,  ModuleId::vco_b,       ModuleId::mixer,      "fixed.vco_b_to_mixer"},
  // 1 inter-module preamp -> mixer.
  {"fixed.preamp_to_mixer",    FixedRouteCategory::kInterModule,  ModuleId::preamp,      ModuleId::mixer,      "fixed.preamp_to_mixer"},
  // 2 DRY host terminals (vco.dry_out -> out.dry).
  {"fixed.vco_a_to_dry_a",     FixedRouteCategory::kDryTerminal,  ModuleId::vco_a,       kFixedRouteNoModule,  ""},
  {"fixed.vco_b_to_dry_b",     FixedRouteCategory::kDryTerminal,  ModuleId::vco_b,       kFixedRouteNoModule,  ""},
  // 2 inter-module mixer -> vcf (fold to ONE `mixer->vcf` compiler edge).
  {"fixed.mixer_to_vcf_l",     FixedRouteCategory::kInterModule,  ModuleId::mixer,       ModuleId::vcf,        "fixed.mixer_to_vcf"},
  {"fixed.mixer_to_vcf_r",     FixedRouteCategory::kInterModule,  ModuleId::mixer,       ModuleId::vcf,        "fixed.mixer_to_vcf"},
  // 2 intra-VCF stages (vcf -> distortion; distortion is not a separate compiled module).
  {"fixed.vcf_l_to_dist_l",    FixedRouteCategory::kIntraVcfStage, ModuleId::vcf,        kFixedRouteNoModule,  ""},
  {"fixed.vcf_r_to_dist_r",    FixedRouteCategory::kIntraVcfStage, ModuleId::vcf,        kFixedRouteNoModule,  ""},
  // 4 declared-deferred: dist -> eff, eff -> wet (deferred effector/DRY terminal).
  {"fixed.dist_l_to_eff_l",    FixedRouteCategory::kDeferred,     kFixedRouteNoModule,  ModuleId::effector,   ""},
  {"fixed.dist_r_to_eff_r",    FixedRouteCategory::kDeferred,     kFixedRouteNoModule,  ModuleId::effector,   ""},
  {"fixed.eff_l_to_wet_l",     FixedRouteCategory::kDeferred,     ModuleId::effector,   kFixedRouteNoModule,  ""},
  {"fixed.eff_r_to_wet_r",     FixedRouteCategory::kDeferred,     ModuleId::effector,   kFixedRouteNoModule,  ""},
  // 1 declared-deferred: piezzo (external input) -> preamp.
  {"fixed.piezzo_to_preamp",   FixedRouteCategory::kDeferred,     kFixedRouteNoModule,  ModuleId::preamp,     ""},
  // 1 inter-module preamp -> env_follower (CONTROL, fixed internal route).
  {"fixed.preamp_to_env_follower", FixedRouteCategory::kInterModule, ModuleId::preamp,  ModuleId::env_follower, "fixed.preamp_to_env_follower"},
  // 6 declared-deferred: voices.voiceN_gate -> drone_N.gate.
  {"fixed.voice_1_gate_to_drone_1", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_1,    ""},
  {"fixed.voice_2_gate_to_drone_2", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_2,    ""},
  {"fixed.voice_3_gate_to_drone_3", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_3,    ""},
  {"fixed.voice_4_gate_to_drone_4", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_4,    ""},
  {"fixed.voice_5_gate_to_drone_5", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_5,    ""},
  {"fixed.voice_6_gate_to_drone_6", FixedRouteCategory::kDeferred, ModuleId::voices,    ModuleId::drone_6,    ""},
};
inline constexpr std::uint32_t kFixedRouteCount =
    static_cast<std::uint32_t>(sizeof(kFixedRoutes) / sizeof(kFixedRoutes[0]));
static_assert(kFixedRouteCount == 28,
              "the fixed-route data table must enumerate all 28 manifest requiredFixedRoutes");

// Number of UNIQUE inter-module fixed compiler edges derived from kFixedRoutes
// (deduped on (source, sink): mixer->vcf_l and mixer->vcf_r fold to one mixer->vcf).
inline constexpr std::uint32_t deriveUniqueInterModuleEdgeCount() {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < kFixedRouteCount; ++i) {
    if (kFixedRoutes[i].category != FixedRouteCategory::kInterModule) continue;
    bool dup = false;
    for (std::uint32_t j = 0; j < i; ++j) {
      if (kFixedRoutes[j].category != FixedRouteCategory::kInterModule) continue;
      if (kFixedRoutes[j].source == kFixedRoutes[i].source &&
          kFixedRoutes[j].sink == kFixedRoutes[i].sink) {
        dup = true;
        break;
      }
    }
    if (!dup) ++n;
  }
  return n;
}
inline constexpr std::uint32_t kCanonicalFixedEdgeCount = deriveUniqueInterModuleEdgeCount();
static_assert(kCanonicalFixedEdgeCount == 11,
              "the 28-route table must fold to exactly 11 unique inter-module compiler edges");

// ---------------------------------------------------------------------------
// Execution-kind disposition for the 21-module registry inventory (@Codex point 2).
//
// Every module has a UNIQUE disposition — there is no "unlisted" module. Two groups:
//   * ACTIVE — bound to a real dispatch kind (the unified per-module executor steps it).
//       The six drones are six INDEPENDENT kDroneBank slots (no ExecutionKind dedup,
//       @Codex 7C2 msg 4e600057).
//   * DEFERRED / control-only — bound to kUnsupported. A strict plan that PULLS one of
//       these into a compiled region (via a patched cable or a fixed edge) fail-closes
//       `unsupported_module`; left isolated (no edge), it is simply not a region member.
// ---------------------------------------------------------------------------
struct MachineDispositionEntry {
  ModuleId id;
  ExecutionKind kind;
};

inline constexpr MachineDispositionEntry kMachineDisposition[] = {
  {ModuleId::vco_a,         ExecutionKind::kVcoA},
  {ModuleId::vco_b,         ExecutionKind::kVcoB},
  {ModuleId::vcf,           ExecutionKind::kVcfPath},
  {ModuleId::mixer,         ExecutionKind::kMixer},
  {ModuleId::preamp,        ExecutionKind::kPreamp},
  {ModuleId::env_follower,  ExecutionKind::kEnvFollower},
  {ModuleId::drone_1,       ExecutionKind::kDroneBank},
  {ModuleId::drone_2,       ExecutionKind::kDroneBank},
  {ModuleId::drone_3,       ExecutionKind::kDroneBank},
  {ModuleId::drone_4,       ExecutionKind::kDroneBank},
  {ModuleId::drone_5,       ExecutionKind::kDroneBank},
  {ModuleId::drone_6,       ExecutionKind::kDroneBank},
  // The six control sources are NOW executed (GH#11 FIXED-CANDIDATE, D1/D2/D4),
  // each as an always-execute source in the compiled plan. GH#12 keyboard product
  // owner: `keyboard` is ALSO now a real executed control source (kKeyboard) — it
  // consumes canonical note ControlEvents and publishes note CV + gate. `effector`/
  // `voices` stay declared-deferred (kUnsupported) — no runtime instance yet.
  {ModuleId::keyboard,      ExecutionKind::kKeyboard},
  {ModuleId::envelope_a,    ExecutionKind::kEnvelope},
  {ModuleId::envelope_b,    ExecutionKind::kEnvelope},
  {ModuleId::lfo_a,         ExecutionKind::kLfo},
  {ModuleId::lfo_b,         ExecutionKind::kLfo},
  {ModuleId::joystick,      ExecutionKind::kJoystick},
  {ModuleId::sequencer,     ExecutionKind::kSequencer},
  {ModuleId::effector,      ExecutionKind::kUnsupported},
  {ModuleId::voices,        ExecutionKind::kUnsupported},
};
inline constexpr std::uint32_t kMachineDispositionCount =
    static_cast<std::uint32_t>(sizeof(kMachineDisposition) / sizeof(kMachineDisposition[0]));
static_assert(kMachineDispositionCount == lunar24::registry::kModuleCount,
              "every registry module carries exactly one disposition");

// VCO base-frequency software provisional reference (@Codex task#78 VCO ruling, f0336e8d): the
// canonical MachineRuntimeDefinition seeds BOTH VCOs' base frequency from a SINGLE centrally-named
// 440Hz provisional value via setVcoBaseHz(), placed BEFORE parameter apply, shared by the default-
// prepare and state-restore paths (both flow through this same state ctor). This value is NOT a
// hardware measurement, NOT a new DeviceState parameter, and does NOT change the 169 disposition
// classification; a later official/measured reference supersedes it.
inline constexpr double kVcoBaseHzProvisional = 440.0;

// ---------------------------------------------------------------------------
// Normalized-route disposition table (@Codex correction 2).
//
// All 6 normalized registry routes get a UNIQUE disposition, keyed by the stable
// RouteId (never by array position in kNormalizedRoutes[]). GH#12 keyboard product owner:
// the four keyboard/EG routes are now ACTIVE (keyboard_v_oct_out -> VCO A/B v_oct_in and
// keyboard_gate_left_main_out -> EG A/B gate_in), so the PatchGraph consumes the keyboard
// note CV + gate. The VCO-A->VCO-B normalised edge (route.vco_b_vco_out_to_cv_in, source =
// vco_a.dry_out) remains active; route.vcf_cv_l_to_cv_r is an intra-VCF fallback (not a
// compilable edge).
// ---------------------------------------------------------------------------
enum class RouteDisposition : std::uint8_t { kActive, kDeferred, kIntraVcfFallback };

struct NormalizedRouteDisposition {
  RouteId id;
  RouteDisposition disposition;
};

inline constexpr NormalizedRouteDisposition kRouteDisposition[] = {
  {RouteId::route_keyboard_v_oct_to_vco,  RouteDisposition::kActive},
  {RouteId::route_keyboard_gate_to_eg,    RouteDisposition::kActive},
  {RouteId::route_vcf_cv_l_to_cv_r,       RouteDisposition::kIntraVcfFallback},
  {RouteId::route_keyboard_v_oct_to_vco_b, RouteDisposition::kActive},
  {RouteId::route_vco_b_vco_out_to_cv_in, RouteDisposition::kActive},
  {RouteId::route_keyboard_gate_to_eg_b,  RouteDisposition::kActive},
};
inline constexpr std::uint32_t kRouteDispositionCount =
    static_cast<std::uint32_t>(sizeof(kRouteDisposition) / sizeof(kRouteDisposition[0]));
static_assert(kRouteDispositionCount == lunar24::registry::kRouteCount,
              "every normalized route carries exactly one disposition");

inline constexpr std::uint32_t countActiveRoutes() {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < kRouteDispositionCount; ++i)
    if (kRouteDisposition[i].disposition == RouteDisposition::kActive) ++n;
  return n;
}

// Look up a NormalizedRoute by its stable RouteId (the anti-index fix: never assume the
// route sits at kNormalizedRoutes[i] for a numeric id).
inline constexpr NormalizedRoute lookupRoute(RouteId id) {
  for (const auto& r : lunar24::registry::kNormalizedRoutes) {
    if (r.id == id) return r;
  }
  return NormalizedRoute{};
}

inline constexpr NormalizedRoute kActiveRoutes[] = {
    // Keep the acyclic VCO-A->VCO-B edge FIRST so the static_assert below pins index 0.
    lookupRoute(RouteId::route_vco_b_vco_out_to_cv_in),
    // GH#12 keyboard product owner: the four keyboard normalized routes are active, so the
    // PatchGraph consumes the keyboard note CV (keyboard_v_oct_out -> VCO A/B v_oct_in) and
    // the engaged gate (keyboard_gate_left_main_out -> EG A/B gate_in). Order within the
    // array is immaterial to compile_graph (it topo-sorts); the A->B edge stays at index 0.
    lookupRoute(RouteId::route_keyboard_v_oct_to_vco),
    lookupRoute(RouteId::route_keyboard_gate_to_eg),
    lookupRoute(RouteId::route_keyboard_v_oct_to_vco_b),
    lookupRoute(RouteId::route_keyboard_gate_to_eg_b),
};
inline constexpr std::uint32_t kActiveRouteCount =
    static_cast<std::uint32_t>(sizeof(kActiveRoutes) / sizeof(kActiveRoutes[0]));
static_assert(countActiveRoutes() == kActiveRouteCount,
              "the active route list must be exactly the kActive routes in the disposition table");
static_assert(kActiveRoutes[0].sourceJack == lunar24::registry::JackId::vco_a_dry_out &&
                  kActiveRoutes[0].sinkJack == lunar24::registry::JackId::vco_b_cv_in,
              "the active route must be the acyclic VCO-A->VCO-B normalised edge (route.vco_b_vco_out_to_cv_in)");

// ---------------------------------------------------------------------------
// AUDIT DISPOSITION (item 8, @Codex eaaf08cc; RESOLVED by task #83 / GH #18): normalized-route
// source conflict. The pre-fix registry had route.vco_b_vco_out_to_cv_in sourced from
// vco_b.vco_out (a VCO-B SELF-edge: B sinks its own VCO output into its generic CV input). That
// self-edge was the documented root cause of the default DRY B DC-stall (N-1): with the linear
// generic-CV transfer p *= (1 + cv*cvAmt) and default cvAmt, a self-fed output reaches the
// zero-pitch transition and latches. Per @Codex's adjudicated direction A (4fe298c8) the ONLY
// authorized change is correcting the SOURCE of RouteId 4 to the EXISTING published VCO-A
// oscillator signal (vco_a.dry_out) — an ACYCLIC A->B edge, so vco_b.cv_in reads A's LIVE value the
// same frame. The stable ID (route.vco_b_vco_out_to_cv_in) and RouteId (4) are PRESERVED as
// legacy. We never change default cvAmt / lin_exp / baseHz, never impose a frequency floor, never
// add a jack, never reassign B's OSC public output owner (vco_b.vco_out). The generator +
// manifest + generated registry headers are updated to the same single fact; the product
// integration (a playable B voice) is the P4 scope, not this fix.
//   * SOURCE (fixed): vco_a.dry_out -> vco_b.cv_in   [acyclic; B reads A live same-frame]
//   * LEGACY: stable_id "route.vco_b_vco_out_to_cv_in", RouteId 4, B OSC owner vco_b.vco_out unchanged.
//   * STATUS: source corrected by task #83 / GH #18; description/registry/manifest in agreement.
// ---------------------------------------------------------------------------

// Forward declaration of the state-aware candidate-builder result
// (machine_candidate.h). Friend-declared below so the validated builder is the single public
// path from a DeviceStateV1 to a MachineRuntimeDefinition.
struct MachineCandidateResult;

// ---------------------------------------------------------------------------
// MachineRuntimeDefinition
// ---------------------------------------------------------------------------
class MachineRuntimeDefinition {
 public:
  MachineRuntimeDefinition(const MachineRuntimeDefinition&) = delete;
  MachineRuntimeDefinition& operator=(const MachineRuntimeDefinition&) = delete;
  MachineRuntimeDefinition(MachineRuntimeDefinition&&) = delete;
  MachineRuntimeDefinition& operator=(MachineRuntimeDefinition&&) = delete;

 private:
  // GH#12 9B state-aware constructor (task#76). This is the SINGLE semantic builder, and it is
  // PRIVATE: a DeviceStateV1 is constructible into a machine ONLY through
  // buildMachineRuntimeCandidate (the validated factory, friend below). There is NO public path
  // from an arbitrary state to a MachineRuntimeDefinition, so a caller cannot bypass state
  // validation and mint a "valid" machine from an invalid candidate. The seed convenience
  // constructor (public) delegates here through the always-valid power-on DEFAULT state, so it is
  // not a bypass either.
  //
  // It owns a COPY of the exact DeviceStateV1 candidate, seeds the voice layer from
  // state.identitySeed.seed, binds the canonical tables, and applies the GH#6 VCF->distortion
  // identity/calibration profile from that SAME owned state (never an orphan snapshot). The full
  // candidate is preserved byte-for-byte and readable back through deviceState().
  //
  // Member-init order is deliberate and REQUIRED by the pointer-capture invariant: state_ is
  // declared AFTER contracts_/modules_/fixedEdges_ but BEFORE runtime_, so (a) the tables the
  // runtime points into are already constructed, and (b) the runtime's ctor runs LAST and can be
  // seeded from state.identitySeed.seed. The init-list order matches that declaration order.
  explicit MachineRuntimeDefinition(const DeviceStateV1& state, double sampleRate = 48000.0)
      : state_(state),
        runtime_(lunar24::registry::kJacks, lunar24::registry::kJackCount,
                 kActiveRoutes, kActiveRouteCount,
                 modules_, kMachineDispositionCount, state.identitySeed.seed, sampleRate,
                 fixedEdges_, kCanonicalFixedEdgeCount) {
    // Populate the owned tables BEFORE any rebuild() reads them. The runtime_'s ctor
    // only captured the pointers; compile/rebuild happens in rebuild() below, so the
    // contracts_ / modules_ / fixedEdges_ members must be filled here, from the
    // constexpr disposition + fixed-route tables (single source, no separate member copy).
    buildDisposition_(sampleRate);
    fillFixedEdges_();

    // Bind every module's ExecutionKind (the canonical dispatcher). One slot per
    // compiled ModuleId, no dedup.
    for (std::uint32_t i = 0; i < kMachineDispositionCount; ++i) {
      runtime_.bindExecutionKind(kMachineDisposition[i].id, kMachineDisposition[i].kind);
    }

    // Canonical Jack bindings (@Codex correction 4): the product machine binds the real
    // registered jacks that carry V/OCT, VCF CV, preamp/env-follower, and the classic
    // drone ENV/CV-MOD cohort — so the source bank driving these controls is the same
    // identity the rest of the product (and the tests) read.
    runtime_.setVoctBindings(lunar24::registry::JackId::vco_a_v_oct_in,
                             lunar24::registry::JackId::vco_b_v_oct_in);
    // Generic CV + VCO output bindings (@Codex correction 4): each VCO's generic cv_in is
    // a second, INDEPENDENT CV/transfer path from its V/OCT. The active A->B normalised route
    // (vco_a.dry_out -> vco_b.cv_in, task #83 / GH #18) makes vco_b.cv_in an ACYCLIC edge from
    // vco_a.dry_out: the VCO-B slot resolves it through setCvInput(held mode) reading A's LIVE
    // published value the same frame, and publishes the real vco_b.vco_out. The lin/exp mode
    // is a runtime decision (explicit below; tests choose it), never a hardcoded law.
    runtime_.setVcoCvBindings(lunar24::registry::JackId::vco_a_cv_in,
                              lunar24::registry::JackId::vco_b_cv_in);
    runtime_.setVcoOutBindings(lunar24::registry::JackId::vco_a_dry_out,
                               lunar24::registry::JackId::vco_b_vco_out);
    // NOTE (item 1, @Codex eaaf08cc): the A/B generic-CV lin/exp mode is deliberately
    // NOT pinned here. Mode is a runtime/test/upper-layer decision — it is NOT canonical
    // hardware truth (the adjudicated ruling). The canonical builder leaves it at the
    // runtime's provisional safe default (kExponential) and the canonical oracles that
    // rely on the generic CV / the (now-acyclic) A->B route must EXPLICITLY select the mode via
    // setVcoControlModes; never a hardcoded law in the builder.
    runtime_.setVcfCvBindings(lunar24::registry::JackId::vcf_cv_l_in,
                              lunar24::registry::JackId::vcf_cv_r_in);
    runtime_.setPreampExtIn(lunar24::registry::JackId::preamp_ext_source_in);
    runtime_.setEnvFolOut(lunar24::registry::JackId::env_follower_env_out);
    // Classic DRONE ENV/CV-MOD cohort (order 0..3 == drone 1/2/4/5).
    runtime_.setDroneEnvOutBindings(lunar24::registry::JackId::drone_1_env_out,
                                    lunar24::registry::JackId::drone_2_env_out,
                                    lunar24::registry::JackId::drone_4_env_out,
                                    lunar24::registry::JackId::drone_5_env_out);
    runtime_.setDroneCvModInBindings(lunar24::registry::JackId::drone_1_cv_mod_in,
                                     lunar24::registry::JackId::drone_2_cv_mod_in,
                                     lunar24::registry::JackId::drone_4_cv_mod_in,
                                     lunar24::registry::JackId::drone_5_cv_mod_in);

    // GH#11 FIXED-CANDIDATE (D1/D2): the six control sources are NOW real DSP
    // instances, so the owning definition binds their REGISTRY jacks (the same identity
    // the rest of the product reads) and always-executes them. Env A/B resolve their
    // real gate_in and publish env_out + vca_cv_out; LFO A/B publish cv_out; joystick
    // publishes x_out/y_out; sequencer consumes ext_clock_in (sink latched) and
    // publishes cv_out + gate_out + clock_out (the CLOCK OUT -10/+10 one-sample virtual-volts
    // pulse (@Codex 7C3), rail confirmed bipolar, width provisional, derived from the SAME
    // pulserRising as the discrete clockOutRising() — never a second phase/latch).
    runtime_.setEnvelopeBindings(lunar24::registry::JackId::envelope_a_gate_in,
                                 lunar24::registry::JackId::envelope_a_env_out,
                                 lunar24::registry::JackId::envelope_a_vca_cv_out,
                                 lunar24::registry::JackId::envelope_b_gate_in,
                                 lunar24::registry::JackId::envelope_b_env_out,
                                 lunar24::registry::JackId::envelope_b_vca_cv_out);
    runtime_.setLfoBindings(lunar24::registry::JackId::lfo_a_cv_out,
                            lunar24::registry::JackId::lfo_b_cv_out);
    runtime_.setJoystickBindings(lunar24::registry::JackId::joystick_x_out,
                                 lunar24::registry::JackId::joystick_y_out);
    runtime_.setSequencerBindings(lunar24::registry::JackId::sequencer_ext_clock_in,
                                  lunar24::registry::JackId::sequencer_cv_out,
                                  lunar24::registry::JackId::sequencer_gate_out,
                                  lunar24::registry::JackId::sequencer_clock_out);
    // GH#12 keyboard product owner: the keyboard publishes the registered note-CV and gate
    // output jacks. VCO A/B and EG A/B consume them through the four now-active keyboard
    // routes (route_keyboard_v_oct_to_vco / _b, route_keyboard_gate_to_eg / _b).
    // task#101: all FOUR registered keyboard outputs are bound (v_oct / gate_left_main /
    // gate_right / pressure_out). The normalized routes keep their existing endpoints — this
    // adds no route and does not touch VCO-B's default source.
    runtime_.setKeyboardBindings(lunar24::registry::JackId::keyboard_v_oct_out,
                                 lunar24::registry::JackId::keyboard_gate_left_main_out,
                                 lunar24::registry::JackId::keyboard_gate_right_out,
                                 lunar24::registry::JackId::keyboard_pressure_out);
    // Always-execute the seven sources so an unwired LFO/EG-SELF-GEN/PULSER/keyboard still
    // runs once per sample (compile_graph force-includes them -> isolated acyclic singleton
    // regions). The keyboard is always-executed so its portamento glide advances every
    // sample even if a user cable overrides its output route. ModuleId{0} is vco_a (a REAL
    // module), so this list is NOT null-terminated — the count is the authoritative bound.
    {
      const ModuleId alwaysExec[7] = {ModuleId::envelope_a, ModuleId::envelope_b,
                                      ModuleId::lfo_a, ModuleId::lfo_b,
                                      ModuleId::joystick, ModuleId::sequencer,
                                      ModuleId::keyboard};
      runtime_.setAlwaysExecute(alwaysExec, 7);
    }

    // Canonical strictness on, then build the plan.
    runtime_.setStrictBindings(true);

    // task #80 (GH#12 9D C3): restore the validated device-state USER CABLES into the real
    // PatchGraph BEFORE the final graph rebuild / publish. The active normalized route (the single
    // acyclic VCO-A->VCO-B edge) is carried by the patch_ construction; each restored user cable, by rule,
    // overrides only its own route sink (a derived fact, never stored). We reuse
    // SynthRuntime::connect() but NEVER treat a lone connect()==true as complete: connect() can
    // atomically displace a PRIOR requested cable at a saturated source/sink port. So after placing
    // every requested cable we VERIFY the final user-cable bank exactly equals the requested set and
    // fail the WHOLE candidate (cableRestoreOk_ = false) on any mismatch — never a silent drop or
    // partial success. Fail-closed, mirroring dspApplyOk_; the host single-commit guard keeps the old
    // owner on reject. routeOverridden is NOT a second routing switch here: its coherence against the
    // ACTUAL cable facts was already enforced by check_routes, and the router merely reflects patch_
    // state, so it is not consulted by this restore.
    {
      std::uint32_t requested = 0;
      bool restoreFailed = false;
      for (std::uint32_t i = 0; i < kDevicePatchCapacity; ++i) {
        if (state_.inputCable[i] == 0u) continue;
        // i is the serialized JackId of the sink, NOT a dense index (the id-space is sparse, with
        // holes). PatchGraph::connect() resolves landedness via the descriptor; a hole fails closed.
        const JackId source = state_.cableSource[i];
        const JackId sink = static_cast<JackId>(i);
        if (!runtime_.connect(source, sink)) { restoreFailed = true; break; }
        ++requested;
      }
      if (!restoreFailed) {
        // The final user-cable set must equal the requested set exactly:
        //   * cableCount() == requested  -> no sunk cable and no stray/duplicate cable;
        //   * each requested sink holds exactly ONE user cable reachable from its requested source
        //     -> no wrong-source (mis-)wire, no displaced requested cable on that sink.
        if (runtime_.cableCount() != requested) restoreFailed = true;
        for (std::uint32_t i = 0; !restoreFailed && i < kDevicePatchCapacity; ++i) {
          if (state_.inputCable[i] == 0u) continue;
          const JackId sink = static_cast<JackId>(i);
          if (runtime_.cableCountInto(sink) != 1u ||
              !runtime_.cableConnected(state_.cableSource[i], sink)) {
            restoreFailed = true;
          }
        }
      }
      cableRestoreOk_ = !restoreFailed;
    }

    // GH#6: the one real identity/calibration apply choke, driven from the SAME owned state
    // (never a detached snapshot). It consumes identityModelVersion + identitySeed.seed +
    // calibration; it is fail-closed (a rejected version/trim makes NO change) inside
    // SynthRuntime, so a valid candidate always configures and identityApplied() reports it.
    identityApplied_ = runtime_.configureVcfIdentity(state.identityModelVersion,
                                                     state.identitySeed.seed,
                                                     state.calibration);
    (void)runtime_.rebuild();
    // task #78 (@Codex f0336e8d, re-applied per ruling 44369539): seed BOTH VCO base frequencies
    // from the single centrally-named 440Hz provisional BEFORE parameter apply. This mirrors the
    // default-prepare and state-restore paths (both flow through this same state ctor). It does NOT
    // introduce a DeviceState parameter, does NOT change the 169 disposition classification, and a
    // later official/measured reference number supersedes it.
    runtime_.setVcoBaseHz(kVcoBaseHzProvisional);
    // task #78: after the GH#6 identity/calibration and the final rebuild (order preserved),
    // apply the WHOLE applied_to_DSP parameter set (exactly 169) from the SAME owned state into
    // the freshly-rebuilt DSP. Fail-closed: exactly 169 must apply, else dspApplyOk_ is false and
    // the first failing id/status is retained for the candidate factory to reject whole (it never
    // contributes a partial-success path).
    {
      ParameterId firstFailId = static_cast<ParameterId>(kParameterCount);
      ParameterApplyStatus firstFailStatus = ParameterApplyStatus::applied;
      const bool ok = runtime_.applyDspState(state_, firstFailId, firstFailStatus);
      dspApplyOk_ = ok;
      dspAppliedCount_ = runtime_.dspAppliedCount();
      dspFirstFailId_ = ok ? static_cast<ParameterId>(kParameterCount) : firstFailId;
      dspFirstFailStatus_ = ok ? ParameterApplyStatus::applied : firstFailStatus;
    }
    // GH#12 task#101: restore the keyboard's per-side PERFORMANCE STATE (mode + both sides'
    // behaviour/arp-seq configuration) from the SAME owned state, after the DSP apply so the
    // parsed config lands on the instance that will actually be ticked. This is configuration
    // only — it publishes no note and changes no default output; an unplayed keyboard stays
    // silent. It deliberately does NOT gate the candidate: there is no real failure condition
    // in this apply, so inventing a keyboardApplyOk_ flag (or a new MachineCandidateStatus
    // enumerator with no reachable false) would be a vacuous pass. The non-vacuous evidence is
    // the per-side readback (keyboardMode()/keyboardArpSeqParams()/keyboardBehaviourParams()).
    runtime_.applyKeyboardState(state_);
  }

  // The validated state-aware builder (machine_candidate.h) is the ONLY public path from a
  // DeviceStateV1 to a MachineRuntimeDefinition, so it may invoke the private state ctor above.
  friend MachineCandidateResult buildMachineRuntimeCandidate(const DeviceStateV1& state,
                                                             double sampleRate);

 public:
  // Convenience: build from a startup seed via the power-on DEFAULT state. This is the SAME
  // state-aware builder as the ctor above — there is no separate seed-only truth path; a seed
  // always denotes the power-on default DeviceState (task#75 make_default_device_state), which
  // is what makes "safe boot prepare(seed)" produce a canonical state == that exact default.
  explicit MachineRuntimeDefinition(std::uint64_t seed, double sampleRate = 48000.0)
      : MachineRuntimeDefinition(make_default_device_state(seed), sampleRate) {}

  SynthRuntime& runtime() { return runtime_; }
  const SynthRuntime& runtime() const { return runtime_; }

  SynthRuntime::RebuildStatus status() const { return runtime_.lastRebuildStatus(); }
  // Real validity: a successfully compiled graph, whether freshly built (ok) or a
  // cached no-change rebuild (graph_unchanged). Any rejection/error status is invalid.
  bool valid() const {
    // task #80: a faithful user-cable restore (cableRestoreOk_) is part of a valid candidate. A
    // restore mismatch / capacity loss / wrong-wire is a typed whole-candidate rejection
    // (rejected_graph via the factory), never a silent drop or partial success.
    return cableRestoreOk_ &&
           (status() == SynthRuntime::RebuildStatus::ok ||
            status() == SynthRuntime::RebuildStatus::graph_unchanged);
  }

  // The exact DeviceStateV1 this machine was built from. This is the ONLY canonical-state truth:
  // it is backed by the definition's own owned copy, so a holder reads it without a second
  // snapshot living elsewhere. Never null.
  const DeviceStateV1& deviceState() const { return state_; }
  // Whether the GH#6 identity/calibration profile was actually configured on the VCF->distortion
  // path from this state. A valid candidate always configures it; a false means the ctor's
  // fail-closed path left it off (a degraded candidate the factory rejects). Named precisely:
  // this reports the IDENTITY/calibration apply only, NOT a whole-DeviceState "applied" claim.
  bool identityApplied() const { return identityApplied_; }

  // task #78: whether the whole 169-parameter applied_to_DSP set held on this definition's owned
  // state actually landed on the DSP. true on a complete apply; false if ANY applied_to_DSP id was
  // rejected (the candidate factory then yields rejected_dsp_apply, carrying the first failure).
  // dspAppliedCount() is exactly count_disposition(applied_to_dsp) on success (169) and partial on
  // rejection. dspFirstFailId()/dspFirstFailStatus() give the first failure (sentinel on success).
  // Named precisely: this reports the FULL DeviceState DSP apply, NOT just the identity/calibration.
  bool dspApplyOk() const { return dspApplyOk_; }
  std::uint32_t dspAppliedCount() const { return dspAppliedCount_; }
  ParameterId dspFirstFailId() const { return dspFirstFailId_; }
  ParameterApplyStatus dspFirstFailStatus() const { return dspFirstFailStatus_; }

  // task #80: whether the validated device-state user cables were faithfully restored into the
  // real PatchGraph (the final user-cable bank exactly equals the requested set). false on a
  // restore mismatch / connect failure, surfaced by valid() as a whole-candidate reject
  // (rejected_graph); the host single-commit guard then keeps the old owner. Layered like
  // dspApplyOk(): it reports the PATCH restore apply, not a whole-DeviceState claim.
  bool cableRestoreOk() const { return cableRestoreOk_; }

  std::uint32_t moduleCount() const { return kMachineDispositionCount; }
  std::uint32_t fixedEdgeCount() const { return kCanonicalFixedEdgeCount; }
  std::uint32_t fixedRouteCount() const { return kFixedRouteCount; }
  std::uint32_t activeRouteCount() const { return kActiveRouteCount; }

  // The canonical ExecutionKind a module plays (the OWNING input disposition, from the
  // table the runtime was built from). Returns nullopt for a ModuleId that is NOT in the
  // 21-module registry inventory — which is a genuine "no such module", deliberately
  // DISTINCT from kUnsupported (a real deferred module that IS in the inventory). This
  // closes the masquerade where a typo'd/unknown id used to read as "explicitly
  // unsupported" (@Codex correction 5).
  std::optional<ExecutionKind> kindOf(ModuleId id) const {
    for (std::uint32_t i = 0; i < kMachineDispositionCount; ++i) {
      if (kMachineDisposition[i].id == id) return kMachineDisposition[i].kind;
    }
    return std::nullopt;
  }

  // The owned prepared scheduling contract for a module (audit surface: lets a test
  // confirm the definition owns a real, valid contract for every compiled module).
  // Returns nullptr for an unknown id — never a valid-looking default sentinel contract.
  const ModuleExecutionContract* contractOf(ModuleId id) const {
    for (std::uint32_t i = 0; i < kMachineDispositionCount; ++i) {
      if (modules_[i].id == id) return modules_[i].contract;
    }
    return nullptr;
  }

 private:
  ModuleExecutionContract* findContract_(ModuleId id) {
    for (std::uint32_t i = 0; i < kMachineDispositionCount; ++i) {
      if (kMachineDisposition[i].id == id) return &contracts_[i];
    }
    return nullptr;
  }

  // Real scheduling contracts (@Codex correction 3). Default: cycle-UNSAFE and carrying
  // NO declared per-path delay (a fixed-fed module in a user-formed SCC falls back to a
  // conservative z^-1, per graph_compiler's decide_feedback_delay_cycle). Only the actual
  // SCC members — VCO-B (its own self-loop), env-follower and preamp (which ACCEPT a
  // user-patched return SCC, with NO auto-wire) — get explicit cycle-safe contracts.
  // maxBlockSize / maxResources stay 0 == UNPREPARED/UNSPECIFIED sentinel (no prepare
  // boundary exists yet; @Codex e35b3eca). These limits are PENDING a future host
  // prepare/resource integration (GH#4/#10 dependencies) — they are NOT a GH#11 gap:
  // the six control sources themselves are implemented and consumed at this head.
  void buildDisposition_(double sampleRate) {
    for (std::uint32_t i = 0; i < kMachineDispositionCount; ++i) {
      ModuleExecutionContract& c = contracts_[i];
      c.sampleRate = sampleRate;
      c.maxBlockSize = 0;           // sentinel: unprepared/unspecified
      c.intrinsicLatencySamples = 0;
      c.hasDirectThroughPath = false;
      c.maxResources = 0;           // sentinel: unprepared/unspecified
      c.allowedInCyclicSCC = false; // default: a module is NOT cycle-safe until declared
      c.pathDelayCount = 0;
      modules_[i].id = kMachineDisposition[i].id;
      modules_[i].contract = &contracts_[i];
    }

    // VCO-B internal path cv_in -> vco_out direct/min0, cycle-safe. This is a CAPABILITY, NOT a
    // default: it is only an SCC when a USER explicitly cables vco_b.vco_out back into
    // vco_b.cv_in (the retained B->B feedback-machinery test). The default A->B edge is acyclic,
    // so this contract is dormant there; when compiled inside a cycle the compiler breaks it with
    // a one-sample z^-1 (canDirectThrough -> algebraic).
    if (ModuleExecutionContract* c = findContract_(ModuleId::vco_b)) {
      c->allowedInCyclicSCC = true;
      ModulePathDelay& p = c->pathDelays[0];
      p.inPort = lunar24::registry::JackId::vco_b_cv_in;
      p.outPort = lunar24::registry::JackId::vco_b_vco_out;
      p.minCausalDelaySamples = 0.0;
      p.canDirectThrough = true;
      p.directThroughExactZeroGain = false;
      c->pathDelayCount = 1;
      c->hasDirectThroughPath = true;
    }

    // env-follower: fixed-input sentinel -> env_out direct/min0, cycle-safe — it ACCEPTS a
    // user-patched return SCC (env_out -> preamp.ext_source_in) but the definition does NOT
    // auto-connect it (@Codex ruling a). env_follower.audio_in is a FIXED endpoint (defers
    // to the FixedEndpoint stage, not a patchable JackId), so the shared
    // kFixedEndpointJackSentinel names it; the compiler's own fixed-edge tag is the SAME
    // value, so a user return cable forming cycle re-entry into env-follower can really
    // match this path.
    if (ModuleExecutionContract* c = findContract_(ModuleId::env_follower)) {
      c->allowedInCyclicSCC = true;
      ModulePathDelay& p = c->pathDelays[0];
      p.inPort = kFixedEndpointJackSentinel;
      p.outPort = lunar24::registry::JackId::env_follower_env_out;
      p.minCausalDelaySamples = 0.0;
      p.canDirectThrough = true;
      p.directThroughExactZeroGain = false;
      c->pathDelayCount = 1;
      c->hasDirectThroughPath = true;
    }

    // GH#11 envelope A/B: a user-patched REAL cable into gate_in and the A-published
    // env_out/vca_cv_out (env_out -> preamp.ext_source_in, vca_cv_out -> drone cv, etc.)
    // can form an SCC, so each EG is cycle-safe at min0 direct. Both paths are REAL
    // same-sample transfers (A/R/D/S + HOLD/SELF-GEN are sample-rate side effects, never
    // a multicycle read). The ENV and VCA-CV outputs share the same gate -> two paths.
    for (const auto& eg : {ModuleId::envelope_a, ModuleId::envelope_b}) {
      if (ModuleExecutionContract* c = findContract_(eg)) {
        c->allowedInCyclicSCC = true;
        ModulePathDelay& p0 = c->pathDelays[0];
        p0.inPort = (eg == ModuleId::envelope_a) ? lunar24::registry::JackId::envelope_a_gate_in
                                                 : lunar24::registry::JackId::envelope_b_gate_in;
        p0.outPort = (eg == ModuleId::envelope_a) ? lunar24::registry::JackId::envelope_a_env_out
                                                  : lunar24::registry::JackId::envelope_b_env_out;
        p0.minCausalDelaySamples = 0.0;
        p0.canDirectThrough = true;
        p0.directThroughExactZeroGain = false;
        ModulePathDelay& p1 = c->pathDelays[1];
        p1.inPort = p0.inPort;
        p1.outPort = (eg == ModuleId::envelope_a) ? lunar24::registry::JackId::envelope_a_vca_cv_out
                                                  : lunar24::registry::JackId::envelope_b_vca_cv_out;
        p1.minCausalDelaySamples = 0.0;
        p1.canDirectThrough = true;
        p1.directThroughExactZeroGain = false;
        c->pathDelayCount = 2;
        c->hasDirectThroughPath = true;
      }
    }
    // GH#11 sequencer: ext_clock_in -> cv_out and ext_clock_in -> gate_out are REAL
    // same-sample direct/min0 (the sink_latch edge -> one-step advance -> publish all in
    // one sample), so a user-patched cycle through either output is legitimate and
    // cycle-safe. clock_out is published each sample too (@Codex 7C3) — but into the SOURCE
    // bank (the -10/+10 one-sample virtual-volts pulse), NOT as a path-delay: it is driven by
    // the free-running internal PULSER, independent of ext_clock_in, so at the intra-module
    // causality level it is not a function of the input and carries no direct-through edge to
    // declare (its volts projection never back-derives ext_clock_in's rail/threshold).
    if (ModuleExecutionContract* c = findContract_(ModuleId::sequencer)) {
      c->allowedInCyclicSCC = true;
      ModulePathDelay& p0 = c->pathDelays[0];
      p0.inPort = lunar24::registry::JackId::sequencer_ext_clock_in;
      p0.outPort = lunar24::registry::JackId::sequencer_cv_out;
      p0.minCausalDelaySamples = 0.0;
      p0.canDirectThrough = true;
      p0.directThroughExactZeroGain = false;
      ModulePathDelay& p1 = c->pathDelays[1];
      p1.inPort = lunar24::registry::JackId::sequencer_ext_clock_in;
      p1.outPort = lunar24::registry::JackId::sequencer_gate_out;
      p1.minCausalDelaySamples = 0.0;
      p1.canDirectThrough = true;
      p1.directThroughExactZeroGain = false;
      c->pathDelayCount = 2;
      c->hasDirectThroughPath = true;
    }

    // preamp: cycle-admission only (ACCEPTS the user return SCC) with NO invented internal
    // path and NO auto-wire. It has no patchable fixed-in->fixed-out path of its own to
    // declare, so pathCount stays 0 (@Codex ruling a + 16b770b0).
    if (ModuleExecutionContract* c = findContract_(ModuleId::preamp)) {
      c->allowedInCyclicSCC = true;
    }
  }

  // Fill the owned fixedEdges_[] from the 28-route data table (@Codex correction 1). Only
  // inter-module routes whose endpoints are BOTH ACTIVE compiled modules become compiler
  // edges; mixer->vcf_l and mixer->vcf_r fold to one, so the count is exactly
  // kCanonicalFixedEdgeCount (11).
  void fillFixedEdges_() {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < kFixedRouteCount; ++i) {
      if (kFixedRoutes[i].category != FixedRouteCategory::kInterModule) continue;
      if (kFixedRoutes[i].source == kFixedRouteNoModule ||
          kFixedRoutes[i].sink == kFixedRouteNoModule) {
        continue; // a host/DRY terminal is not a compiled edge (defensive; inter-module never sees one)
      }
      bool dup = false;
      for (std::uint32_t j = 0; j < n; ++j) {
        if (fixedEdges_[j].sourceModule == kFixedRoutes[i].source &&
            fixedEdges_[j].sinkModule == kFixedRoutes[i].sink) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      fixedEdges_[n].sourceModule = kFixedRoutes[i].source;
      fixedEdges_[n].sinkModule = kFixedRoutes[i].sink;
      fixedEdges_[n].name = kFixedRoutes[i].edgeName;
      ++n;
    }
  }

  // Owned, stable-address storage (non-movable: see the class comment).
  ModuleExecutionContract contracts_[kMachineDispositionCount];
  GraphModule modules_[kMachineDispositionCount];
  FixedEdge fixedEdges_[kCanonicalFixedEdgeCount];

  // The exact DeviceStateV1 this machine was built from. Must be declared BEFORE runtime_: the
  // runtime's ctor is seeded from state.identitySeed.seed, and the whole class is non-movable so
  // this owned copy is the single canonical-state truth for the definition's whole life.
  DeviceStateV1 state_;

  // Whether the GH#6 identity/calibration profile was configured on the VCF->distortion path.
  bool identityApplied_ = false;
  // task #80: faithful user-cable restore verdict (the candidate-builder gate for the patch).
  bool cableRestoreOk_ = true;
  // task #78 full-apply verdict (the candidate-builder gate + first-failure id/status).
  bool dspApplyOk_ = false;
  std::uint32_t dspAppliedCount_ = 0;
  ParameterId dspFirstFailId_ = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus dspFirstFailStatus_ = ParameterApplyStatus::applied;

  // The owning executor. Declared AFTER the arrays it points into so the init-list is
  // well-formed; its ctor only stores the addresses (compile happens in rebuild()).
  SynthRuntime runtime_;
};

}  // namespace lunar24::core
