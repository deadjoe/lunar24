// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P0 tests for the generated machine registry. Verifies the constexpr descriptor
// arrays are internally consistent, every id it references exists, every
// parameter range/journal is valid, every route endpoint resolves, and every
// descriptor carries evidence + a valid status. Also locks the frozen P0 counts.

#include "mini_test.h"

#include <string_view>

#include <lunar24/core/device_state.h>
#include <lunar24/registry.hpp>

namespace core = lunar24::core;
namespace reg = lunar24::registry;

static int find_jack(core::JackId id) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

static int find_module(core::ModuleId id) {
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    if (reg::kModules[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

static bool owner_exists(std::string_view owner) {
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    if (reg::kModules[i].stable_id == owner) return true;
  }
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    if (reg::kPrograms[i].stable_id == owner) return true;
  }
  return false;
}

static bool valid_status(core::EvidenceStatus s) {
  return s == core::EvidenceStatus::confirmed ||
         s == core::EvidenceStatus::unverified ||
         s == core::EvidenceStatus::provisional;
}

static bool valid_fe(const core::FieldEvidence& fe) {
  return valid_status(fe.nominalRange) && valid_status(fe.toleratedRange) &&
         valid_status(fe.threshold) && valid_status(fe.saturation) &&
         valid_status(fe.transfer) && valid_status(fe.signalType) &&
         valid_status(fe.polarity) && valid_status(fe.coupling);
}

static bool valid_pfe(const core::ParameterFieldEvidence& pfe) {
  return valid_status(pfe.range) && valid_status(pfe.unit) &&
         valid_status(pfe.initial) && valid_status(pfe.step) &&
         valid_status(pfe.smoothing) && valid_status(pfe.persistence);
}

static bool valid_so(core::SelfOscillating s) {
  return s == core::SelfOscillating::unknown || s == core::SelfOscillating::no ||
         s == core::SelfOscillating::yes;
}

static bool valid_prog_fe(const core::ProgramFieldEvidence& pfe) {
  return valid_status(pfe.family) && valid_status(pfe.selfOscillating);
}

// unknown-enum biconditional (Codex 587f5e72): value==unknown ⇔ evidence==unverified.
// Forward: an `unknown` value is never a confirmed/provisional fact. Reverse: a concrete
// value (audio/cv/gate/clock/unipolar/bipolar/ac/dc/no/yes, or a family) can carry only
// confirmed/provisional provenance — never `unverified`, which would present a guess as a
// fact. Both directions are enforced, not just unknown→unverified.
static bool unknown_evidence_consistent(core::SignalType v, core::EvidenceStatus ev) {
  return v == core::SignalType::unknown ? ev == core::EvidenceStatus::unverified
                                        : ev != core::EvidenceStatus::unverified;
}
static bool unknown_evidence_consistent(core::Polarity v, core::EvidenceStatus ev) {
  return v == core::Polarity::unknown ? ev == core::EvidenceStatus::unverified
                                      : ev != core::EvidenceStatus::unverified;
}
static bool unknown_evidence_consistent(core::Coupling v, core::EvidenceStatus ev) {
  return v == core::Coupling::unknown ? ev == core::EvidenceStatus::unverified
                                      : ev != core::EvidenceStatus::unverified;
}
static bool unknown_evidence_consistent(core::SelfOscillating v, core::EvidenceStatus ev) {
  return v == core::SelfOscillating::unknown ? ev == core::EvidenceStatus::unverified
                                             : ev != core::EvidenceStatus::unverified;
}
static bool unknown_evidence_consistent(std::string_view family, core::EvidenceStatus ev) {
  return family == "unknown" ? ev == core::EvidenceStatus::unverified
                             : ev != core::EvidenceStatus::unverified;
}

static void frozen_counts() {
  // Locked P0 baseline — the audit target. These are the actual vertical-slice
  // counts in spec/machine/lunar24.json at the P0 lock.
  CHECK_EQ(core::kModuleCount, 18u);     // Phase B DRONE 4 classic voice slice (Codex msg 483a9dc5):
                                         // +1 module (drone_4 id 17, source, 19 params/3 jacks).
                                         // Prior: DRONE 2 classic voice slice (Codex msg 3bc2111c)
                                         // drone_2 id 16, DRONE 1 classic voice slice (Codex msg
                                         // 28e00d92) drone_1 id 15, drone-voices module closure (Codex
                                         // msg c212dcfb) voices id 14 (performance, zero params/jacks),
                                         // dual-effector slice (Codex msg 1a47b5d0) effector
                                         // id 13, voice-mixer slice (Codex msg 920fa79b) mixer id 12,
                                         // 5-step sequencer slice (Codex msg 6bedef35) sequencer id 11,
                                         // preamp/env_follower (Codex ebd65910) id 9/10, joystick id 8,
                                         // lfo_a/lfo_b, vco_a/vco_b/vcf/keyboard/envelope_a/
                                         // envelope_b.
  CHECK_EQ(core::kParameterCount, 191u); // Phase B DRONE 4 classic voice slice (Codex msg 483a9dc5):
                                         // +19 params (drone_4.tune_1..5 / mute_1..5 / mod_1..5 /
                                         // volt / att / rls continuous + gate_hold selector,
                                         // ids 239-257, software-normalized placeholders). Prior:
                                         // DRONE 2 classic voice slice (Codex msg 3bc2111c) +19
                                         // (ids 220-238), DRONE 1 classic voice slice (Codex msg
                                         // 28e00d92) +19 (ids 201-219), dual-effector slice +8
                                         // (ids 193-200), voice-mixer slice +20
                                         // (mixer.ch1..ch10 so{pan,vol}, ids 173-192). Modules now
                                         // carry 185, programs 6. Left as honest gaps the
                                         // drone_3/5/6 voice params + 12 keyboard
                                         // complex params (seq_steps, quantise_scale_editor,
                                         // plate_tune, pushbutton_value, preset_a..d, arp_clock,
                                         // seq_clock, arp_rhythm, seq_rhythm + remaining module
                                         // ranges)
  CHECK_EQ(core::kJackCount, 51u);       // Phase B DRONE 4 classic voice slice (Codex msg 483a9dc5):
                                         // +3 patchable jacks (drone_4.cv_mod_in/.gate_in/.env_out,
                                         // ids 49-51; for env_out the nominalRange + polarity stay
                                         // unverified/unknown because manual L157 "ENV VOICES
                                         // 1,2,3,6,7,8" does NOT list 4/5, so the DRONE 1/2
                                         // -10..+10V/bipolar confirmed is not copied here).
                                         // Prior: DRONE 2 classic voice slice (Codex msg 3bc2111c)
                                         // +3 (drone_2.cv_mod_in/.gate_in/.env_out, ids 46-48),
                                         // DRONE 1 classic voice slice (Codex msg 28e00d92) +3
                                         // (drone_1.cv_mod_in/.gate_in/.env_out, ids 43-45),
                                         // dual-effector slice +3 CV inputs (ids 40-42, -10..+10V
                                         // CV/bipolar from L1164).
                                         // Prior: the
                                         // 5-step sequencer slice +4 (sequencer.ext_clock_in/
                                         // clock_out clock, cv_out 0..+5V CV/unipolar L159, gate_out
                                         // 0..+10V gate/unipolar L160). Prior phases added
                                         // preamp.ext_source_in, env_follower.env_out/gate_out,
                                         // joystick x_out/y_out, lfo_a/lfo_b cv_out, envelope_b
                                         // gate_in/env_out/vca_cv_out, vco_a/vco_b wave_out+pwm_in,
                                         // keyboard pressure_out+reset_in, envelope_a vca_cv_out and
                                         // dropped bad jack vcf.audio_in
  // Program identity layer landed (Codex 7a6467cc slice #57): 39 program
  // identities (cathedral.1 + magic.1 keep their params; the other 37 are
  // identity-only, paramCount==0, honest gaps pending Phase B). kParameterCount
  // counts modules(87) + programs(6) = 93 — the 37 additions add no params.
  CHECK_EQ(core::kProgramCount, 39u);
  CHECK_EQ(core::kRouteCount, 6u);       // Phase B route slice: +2 normalized routes whose endpoints
                                         // already existed + route.keyboard_gate_to_eg_b (Codex msg
                                         // c4e6c0ff) once envelope_b landed
                                         // (route.keyboard_v_oct_to_vco_b, route.vco_b_vco_out_to_cv_in,
                                         // route.keyboard_gate_to_eg_b
  // kNormalizedRoutes[] still holds exactly the frozen route count (kRouteCount
  // generated; the array is sized by that count).
  CHECK_EQ(sizeof(reg::kNormalizedRoutes) / sizeof(reg::kNormalizedRoutes[0]), core::kRouteCount);
}

static void module_ranges_are_contiguous_and_disjoint() {
  std::uint32_t paramNext = 0, jackNext = 0;
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    const auto& m = reg::kModules[i];
    CHECK(m.paramBegin == paramNext);
    CHECK(m.paramBegin + m.paramCount <= core::kParameterCount);
    CHECK(m.jackBegin == jackNext);
    CHECK(m.jackBegin + m.jackCount <= core::kJackCount);
    paramNext += m.paramCount;
    jackNext += m.jackCount;
  }
  // kParameterCount also counts program params (module params come first), so
  // module ranges only need to fit within it; prog contiguity is checked below.
  CHECK(paramNext <= core::kParameterCount);
  CHECK(jackNext == core::kJackCount);
}

