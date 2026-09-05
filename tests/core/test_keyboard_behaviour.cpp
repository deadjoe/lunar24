// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-③ per-note keyboard behaviour tests (design/00 §2d/§2e, design/06 §P4,
// design/07 §3 §6; @Claude Go msg 37db4aa5).
//
// Four behaviours: pressure output modes/rise-fall, portamento, vibrato, note
// quantiser scale+root. The contract this layer exists to uphold:
//   * scalar parameter reads go through read_side_scalar (the side-resolution
//     choke point in keyboard_mode.h) — never the global parameters[] for a side;
//   * the non-scalar scale editor is read through the same side_bank() resolution
//     from the per-side `_r` mirror;
//   * it emits control signals only (no audio);
//   * the whole interpretation flows through the P4-① InputStateMachine.
//
// Per @Claude the information is in the negation (mandate #4): the negative is a
// REAL bypass path, not a stub. Here the choke point being protected is the SIDE
// READ (the P4-① translate() bypass is already proven in test_input_equivalence),
// so the negative is a reader that ignores the resolved side bank and reads the
// global/shared parameters[] for a split-RIGHT note — the genuine mistake of
// forgetting the right bank. It must produce a divergent control stream.
//
// Continuous behaviours (portamento/vibrato/pressure envelope) are asserted by
// STRUCTURE + the cross-sample-rate invariant, not by arbitrary exact curves: the
// norm->time/amount laws are PROVISIONAL (design/00 §5 "先量后签"), the exact
// envelope segment levels are PROVISIONAL (manual enumerates modes, not levels) —
// so the tests hold shape, monotonicity, gate behaviour and the mandate-#4
// divergence, not an invented constant. The quantiser is DISCRETE and
// deterministic, so it carries exact absolute anchors.

#include "mini_test.h"

#include <array>
#include <cmath>
#include <cstdint>

#include <lunar24/core/control_event.h>
#include <lunar24/core/keyboard_behaviour.h>
#include <lunar24/core/keyboard_mode.h>

namespace core = lunar24::core;

static bool near(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) < eps;
}

// ---------------------------------------------------------------- quantiser ----

static void quantise_absolute_anchors() {
  // Ionian @ C: notes {0,2,4,5,7,9,11}. A 1 V/oct pitch CV = 12 semitones.
  // Exact, deterministic (discrete per-note decision).
  CHECK_TRUE(near(core::quantize_pitch(0.0, core::kScaleIonian, 0), 0.0));
  // 1 semitone below the 2nd scale note snaps DOWN to C.
  CHECK_TRUE(near(core::quantize_pitch(1.0 / 12.0, core::kScaleIonian, 0), 0.0));
  // 2 semitones is an in-scale note -> stays.
  CHECK_TRUE(near(core::quantize_pitch(2.0 / 12.0, core::kScaleIonian, 0), 2.0 / 12.0));
  // Two octaves up (24 semitones) -> octave-wrapped C.
  CHECK_TRUE(near(core::quantize_pitch(2.0, core::kScaleIonian, 0), 2.0));
  // Microtonal (all notes off) -> passthrough, no quantise.
  CHECK_TRUE(near(core::quantize_pitch(1.0 / 12.0, core::kMicrotonalScaleMask, 0), 1.0 / 12.0));
  CHECK_TRUE(near(core::quantize_pitch(2.0 / 12.0, core::kMicrotonalScaleMask, 0), 2.0 / 12.0));
  // Chromatic (all 12) -> snaps to the nearest semitone grid.
  CHECK_TRUE(near(core::quantize_pitch(1.9 / 12.0, core::kChromaticScaleMask, 0), 2.0 / 12.0));
  CHECK_TRUE(near(core::quantize_pitch(0.4 / 12.0, core::kChromaticScaleMask, 0), 0.0));
  // Root offset: root D (=2) transposes the whole scale up two semitones. The
  // scale note "2 semitones above D" is F#/4; an input at that pitch stays put.
  CHECK_TRUE(near(core::quantize_pitch(4.0 / 12.0, core::kScaleIonian, 2), 4.0 / 12.0));
}

