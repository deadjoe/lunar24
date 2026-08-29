// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) Envelope A/B sound-core strong-oracle suite for
// core/include/lunar24/core/envelope_generator.h. This new EG is a SEPARATE
// ADSR/HOLD/SELF-GEN generator, NOT the EnvelopeFollower (which is a preamp L2
// detector with its own rectifying one-pole); the tests pin behaviour / trend /
// determinism, and never fake a measured hardware curve (see the header's
// PROVISIONAL modelling constants).
//
// @Codex mandate (msg 37096471) must-tests:
//   ①  full ADSR phases + monotonicity (attack rise, decay to sustain, release to 0)
//   ②  three-time speed trends (larger A/D/R -> slower)
//   ③  attack/release mid-way reversal continuity (no jump in either direction)
//   ④  real descriptor gate interpretation (CV -> sink_gate_interpret -> EG)
//   ⑤  HOLD only opens VCA, never freezes/pulls up ENV; HOLD off resumes, no reset
//   ⑥  SELF-GEN no-gate period (LFO-like, uses A rise + R fall only)
//   ⑦  A/B state/config isolation (two real instances, two real JackIds)
//   ⑧  reset (clears dynamic/level/latch, keeps config)
//   ⑨  four sample-rates wall-clock trend (44.1/48/88.2/96k, NO fixed 48k)
//   ⑩  block partition per-sample bit-identical (one pass vs 64/128)
//   ⑪  ENV 0..8V finite/bounded; VCA-CV bounded
//   ⑫  zero-seconds deterministic, finite, no divide-by-zero
//
// Negative controls (each narrow old-error RED->revert GREEN) are run separately
// in a detached /tmp worktree: ① attack/decay -> instant gate, ② release ignored /
// starts from 1, ③ HOLD forces ENV high or VCA not open, ④ SELF-GEN off/constant,
// ⑤ A/B shared state or fixed 48k phase step.

#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "drone_test_common.h"
#include "lunar24/core/descriptors.h"
#include "lunar24/core/envelope_generator.h"
#include "lunar24/core/sink_interpret.h"
#include "lunar24/registry.hpp"

namespace core = lunar24::core;
namespace reg = lunar24::registry;

namespace {

using core::EnvelopeGenerator;

const core::JackDescriptor& real_jack(core::JackId id) {
  const core::JackDescriptor* p = nullptr;
  for (std::size_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) {
      p = &reg::kJacks[i];
      break;
    }
  }
  CHECK(p != nullptr);
  return p ? *p : reg::kJacks[0];
}

bool nondecreasing(const std::vector<double>& v, std::size_t lo, std::size_t hi) {
  for (std::size_t i = lo + 1; i < hi; ++i)
    if (v[i] < v[i - 1] - 1e-9) return false;
  return true;
}

bool nonincreasing(const std::vector<double>& v, std::size_t lo, std::size_t hi) {
  for (std::size_t i = lo + 1; i < hi; ++i)
    if (v[i] > v[i - 1] + 1e-9) return false;
  return true;
}

// ---------------------------------------------------------------------------
// ①  full ADSR phases + monotonicity
// ---------------------------------------------------------------------------
void test_adsr_phases_and_monotonic() {
  const double sr = 48000.0;
  EnvelopeGenerator eg(sr);
  eg.setAttackSeconds(0.005);
  eg.setDecaySeconds(0.01);
  eg.setReleaseSeconds(0.005);
  eg.setSustain(0.5);

  // gate held high: attack rises to 1, then decays to sustain(0.5) and holds.
  std::vector<double> hold;
  const std::size_t N = 12000;
  for (std::size_t i = 0; i < N; ++i) {
    eg.tick(true);
    hold.push_back(eg.level01());
  }
  std::size_t peak = hold.size();
  for (std::size_t i = 0; i < hold.size(); ++i)
    if (hold[i] >= 1.0 - 1e-9) {
      peak = i;
      break;
    }
  CHECK(peak > 0 && peak < hold.size());
  CHECK(nondecreasing(hold, 0, peak));            // attack: monotone rise
  CHECK(nonincreasing(hold, peak, hold.size()));  // decay: monotone fall to sustain
  CHECK(std::fabs(hold.back() - 0.5) < 1e-3);     // settled at sustain
  CHECK(eg.phase() == EnvelopeGenerator::Phase::sustain);

  // gate low: release to 0 and return to idle.
  std::vector<double> rel;
  for (std::size_t i = 0; i < N; ++i) {
    eg.tick(false);
    rel.push_back(eg.level01());
  }
  CHECK(nonincreasing(rel, 0, rel.size()));
  CHECK(std::fabs(rel.back()) < 1e-3);
  CHECK(eg.phase() == EnvelopeGenerator::Phase::idle);
}