static void program_ranges_within_params() {
  // Program params are contiguous and start immediately after the module range.
  std::uint32_t moduleParams = 0;
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i)
    moduleParams += reg::kModules[i].paramCount;

  std::uint32_t next = moduleParams;
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    const auto& p = reg::kPrograms[i];
    CHECK(p.paramBegin == next);
    CHECK(p.paramBegin + p.paramCount <= core::kParameterCount);
    CHECK(p.slot >= 1u && p.slot <= 3u);
    CHECK(!p.stable_id.empty());
    CHECK(!p.name.empty());
    CHECK(!p.cartridge.empty());               // identity content (Codex f9a4bdae): +cartridge
    CHECK(valid_status(p.status));             // +status basic validity
    CHECK(!p.evidence.source.empty());         // +evidence basic validity (cited manual + span)
    CHECK(p.evidence.lineStart <= p.evidence.lineEnd);
    CHECK(valid_so(p.selfOscillating));
    CHECK(valid_prog_fe(p.fieldEvidence));  // per-field provenance (family, selfOscillating)
    // unknown ⇔ unverified biconditional for both program facts (not just one-way).
    CHECK(unknown_evidence_consistent(p.family, p.fieldEvidence.family));
    CHECK(unknown_evidence_consistent(p.selfOscillating, p.fieldEvidence.selfOscillating));
    next += p.paramCount;
  }
  CHECK(next == core::kParameterCount);
}