static void root_note_and_scale_table() {
  // ROOT NOTE: norm -> semitone (C..H). PROVISIONAL split; nominal only.
  CHECK_EQ(static_cast<std::uint32_t>(core::root_note_semitone(0.0)), 0u);
  CHECK_EQ(static_cast<std::uint32_t>(core::root_note_semitone(1.0)), 11u);
  CHECK_EQ(static_cast<std::uint32_t>(core::root_note_semitone(0.5)), 6u);  // lround(5.5)

  // preset_scale_mask resolves the unambiguous scales; the style scales the manual
  // only NAMES without enumerating their intervals are UNRESOLVED (a real evidence
  // gap — we refuse to invent a pattern).
  CHECK_EQ(core::preset_scale_mask(0), core::kChromaticScaleMask);      // semitones
  CHECK_EQ(core::preset_scale_mask(1), core::kScaleIonian);
  CHECK_EQ(core::preset_scale_mask(6), core::kScaleAeolian);
  CHECK_EQ(core::preset_scale_mask(18), core::kScaleWholeTone);
  CHECK_EQ(core::preset_scale_mask(8), core::kScaleUnresolved);   // blues-major
  CHECK_EQ(core::preset_scale_mask(13), core::kScaleUnresolved);  // japanese
  CHECK_EQ(core::preset_scale_mask(16), core::kScaleUnresolved);  // arabian
}

// ---------------------------------------------------------------- portamento ----

static void portamento_legato_decision_and_glide() {
  // LEGATO off = always glide.
  core::PortamentoGlide g;
  g.setSampleRate(48000);
  g.setNorm(0.2, 0.0);  // tau = 0.2*2.5 = 0.5s, legato off
  g.noteOn(0.0, true);
  CHECK_TRUE(near(g.current(), 0.0));
  g.noteOn(2.0, true);  // glide to 2
  double c = 0.0;
  for (int i = 0; i < 48000; ++i) c = g.tick();  // exactly 1 second
  // One-pole reaches 2*(1 - exp(-t/tau)) = 2*(1-exp(-2)) at t=1s (fs-independent).
  CHECK_TRUE(near(c, 2.0 * (1.0 - std::exp(-2.0)), 1e-4));

  // Cross-sample-rate invariant: same wall-clock value at 48k and 96k.
  core::PortamentoGlide g96;
  g96.setSampleRate(96000);
  g96.setNorm(0.2, 0.0);
  g96.noteOn(0.0, true);
  g96.noteOn(2.0, true);
  double c96 = 0.0;
  for (int i = 0; i < 96000; ++i) c96 = g96.tick();
  CHECK_TRUE(near(c, c96, 1e-4));

  // LEGATO on: a single (non multi-touch) note JUMPS, no glide.
  core::PortamentoGlide l;
  l.setSampleRate(48000);
  l.setNorm(0.2, 1.0);  // legato on
  l.noteOn(0.0, false);  // single
  CHECK_TRUE(near(l.current(), 0.0));
  l.noteOn(2.0, false);  // single again -> jump
  CHECK_TRUE(near(l.current(), 2.0));
  l.noteOn(3.0, true);   // multi-touch -> glide
  double gts = l.tick();
  CHECK_TRUE(gts > 2.0 && gts < 3.0);  // monotonic, on the way up
}

// -------------------------------------------------------------------- vibrato ----

static void vibrato_structure() {
  // Speed 0.5 -> 7.5 Hz, depth 0.5 -> 0.5*(2/12) V, delay 0 -> full strength now.
  core::Vibrato v;
  v.setSampleRate(48000);
  v.setNorm(0.5, 0.5, 0.0, 0.0);
  v.gate(true);
  double first = 0.0, maxAbs = 0.0;
  for (int i = 0; i < 48000; ++i) {
    double d = v.tick(0.0);
    if (i == 0) first = d;  // the first output is the zero-crossing edge (phase 0)
    if (std::fabs(d) > maxAbs) maxAbs = std::fabs(d);
  }
  CHECK_TRUE(std::fabs(first) < 1e-3);               // starts at the zero crossing
  CHECK_TRUE(maxAbs > 0.05 && maxAbs < 0.10);        // bounded by ~depth (0.0833)

  // gate off -> output stops.
  v.gate(false);
  CHECK_TRUE(near(v.tick(0.0), 0.0));

  // PRESSURE control: higher key pressure scales the depth up (monotonic).
  core::Vibrato vp;
  vp.setSampleRate(48000);
  vp.setNorm(0.5, 0.5, 0.0, 0.5);  // pressure control on, amount 0.5
  vp.gate(true);
  double maxP = 0.0;
  for (int i = 0; i < 48000; ++i) {
    double d = vp.tick(1.0);  // full pressure
    if (std::fabs(d) > maxP) maxP = std::fabs(d);
  }
  CHECK_TRUE(maxP > maxAbs + 0.01);  // pressure scaled the depth up

  // DELAY: the depth ramps from 0 to full over the delay time.
  core::Vibrato vd;
  vd.setSampleRate(48000);
  vd.setNorm(0.5, 0.5, 0.8, 0.0);  // delay -> 2s
  vd.gate(true);
  double early = std::fabs(vd.tick(0.0));            // t=0: ramp ~0
  double mid = 0.0;
  for (int i = 0; i < 4800; ++i) mid = std::fabs(vd.tick(0.0));  // ~0.1s
  CHECK_TRUE(early < 1e-6);
  CHECK_TRUE(mid < 0.02);  // only ~5% strength at 0.1s of a 2s delay
}