// ---------------------------------------------------------------------------
// ②  three-time speed trends
// ---------------------------------------------------------------------------
void test_time_speed_trends() {
  const double sr = 48000.0;
  // attack: larger seconds -> slower, so later reach of 0.7.
  std::vector<double> attack_reach;
  for (double a : {0.02, 0.04, 0.08}) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(a);
    eg.setReleaseSeconds(0.02);
    eg.setSustain(0.0);
    double t = -1.0;
    for (std::size_t i = 0; i < 48000; ++i) {
      eg.tick(true);
      if (eg.level01() >= 0.7) {
        t = static_cast<double>(i) / sr;
        break;
      }
    }
    attack_reach.push_back(t);
  }
  CHECK(attack_reach[0] > 0.0);
  CHECK(attack_reach[0] < attack_reach[1] && attack_reach[1] < attack_reach[2]);

  // release: larger seconds -> slower, so later fall below 0.3.
  std::vector<double> release_fall;
  for (double r : {0.02, 0.04, 0.08}) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(0.001);
    eg.setReleaseSeconds(r);
    eg.setSustain(0.0);
    // drive to ~full, then release and time the fall.
    for (std::size_t i = 0; i < 4000; ++i) eg.tick(true);  // to full-ish
    double t = -1.0;
    for (std::size_t i = 0; i < 48000; ++i) {
      eg.tick(false);
      if (eg.level01() <= 0.3) {
        t = static_cast<double>(i) / sr;
        break;
      }
    }
    release_fall.push_back(t);
  }
  CHECK(release_fall[0] > 0.0);
  CHECK(release_fall[0] < release_fall[1] && release_fall[1] < release_fall[2]);
}

// ---------------------------------------------------------------------------
// ③  attack/release mid-way reversal continuity (no jump either way)
// ---------------------------------------------------------------------------
void test_midway_reversal_continuity() {
  const double sr = 48000.0;
  EnvelopeGenerator eg(sr);
  eg.setAttackSeconds(0.04);
  eg.setReleaseSeconds(0.04);
  eg.setSustain(0.5);

  // Rise to a mid-attack level, then DROP the gate mid-attack -> release from the
  // current level, no jump up or down.
  for (int i = 0; i < 1000; ++i) eg.tick(true);
  CHECK(eg.phase() == EnvelopeGenerator::Phase::attack);
  const double mid_attack = eg.level01();
  CHECK(mid_attack > 0.05 && mid_attack < 0.95);
  const double before = eg.level01();
  eg.tick(false);
  const double after = eg.level01();
  CHECK(eg.phase() == EnvelopeGenerator::Phase::release);
  CHECK(after <= before + 1e-9);            // no jump up
  CHECK(after >= mid_attack - 5e-3);        // no jump to 0
  CHECK(after > 0.0);

  // Let it release part-way, then RETRIGGER -> re-attack from the current release
  // level, not a jump to 0.
  for (int i = 0; i < 500; ++i) eg.tick(false);
  const double mid_release = eg.level01();
  CHECK(mid_release > 0.02 && mid_release < 0.9);
  const double b2 = eg.level01();
  eg.tick(true);
  const double a2 = eg.level01();
  CHECK(eg.phase() == EnvelopeGenerator::Phase::attack);
  CHECK(a2 >= b2 - 1e-9);                   // rising again from current, not 0
  CHECK(a2 >= mid_release - 0.05);
}