static void parameters_are_valid() {
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& p = reg::kParameters[i];
    CHECK(!p.stable_id.empty());
    CHECK(owner_exists(p.owner));
    CHECK(p.min <= p.max);
    CHECK(p.initial >= p.min && p.initial <= p.max);
    CHECK(p.step >= 0.0);
    CHECK(valid_status(p.status));
    CHECK(valid_pfe(p.fieldEvidence));  // per-field provenance split (07 §10, Codex 03848819)
    CHECK(!p.evidence.source.empty());
  }
}

static void selector_options_are_well_formed() {
  // A selector parameter's discrete value domain is carried IN the descriptor so UI/MIDI never
  // receive a bare 0/1/2 integer without knowing what it means (Codex 03848819 Root 2). Verify the
  // option table is exact and the pointer/count wiring is consistent: a continuous parameter has
  // optionCount==0 with options==nullptr; a selector has optionCount>=2, a non-null options pointer,
  // non-empty labels, no duplicate label within one selector's slice, and every option slice counted
  // exactly once (sum(optionCount) == kParameterOptionLabelCount).
  std::uint32_t totalOptions = 0;
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& p = reg::kParameters[i];
    if (p.optionCount == 0) {
      CHECK(p.options == nullptr);
      continue;
    }
    CHECK(p.optionCount >= 2u);
    CHECK(p.options != nullptr);
    totalOptions += p.optionCount;
    for (std::uint32_t k = 0; k < p.optionCount; ++k) {
      CHECK(p.options[k] != nullptr);
      CHECK(p.options[k][0] != '\0');  // non-empty label
      for (std::uint32_t m = k + 1; m < p.optionCount; ++m)
        // Compare label CONTENT, not the pointer: ["same","same"] would share no pointer yet is
        // still a duplicate label (Codex 644ea86e Root 2).
        CHECK_FALSE(std::string_view(p.options[k]) == std::string_view(p.options[m]));
    }
  }
  CHECK_EQ(totalOptions, reg::kParameterOptionLabelCount);
  for (std::uint32_t k = 0; k < reg::kParameterOptionLabelCount; ++k) {
    CHECK(reg::kParameterOptionLabels[k] != nullptr);
    CHECK(reg::kParameterOptionLabels[k][0] != '\0');
  }
}