// ------------------------------------------------------------- pressure output ----

static void pressure_mode_slew_no_overshoot() {
  core::PressureOutlet p;
  p.setSampleRate(48000);
  p.setMode(core::PressureOutput::Pressure);
  p.setTimes(0.2, 0.2);  // rise=fall=0.5s
  double out = 0.0, everMax = 0.0;
  for (int i = 0; i < 48000; ++i) {
    out = p.tick(1.0);
    if (out > everMax) everMax = out;
  }
  CHECK_TRUE(everMax <= 1.0 + 1e-9);  // one-pole never overshoots
  CHECK_TRUE(out > 0.5);              // reached most of the way (tau 0.5s, 1s elapsed)
}

static void pressure_asr_ad_loop_random() {
  // ASR: attack to sustain (captured at press), hold, release to 0 on gate-off.
  core::PressureOutlet asr;
  asr.setSampleRate(48000);
  asr.setMode(core::PressureOutput::Asr);
  asr.setTimes(0.05, 0.05);  // rise/fall tau = 0.125s
  asr.gate(true);
  double out = 0.0;
  for (int i = 0; i < 48000; ++i) out = asr.tick(1.0);
  CHECK_TRUE(near(out, 1.0, 1e-2));  // sustain ~= pressure at press (1.0)
  asr.gate(false);
  for (int i = 0; i < 48000; ++i) out = asr.tick(0.0);
  CHECK_TRUE(out < 0.1);  // released toward 0

  // AD: attack to peak then decay to 0 even while the gate stays high.
  core::PressureOutlet ad;
  ad.setSampleRate(48000);
  ad.setMode(core::PressureOutput::Ad);
  ad.setTimes(0.01, 0.01);  // tau = 0.025s (converges within the attack window)
  ad.gate(true);
  double peak = 0.0;
  for (int i = 0; i < 4800; ++i) { out = ad.tick(1.0); if (out > peak) peak = out; }
  CHECK_TRUE(peak > 0.9);  // reached near the peak (pressure at press)
  for (int i = 0; i < 48000; ++i) out = ad.tick(1.0);
  CHECK_TRUE(out < 0.05);  // decayed to 0 despite gate high

  // LOOP: reverses back to attack after decay while held.
  core::PressureOutlet lp;
  lp.setSampleRate(48000);
  lp.setMode(core::PressureOutput::Loop);
  lp.setTimes(0.01, 0.01);
  lp.gate(true);
  for (int i = 0; i < 48000; ++i) { (void)lp.tick(1.0); }  // warm up (let it cycle once)
  // Ensure it cycled over a couple periods: peak -> fall -> rise.
  bool peaked = false, fellAfterPeak = false, roseAfterFall = false;
  double prev = 0.0;
  lp.gate(true);
  for (int i = 0; i < 96000; ++i) {
    out = lp.tick(1.0);
    if (out < prev && peaked) fellAfterPeak = true;
    if (out > prev && fellAfterPeak) roseAfterFall = true;
    if (out > 0.5 && !peaked) peaked = true;
    prev = out;
  }
  CHECK_TRUE(peaked && fellAfterPeak && roseAfterFall);

  // RANDOM: deterministic with a fixed seed, value in [0,1), changes per press.
  core::PressureOutlet r1, r2;
  r1.setSampleRate(48000); r2.setSampleRate(48000);
  r1.setMode(core::PressureOutput::Random); r2.setMode(core::PressureOutput::Random);
  r1.setTimes(0.0, 0.0); r2.setTimes(0.0, 0.0);
  r1.setRandomSeed(1234); r2.setRandomSeed(1234);
  r1.gate(true); r2.gate(true);
  double a = r1.tick(0.0), b = r2.tick(0.0);
  CHECK_TRUE(a >= 0.0 && a < 1.0);
  CHECK_TRUE(near(a, b, 1e-9));  // same seed -> same value (reproducible)
  r1.gate(false); r1.gate(true);  // new press draws the next value
  double c = r1.tick(0.0);
  CHECK_TRUE(std::fabs(c - a) > 1e-9);  // a fresh press is a fresh random voltage
}