// ---------------------------------------------------------------------------
// ④  real descriptor gate interpretation (CV -> sink_gate_interpret -> EG)
// ---------------------------------------------------------------------------
void test_real_descriptor_gate() {
  const core::JackDescriptor& ga = real_jack(core::JackId::envelope_a_gate_in);
  const core::JackDescriptor& gb = real_jack(core::JackId::envelope_b_gate_in);
  CHECK(ga.gateThresholdVolts > 0.0 && ga.hysteresisVolts > 0.0);
  CHECK(gb.gateThresholdVolts > 0.0 && gb.hysteresisVolts > 0.0);

  core::GateClockSinkState stA, stB;
  EnvelopeGenerator ega(48000.0), egb(48000.0);
  ega.setAttackSeconds(0.02);
  egb.setAttackSeconds(0.02);
  ega.setSustain(0.5);
  egb.setSustain(0.5);

  auto step = [&](double va, double vb) {
    const core::SinkSample sa = core::sink_gate_interpret(ga, stA, va);
    const core::SinkSample sb = core::sink_gate_interpret(gb, stB, vb);
    ega.tick(sa.gateHigh);
    egb.tick(sb.gateHigh);
  };

  // Both low: idle.
  for (int i = 0; i < 2000; ++i) step(0.0, 0.0);
  CHECK(std::fabs(ega.level01()) < 1e-9);
  CHECK(std::fabs(egb.level01()) < 1e-9);
  // A rises (its CV crosses the descriptor threshold), B stays low (no gate).
  for (int i = 0; i < 2000; ++i) step(6.0, 0.0);
  CHECK(ega.level01() > 0.5);
  CHECK(egb.level01() < 1e-9);
  // A below-threshold CV (0.45V < thr+hyst=0.6) must NOT trigger: the EG never
  // sees a rising gate, which proves it does not hardcode gate volts.
  EnvelopeGenerator ega2(48000.0);
  ega2.setAttackSeconds(0.02);
  ega2.setSustain(0.5);
  core::GateClockSinkState st2;
  for (int i = 0; i < 2000; ++i) ega2.tick(core::sink_gate_interpret(ga, st2, 0.45).gateHigh);
  CHECK(std::fabs(ega2.level01()) < 1e-9);   // stayed idle below the real threshold
}

// ---------------------------------------------------------------------------
// ⑤  HOLD only opens VCA, not freeze/pull-up ENV; HOLD off resumes, no reset
// ---------------------------------------------------------------------------
void test_hold_only_opens_vca() {
  EnvelopeGenerator eg(48000.0);
  eg.setAttackSeconds(0.02);
  eg.setDecaySeconds(0.01);
  eg.setReleaseSeconds(0.04);
  eg.setSustain(0.5);
  eg.setHold(true);

  // gate high: ENV still runs its ADSR even with HOLD on; VCA stays open.
  for (int i = 0; i < 600; ++i) eg.tick(true);
  const double env_mid = eg.envVolts();
  CHECK(env_mid > 0.0 && env_mid < core::kEnvelopeEnvPeakVolt);  // ENV moving, not pinned
  CHECK(eg.vcaCvVolts() == core::kEnvelopeVcaOpenVolt);          // VCA held open
  for (int i = 0; i < 16000; ++i) eg.tick(true);                 // settle to sustain
  CHECK(eg.envVolts() <= core::kEnvelopeEnvPeakVolt + 1e-9);
  CHECK(std::fabs(eg.level01() - 0.5) < 1e-3);
  CHECK(eg.phase() == EnvelopeGenerator::Phase::sustain);
  CHECK(eg.vcaCvVolts() == core::kEnvelopeVcaOpenVolt);

  // HOLD off: VCA resumes tracking the current envelope; EG level untouched.
  const double lvl_before = eg.level01();
  eg.setHold(false);
  CHECK(eg.vcaCvVolts() < core::kEnvelopeVcaOpenVolt + 1e-12);
  CHECK(eg.vcaCvVolts() >= 0.0);
  CHECK(eg.level01() == lvl_before);   // no reset / retrigger

  // A/B HOLD independent: same config/gate, identical ENV, VCA differs.
  EnvelopeGenerator a(48000.0), b(48000.0);
  a.setAttackSeconds(0.02);
  b.setAttackSeconds(0.02);
  a.setSustain(0.5);
  b.setSustain(0.5);
  a.setHold(true);
  b.setHold(false);
  for (int i = 0; i < 600; ++i) {
    a.tick(true);
    b.tick(true);
  }
  CHECK(a.vcaCvVolts() == core::kEnvelopeVcaOpenVolt);
  CHECK(b.vcaCvVolts() < core::kEnvelopeVcaOpenVolt - 1e-9);
  CHECK(a.envVolts() == b.envVolts());   // ENV identical; only VCA opened
}