static void jacks_are_valid() {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    const auto& j = reg::kJacks[i];
    CHECK(!j.stable_id.empty());
    CHECK(find_module(j.module) >= 0);      // owning module must exist
    CHECK(j.nominalMin <= j.nominalMax);
    CHECK(j.toleratedMin <= j.toleratedMax);
    CHECK(j.maxCables >= 1u);
    CHECK(j.modulationDepthPerVolt >= 0.0);
    CHECK(j.gateThresholdVolts >= 0.0);
    CHECK(j.hysteresisVolts >= 0.0);
    CHECK(valid_status(j.status));
    CHECK(valid_fe(j.fieldEvidence));       // per-field provenance split (07 §3, §10)
    CHECK(!j.evidence.source.empty());
    // unknown-enum biconditional (Codex 587f5e72): value==unknown ⇔ evidence==unverified.
    // Both directions enforced — an unknown must be unverified AND a concrete value must be
    // confirmed/provisional (never unverified).
    CHECK(unknown_evidence_consistent(j.signalType, j.fieldEvidence.signalType));
    CHECK(unknown_evidence_consistent(j.polarity, j.fieldEvidence.polarity));
    CHECK(unknown_evidence_consistent(j.coupling, j.fieldEvidence.coupling));
  }
}

static void routes_resolve_and_sink_cardinality() {
  const std::uint32_t routeCount = core::kRouteCount;
  for (std::uint32_t i = 0; i < routeCount; ++i) {
    const auto& r = reg::kNormalizedRoutes[i];
    CHECK(find_jack(r.sourceJack) >= 0);     // source jack must exist
    CHECK(find_jack(r.sinkJack) >= 0);       // sink jack must exist
    CHECK(!r.stable_id.empty());
    CHECK(valid_status(r.status));
    CHECK(!r.evidence.source.empty());
    // Route is identified by its own stable numeric id, and that id round-trips
    // through the generated string switch (order-independent identity, 07 §7).
    CHECK(core::route_id_string(r.id) == r.stable_id);
    // A sink is the target of at most one route.
    for (std::uint32_t j = i + 1; j < routeCount; ++j) {
      CHECK_FALSE(reg::kNormalizedRoutes[j].sinkJack == r.sinkJack);
    }
  }
}

static void devices_capacity_matches_routes() {
  // The route-override bank is indexed by RouteId, so it must cover the id-space
  // (one-past-the-last id), not merely the count — a sparse route id is caught.
  CHECK(core::kDeviceRouteCapacity >= core::kRouteIdSpace);
  CHECK(core::kDeviceRouteCapacity >= core::kRouteCount);
}

static void id_string_lookup_is_complete() {
  // Every enum value must round-trip through the generated id-string switch, keyed
  // by each descriptor's OWN explicit numeric id (order-independent identity,
  // design/07 §7) — never by its JSON/array index. index==id is an accident to be
  // caught by keys-by-id, not an ordering guarantee to rely on.
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    const auto& d = reg::kModules[i];
    CHECK(core::module_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& d = reg::kParameters[i];
    CHECK(core::parameter_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    const auto& d = reg::kJacks[i];
    CHECK(core::jack_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    const auto& d = reg::kPrograms[i];
    CHECK(core::program_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kRouteCount; ++i) {
    const auto& d = reg::kNormalizedRoutes[i];
    CHECK(core::route_id_string(d.id) == d.stable_id);
  }
}

int main() {
  frozen_counts();
  module_ranges_are_contiguous_and_disjoint();
  program_ranges_within_params();
  parameters_are_valid();
  selector_options_are_well_formed();
  jacks_are_valid();
  routes_resolve_and_sink_cardinality();
  devices_capacity_matches_routes();
  id_string_lookup_is_complete();
  return ::test::finish("registry");
}