// ---------------------------------------------------------- side-read bypass ----

// Mandate #4 negative for THIS slice. The choke point being protected here is the
// side-context scalar read (read_side_scalar) — the P4-① translate() bypass is
// proven in test_input_equivalence, and the scale-editor non-scalar side path is a
// separate concern. The bug the choke point exists to prevent: reading the global /
// shared bank for a split-RIGHT note (forgetting the right bank). A rogue reader
// that ignores the resolved side and reads bank 0 must diverge from the conforming
// side-aware reader.
static void side_read_bypass_produces_divergent_stream() {
  constexpr std::size_t KB = 512;
  std::array<double, KB> left{};
  std::array<double, KB> right{};
  auto idv = [](core::ParameterId p) { return static_cast<core::IdValue>(p); };

  // Same pressure mode on both banks so the ONLY divergence is the per-side smooth
  // time. Left (shared) ramps instantly; right slews slowly.
  left[idv(core::ParameterId::keyboard_pressure_output)] = 0.0;   // Pressure
  right[idv(core::ParameterId::keyboard_pressure_output)] = 0.0;  // Pressure
  left[idv(core::ParameterId::keyboard_pressure_rise)] = 0.0;     // instant
  left[idv(core::ParameterId::keyboard_pressure_fall)] = 0.0;
  right[idv(core::ParameterId::keyboard_pressure_rise)] = 1.0;    // tau = 2.5s
  right[idv(core::ParameterId::keyboard_pressure_fall)] = 1.0;

  // Conforming: resolves the right bank for a split-RIGHT note.
  auto canonBank = [&](std::uint8_t bank, core::IdValue id) { return bank == 1 ? right[id] : left[id]; };
  // Rogue: ignores the resolved bank and reads the global/reference bank 0 only.
  auto rogueBank = [&](std::uint8_t /*bank*/, core::IdValue id) { return left[id]; };
  int scaleBankSeen = -1;  // records the bank the non-scalar editor was read from
  auto scaleOf = [&](std::uint8_t b) {
    scaleBankSeen = static_cast<int>(b);
    return std::uint16_t{0};  // all-off (microtonal)
  };

  const core::KeyboardMode split = core::KeyboardMode::Split;
  const core::KeyboardSide rightSide = core::KeyboardSide::Right;

  auto canon = core::read_behaviour_params(canonBank, scaleOf, split, rightSide);
  auto rogue = core::read_behaviour_params(rogueBank, scaleOf, split, rightSide);

  // Same mode; the divergence is the per-side smooth time.
  CHECK_EQ(canon.pressureOutput, rogue.pressureOutput);
  CHECK_TRUE(canon.pressureRise > rogue.pressureRise);
  // Absolute anchor: the non-scalar scale editor honours side_bank(Split, Right)==1
  // (it would read bank 0 for the left side — an anchor, not a relative check).
  CHECK_EQ(scaleBankSeen, 1);

  // Render a right-side note through each, one sample after the pressure lands.
  core::KeyboardBehaviour cb, rb;
  cb.configure(canon, 48000);
  rb.configure(rogue, 48000);

  core::ControlEvent ge{};
  ge.kind = core::ControlEventKind::gate_on;
  ge.value = core::SignalSample{1};
  cb.handleControlEvent(ge);
  rb.handleControlEvent(ge);
  core::ControlEvent pr{};
  pr.kind = core::ControlEventKind::pressure;
  pr.value = core::SignalSample{1.0};
  cb.handleControlEvent(pr);
  rb.handleControlEvent(pr);

  double cpitch = 0.0, cpres = 0.0, rpitch = 0.0, rpres = 0.0;
  cb.tick(&cpitch, &cpres);
  rb.tick(&rpitch, &rpres);

  CHECK_TRUE(near(cpres, 0.0, 1e-3));  // right bank: slow slew, still ~0
  CHECK_TRUE(near(rpres, 1.0, 1e-3));  // rogue read bank 0: instant jump to 1
  CHECK_TRUE(std::fabs(cpres - rpres) > 0.5);  // the control streams diverge
}

int main() {
  quantise_absolute_anchors();
  root_note_and_scale_table();
  portamento_legato_decision_and_glide();
  vibrato_structure();
  pressure_mode_slew_no_overshoot();
  pressure_asr_ad_loop_random();
  side_read_bypass_produces_divergent_stream();
  return ::test::finish("test_keyboard_behaviour");
}