// ---------------------------------------------------------------------------
// ⑥  SELF-GEN no-gate period (LFO-like: A rise + R fall only)
// ---------------------------------------------------------------------------
void test_selfgen_no_gate_periodicity() {
  const double sr = 48000.0;
  const std::size_t n = static_cast<std::size_t>(sr * 0.3);
  auto oscillations = [&](double atk, double rls) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(atk);
    eg.setReleaseSeconds(rls);
    eg.setSustain(0.0);   // decay/sustain must not matter
    eg.setSelfGen(true);
    int osc = 0;
    bool high = false;
    for (std::size_t i = 0; i < n; ++i) {
      eg.tick(false);     // no external gate at all
      const double lvl = eg.level01();
      if (lvl >= 0.7) {
        high = true;
      } else if (high && lvl <= 0.2) {
        ++osc;            // completed one rise-to-peak + fall-to-trough
        high = false;
      }
    }
    return osc;
  };
  const int small = oscillations(0.003, 0.003);
  const int large = oscillations(0.05, 0.05);
  std::printf("⑥ self-gen oscillations in 0.3s: small-tau %d, large-tau %d\n", small, large);
  CHECK(small >= 2);            // it actually self-oscillates and repeats, no gate
  CHECK(small > large);         // larger ATT/RLS -> slower self-osc

  // SELF-GEN + HOLD: ENV keeps cycling (not narrowed by VCA), VCA stays open.
  EnvelopeGenerator eg(sr);
  eg.setAttackSeconds(0.004);
  eg.setReleaseSeconds(0.004);
  eg.setSustain(0.5);
  eg.setSelfGen(true);
  eg.setHold(true);
  double env_min = 1e9, env_max = -1e9;
  for (int i = 0; i < 20000; ++i) {
    eg.tick(false);
    const double e = eg.envVolts();
    env_min = std::min(env_min, e);
    env_max = std::max(env_max, e);
    CHECK(eg.vcaCvVolts() == core::kEnvelopeVcaOpenVolt);
  }
  CHECK(env_max > core::kEnvelopeEnvPeakVolt * 0.7);  // ENV continues to cycle high
  CHECK(env_min < core::kEnvelopeEnvPeakVolt * 0.3);  // and back low, despite HOLD
}

// ---------------------------------------------------------------------------
// ⑦  A/B state/config isolation (two real instances, two real JackIds)
// ---------------------------------------------------------------------------
void test_ab_isolation() {
  EnvelopeGenerator a(48000.0), b(48000.0);
  a.setAttackSeconds(0.02);
  b.setAttackSeconds(0.02);
  a.setSustain(0.8);
  b.setSustain(0.3);
  for (int i = 0; i < 2000; ++i) {
    a.tick(true);    // A gated
    b.tick(false);   // B not gated
  }
  CHECK(a.level01() > 0.5);
  CHECK(b.level01() < 1e-9);                     // dynamic level NOT shared
  CHECK(b.phase() == EnvelopeGenerator::Phase::idle);
  CHECK(a.phase() != EnvelopeGenerator::Phase::idle);

  EnvelopeGenerator a2(48000.0), b2(48000.0);
  for (EnvelopeGenerator* e : {&a2, &b2}) {
    e->setAttackSeconds(0.01);
    e->setDecaySeconds(0.01);
    e->setReleaseSeconds(0.01);
  }
  a2.setSustain(0.9);
  b2.setSustain(0.2);
  for (int i = 0; i < 20000; ++i) {
    a2.tick(true);
    b2.tick(true);
  }
  CHECK(std::fabs(a2.level01() - 0.9) < 1e-3);   // config isolation
  CHECK(std::fabs(b2.level01() - 0.2) < 1e-3);
}

// ---------------------------------------------------------------------------
// ⑧  reset: clears dynamic/level/latch, keeps config
// ---------------------------------------------------------------------------
void test_reset_clears_dynamic_keeps_config() {
  EnvelopeGenerator eg(48000.0);
  eg.setAttackSeconds(0.02);
  eg.setDecaySeconds(0.03);
  eg.setReleaseSeconds(0.04);
  eg.setSustain(0.7);
  eg.setHold(true);
  eg.setSelfGen(false);
  for (int i = 0; i < 3000; ++i) eg.tick(true);
  CHECK(eg.level01() > 0.3);

  eg.reset();
  CHECK(eg.level01() == 0.0);
  CHECK(eg.phase() == EnvelopeGenerator::Phase::idle);
  CHECK(eg.gateLatch() == false);
  CHECK(eg.attackSeconds() == 0.02);
  CHECK(eg.decaySeconds() == 0.03);
  CHECK(eg.releaseSeconds() == 0.04);
  CHECK(eg.sustain() == 0.7);
  CHECK(eg.hold() == true);
  CHECK(eg.selfGen() == false);

  eg.tick(true);   // a fresh rising edge after reset must attack from 0
  CHECK(eg.phase() == EnvelopeGenerator::Phase::attack);
  CHECK(eg.level01() > 0.0);
}

// ---------------------------------------------------------------------------
// ⑨  four sample-rates wall-clock trend (no fixed 48k)
// ---------------------------------------------------------------------------
void test_sample_rate_trend() {
  const std::vector<double> rates = {44100.0, 48000.0, 88200.0, 96000.0};
  std::vector<double> reach;
  for (double sr : rates) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(0.02);
    eg.setReleaseSeconds(0.02);
    eg.setSustain(0.0);
    double t = -1.0;
    const std::size_t n = static_cast<std::size_t>(sr * 0.2);
    for (std::size_t i = 0; i < n; ++i) {
      eg.tick(true);
      if (eg.level01() >= 0.7) {
        t = static_cast<double>(i) / sr;
        break;
      }
    }
    reach.push_back(t);
  }
  const double mn = *std::min_element(reach.begin(), reach.end());
  const double mx = *std::max_element(reach.begin(), reach.end());
  std::printf("⑨ cross-sr 0.7-reach: min %.6f s, spread %.6f s\n", mn, mx - mn);
  CHECK(mn > 0.0);
  CHECK(mx - mn < 0.01);   // consistent wall-clock across 44.1..96k, no fixed 48k
}

// ---------------------------------------------------------------------------
// ⑩  block partition per-sample bit-identical
// ---------------------------------------------------------------------------
void test_block_partition_bit_identical() {
  const double sr = 48000.0;
  const std::size_t total = 12000;
  std::vector<bool> gate(total);
  for (std::size_t i = 0; i < total; ++i)
    gate[i] = (i < 4000) || (i >= 5000 && i < 8000);   // two pulses

  auto render = [&](std::size_t block) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(0.02);
    eg.setDecaySeconds(0.05);
    eg.setReleaseSeconds(0.03);
    eg.setSustain(0.5);
    std::vector<double> out(total);
    std::size_t idx = 0;
    while (idx < total) {
      const std::size_t end = std::min(idx + block, total);
      for (std::size_t i = idx; i < end; ++i) {
        eg.tick(gate[i]);
        out[i] = eg.envVolts();
      }
      idx = end;
    }
    return out;
  };
  const std::vector<double> full = render(total);
  CHECK(drone_test::same_render(full, render(64)));
  CHECK(drone_test::same_render(full, render(128)));

  // SELF-GEN is also a per-sample state machine -> partition invariant.
  auto render_selfgen = [&](std::size_t block) {
    EnvelopeGenerator eg(sr);
    eg.setAttackSeconds(0.01);
    eg.setReleaseSeconds(0.01);
    eg.setSelfGen(true);
    std::vector<double> out(total);
    std::size_t idx = 0;
    while (idx < total) {
      const std::size_t end = std::min(idx + block, total);
      for (std::size_t i = idx; i < end; ++i) {
        eg.tick(false);
        out[i] = eg.envVolts();
      }
      idx = end;
    }
    return out;
  };
  CHECK(drone_test::same_render(render_selfgen(total), render_selfgen(64)));
}

// ---------------------------------------------------------------------------
// ⑪  ENV bounded/finite; VCA-CV bounded
// ---------------------------------------------------------------------------
void test_outputs_finite_and_bounded() {
  EnvelopeGenerator eg(44100.0);
  eg.setAttackSeconds(0.001);
  eg.setDecaySeconds(0.01);
  eg.setReleaseSeconds(0.001);
  eg.setSustain(0.9);
  eg.setSelfGen(true);
  bool gate = true;
  for (int i = 0; i < 50000; ++i) {
    if (i % 3000 == 0) gate = !gate;
    if (i % 5000 == 0) eg.setHold(!eg.hold());
    eg.tick(gate);
    const double e = eg.envVolts();
    const double v = eg.vcaCvVolts();
    CHECK(std::isfinite(e) && e >= 0.0 && e <= core::kEnvelopeEnvPeakVolt);
    CHECK(std::isfinite(v) && v >= 0.0 && v <= core::kEnvelopeVcaOpenVolt);
  }
}

// ---------------------------------------------------------------------------
// ⑫  zero-seconds deterministic / finite (instant stages, no div-by-zero)
// ---------------------------------------------------------------------------
void test_zero_seconds_deterministic() {
  EnvelopeGenerator eg(48000.0);
  eg.setAttackSeconds(0.0);
  eg.setDecaySeconds(0.0);
  eg.setReleaseSeconds(0.0);
  eg.setSustain(0.5);
  bool finite = true;
  for (int i = 0; i < 5; ++i) {
    eg.tick(true);
    if (!std::isfinite(eg.level01())) finite = false;
  }
  CHECK(finite);
  CHECK(std::fabs(eg.level01() - 0.5) < 1e-6);   // instant attack->decay->sustain
  CHECK(eg.phase() == EnvelopeGenerator::Phase::sustain);
  for (int i = 0; i < 5; ++i) eg.tick(false);
  CHECK(eg.level01() == 0.0);                    // instant release to 0
  CHECK(eg.phase() == EnvelopeGenerator::Phase::idle);
}

}  // namespace

int main() {
  test_adsr_phases_and_monotonic();
  test_time_speed_trends();
  test_midway_reversal_continuity();
  test_real_descriptor_gate();
  test_hold_only_opens_vca();
  test_selfgen_no_gate_periodicity();
  test_ab_isolation();
  test_reset_clears_dynamic_keeps_config();
  test_sample_rate_trend();
  test_block_partition_bit_identical();
  test_outputs_finite_and_bounded();
  test_zero_seconds_deterministic();
  return test::finish("envelope_generator");
}
