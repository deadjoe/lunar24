// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-④ per-side arp/seq engine tests (design/00 §2g; @Claude mandate msg 8b19b72a).
//
// The contract this layer exists to uphold:
//   * it SITS between the P4-① InputStateMachine (translate) and the P4-③
//     KeyboardBehaviour, transforming canonical ControlEvents, never audio;
//   * there is NO global arp/seq singleton — each side owns an ArpSeq instance, so a
//     split twin side can arpeggiate while the other plays a plain keyboard;
//   * the arp/seq mode mux is keyboard.mode (id 101, positions
//     [keyboard/arpeggiator/sequencer]); keyboard mode is a transparent pass-through;
//   * GLOBAL tempo (clock_bpm, id 129) is shared; per-side division ratio and rhythm
//     pattern are UN-RESOLVED and read as raw selectors, NEVER applied numerically.
//
// The value maps (arp interval 1..12, seq length 2..16, etc.) are PROVISIONAL linear
// ceilings (design/00 §5), and the arp note ordering is pitch-ordered PROVISIONAL
// (the manual's "sequence number of pressed plates" is UN-RESOLVED). Following the
// P4-③ test discipline, these tests pin the STRUCTURE — the mode mux, the chord/step
// advancement on the clock, the per-side independence, and the mandate-#4 divergence
// — not invented exact curves.
//
// Per @Claude (mandate #4) the negative is the real bypass path: the choke point
// protected here is the SIDE READ (read_side_scalar / side_bank resolution). The
// genuine mistake it prevents is reading the global/shared bank for a split-RIGHT
// side (forgetting the right bank). A rogue reader that ignores the resolved side
// reads bank 0 and must produce a divergent arp/seq control stream.

#include "mini_test.h"

#include <array>
#include <cmath>
#include <cstdint>

#include <lunar24/core/arp_sequencer.h>
#include <lunar24/core/control_event.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

namespace {

// A downstream sink that records every ControlEvent it is handed.
struct Recorder {
  std::array<core::ControlEvent, 64> ev{};
  std::uint32_t n = 0;
  void operator()(const core::ControlEvent& e) {
    if (n < static_cast<std::uint32_t>(ev.size())) ev[n++] = e;
  }
  std::uint32_t count(core::ControlEventKind k) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < n; ++i) if (ev[i].kind == k) ++c;
    return c;
  }
  // The i-th pitch event's value, in emission order (the interleaved gate_off between
  // notes makes raw-index assumptions brittle — index the pitch events directly).
  double pitchAt(std::uint32_t i) const {
    std::uint32_t c = 0;
    for (std::uint32_t k = 0; k < n; ++k)
      if (ev[k].kind == core::ControlEventKind::pitch) {
        if (c == i) return static_cast<double>(ev[k].value);
        ++c;
      }
    return 0.0;
  }
  bool near(double a, double b) const { return std::fabs(a - b) < 1e-6; }
};

// Feed the events translate() would emit for a note_on chord member: gate_on (no note
// identity) then the pitch (the note value that the arp builds its chord from).
void note_on(core::ArpSeq& s, Recorder& r, double pitch) {
  core::ControlEvent g{};
  g.kind = core::ControlEventKind::gate_on;
  g.value = core::SignalSample{1.0};
  g.source = 1;
  s.handleControlEvent(g, r);
  core::ControlEvent p{};
  p.kind = core::ControlEventKind::pitch;
  p.value = static_cast<core::SignalSample>(pitch);
  p.source = 1;
  s.handleControlEvent(p, r);
}

void note_off(core::ArpSeq& s, Recorder& r) {
  core::ControlEvent g{};
  g.kind = core::ControlEventKind::gate_off;
  g.value = core::SignalSample{0.0};
  g.source = 1;
  s.handleControlEvent(g, r);
}

void clock_edge(core::ArpSeq& s, Recorder& r) {
  core::ControlEvent c{};
  c.kind = core::ControlEventKind::clock;
  c.value = core::SignalSample{1.0};
  c.source = 9;
  s.handleControlEvent(c, r);
}

core::ArpSeqParams base_params() {
  core::ArpSeqParams p{};
  p.mode = 0;         // keyboard
  p.arpInterval = 0.0;
  p.seqLength = 0.0;  // -> 2 steps
  p.seqCvOutput = 1;  // gated
  for (std::uint32_t i = 0; i < 16; ++i) {
    p.steps[i].note = static_cast<std::uint8_t>(i % 16);
    p.steps[i].gate = 1;
  }
  return p;
}

}  // namespace

// ------------------------------------------------------ mode mux + pass-through --

static void mode_mux_and_keyboard_passthrough() {
  CHECK_TRUE(core::arp_seq_mode(0) == core::ArpSeqMode::Keyboard);
  CHECK_TRUE(core::arp_seq_mode(1) == core::ArpSeqMode::Arpeggiator);
  CHECK_TRUE(core::arp_seq_mode(2) == core::ArpSeqMode::Sequencer);
  // Unknown raw degrades to keyboard (no phantom arp/seq conjured).
  CHECK_TRUE(core::arp_seq_mode(7) == core::ArpSeqMode::Keyboard);

  // Keyboard mode = transparent pass-through: every event reaches the sink unchanged.
  core::ArpSeqParams p = base_params();  // mode 0
  core::ArpSeq s;
  s.configure(p, 48000);
  CHECK_TRUE(s.mode() == core::ArpSeqMode::Keyboard);

  Recorder r;
  note_on(s, r, 0.25);
  clock_edge(s, r);
  clock_edge(s, r);
  note_off(s, r);
  // The sink saw every event the engine was handed, none transformed.
  CHECK_EQ(r.n, 5u);  // gate_on+pitch, clock, clock, gate_off
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::clock), 2u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 1u);
}

// ------------------------------------------------- arpeggiator one-note-per-clock --

static void arp_emits_one_note_per_clock() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;  // arpeggiator
  p.arpDirection = 0;  // forward
  core::ArpSeq s;
  s.configure(p, 48000);
  CHECK_TRUE(s.mode() == core::ArpSeqMode::Arpeggiator);

  Recorder r;
  // Two-plate chord: C (0.25 V) and G (0.5 V) — 3 octaves... use nearby pitches.
  note_on(s, r, 0.0 / 12.0);   // C
  note_on(s, r, 4.0 / 12.0);   // E
  CHECK_EQ(r.n, 0u);           // arp mode does NOT sound the chord directly

  clock_edge(s, r);  // 1st note
  clock_edge(s, r);  // 2nd note
  clock_edge(s, r);  // 3rd note (wraps)
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 3u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 3u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 2u);  // between consecutive notes

  // First emitted pair is gate_on+pitch over chord[0]; no leading gate_off.
  CHECK_TRUE(r.ev[0].kind == core::ControlEventKind::gate_on);
  CHECK_TRUE(r.ev[1].kind == core::ControlEventKind::pitch);
  // Forward direction over chord [C, E]: C+i, E+i, C+i (wraps).
  const double i0 = static_cast<double>(core::arp_interval_semitones(p.arpInterval));
  CHECK_TRUE(r.near(r.pitchAt(0), 0.0 / 12.0 + i0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0 + i0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(2), 0.0 / 12.0 + i0 / 12.0));
}

static void arp_hold_keeps_chord_through_release() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  p.arpHold = 1;  // HOLD
  core::ArpSeq s;
  s.configure(p, 48000);

  Recorder r;
  note_on(s, r, 0.0 / 12.0);
  note_off(s, r);  // released, but HOLD keeps the chord
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 0u);  // no release emitted
  clock_edge(s, r);  // still arpeggiates
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
}

// ------------------------------------------------------- sequencer step advance --

static void seq_advances_steps_and_gates() {
  core::ArpSeqParams p = base_params();
  p.mode = 2;  // sequencer
  p.seqRun = 0;  // free
  p.seqDirection = 0;  // forward
  p.seqCvOutput = 1;   // gated: gate follows each step's gate flag
  p.seqLength = 0.0;   // -> 2 steps
  p.steps[0].note = 0; p.steps[1].note = 1;
  p.steps[0].gate = 1; p.steps[1].gate = 1;
  core::ArpSeq s;
  s.configure(p, 48000);
  CHECK_TRUE(s.mode() == core::ArpSeqMode::Sequencer);

  Recorder r;
  note_on(s, r, 0.0 / 12.0);  // transposition base C
  CHECK_EQ(r.n, 0u);           // seq does not sound the plate directly

  clock_edge(s, r);  // step 0
  clock_edge(s, r);  // step 1
  clock_edge(s, r);  // step 0 (wrap, len 2)
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 3u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 3u);
  // Seq step [i].note is transposed by the held-plate base (C = 0 V): 0, 1, 0 (wrap).
  CHECK_TRUE(r.near(r.pitchAt(0), 0.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(1), 1.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(2), 0.0 / 12.0));
}

static void seq_continuous_cv_always_gates() {
  core::ArpSeqParams p = base_params();
  p.mode = 2;
  p.seqRun = 0;
  p.seqCvOutput = 0;  // continuous: any step, gate always high while running
  p.seqLength = 0.0;
  p.steps[0].note = 0; p.steps[0].gate = 0;  // step 0 has gate flag 0
  core::ArpSeq s;
  s.configure(p, 48000);

  Recorder r;
  note_on(s, r, 0.0 / 12.0);
  clock_edge(s, r);  // step 0, continuous -> still emits gate_on
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);
}

// ----------------------------------------------- per-side, no global singleton --

static void per_side_instantiation_independent() {
  // Two sides of the same device: LEFT arpeggiates, RIGHT is a plain keyboard. Each
  // owns its own ArpSeq instance with its own settings (no shared global).
  core::ArpSeqParams left = base_params();
  left.mode = 1;  // arpeggiator
  left.arpDirection = 0;
  core::ArpSeqParams right = base_params();
  right.mode = 0;  // keyboard

  core::ArpSeq sl, sr;
  sl.configure(left, 48000);
  sr.configure(right, 48000);

  Recorder rl, rr;
  note_on(sl, rl, 0.0 / 12.0);
  note_on(sr, rr, 0.0 / 12.0);
  clock_edge(sl, rl);
  clock_edge(sr, rr);

  CHECK_EQ(rl.count(core::ControlEventKind::pitch), 1u);   // left arpeggiates
  CHECK_EQ(rl.count(core::ControlEventKind::gate_on), 1u);
  CHECK_EQ(rr.count(core::ControlEventKind::pitch), 1u);   // right: plate pitch passthrough
  CHECK_EQ(rr.count(core::ControlEventKind::gate_on), 1u);
  // The right keyboard mode forwarded the plate pitch (0 V) — DIFFERENT from the left
  // arp interval-shifted note. Verifies the two instances are genuinely independent.
  CHECK_TRUE(rl.near(rl.pitchAt(0), core::arp_interval_semitones(left.arpInterval) / 12.0));
  CHECK_TRUE(rr.near(rr.pitchAt(0), 0.0));
}

// ------------------------------------------------- mandate #4: side-drop bypass --

static void side_drop_produces_divergent_stream() {
  // The choke point protected here is the SIDE READ. In split mode, a RIGHT-side
  // arp/seq reader must resolve bank 1 (keyboardScalarRight). A rogue reader that
  // ignores the resolved side and reads bank 0 gets the WRONG mode/settings.
  // Build the per-side bank readers exactly as read_side_scalar would resolve them,
  // then feed a conforming and a rogue ArpSeq and show the streams diverge.
  constexpr std::size_t KB = 512;
  std::array<double, KB> left{};   // bank 0 (shared/left)
  std::array<double, KB> right{};  // bank 1 (right)
  auto idv = [](core::ParameterId id) { return static_cast<core::IdValue>(id); };

  // LEFT bank: keyboard mode (0). RIGHT bank: sequencer mode (2) + a different length.
  left[idv(core::ParameterId::keyboard_mode)] = 0.0;
  right[idv(core::ParameterId::keyboard_mode)] = 2.0;
  left[idv(core::ParameterId::keyboard_seq_length)] = 0.0;   // -> 2 steps (left)
  right[idv(core::ParameterId::keyboard_seq_length)] = 1.0;  // -> 16 steps (right)
  right[idv(core::ParameterId::keyboard_seq_cv_output)] = 1.0;

  // Conforming reader resolves the RIGHT bank for a split-RIGHT side.
  auto canonBank = [&](std::uint8_t bank, core::IdValue i) { return bank == 1 ? right[i] : left[i]; };
  // Rogue reader ignores the resolved bank and reads the shared bank 0 (the bug).
  auto rogueBank = [&](std::uint8_t /*bank*/, core::IdValue i) { return left[i]; };

  // The non-scalar side paths (steps + clock selectors) — the concrete DeviceState
  // wiring is a later slice; here they are uniform for both readers so the ONLY
  // divergence is the scalar side-read choke being tested.
  std::array<core::ArpSeqStep, 16> steps{};
  for (auto& s : steps) s.gate = 1;  // gated sequencer (right bank) steps are active
  std::array<std::uint8_t, 4> selectors{};
  const core::ArpSeqParams canon = core::read_arp_seq_params(
      canonBank, core::KeyboardMode::Split, core::KeyboardSide::Right, 120.0, steps, selectors);
  const core::ArpSeqParams rogue = core::read_arp_seq_params(
      rogueBank, core::KeyboardMode::Split, core::KeyboardSide::Right, 120.0, steps, selectors);
  // Proof the two sides genuinely differ once the side is honoured:
  CHECK_EQ(canon.mode, 2u);   // sequencer (right bank)
  CHECK_EQ(rogue.mode, 0u);   // keyboard (rogue read bank 0)
  CHECK_TRUE(canon.seqLength > rogue.seqLength);

  core::ArpSeq cs, rs;
  cs.configure(canon, 48000);
  rs.configure(rogue, 48000);

  Recorder rc, rr;
  // A held plate + three clock edges. Conforming = sequencer (emits steps); rogue =
  // keyboard (pass-through, emits nothing extra but the plate pitch once).
  note_on(cs, rc, 0.0 / 12.0);
  note_on(rs, rr, 0.0 / 12.0);
  clock_edge(cs, rc);
  clock_edge(cs, rc);
  clock_edge(cs, rc);
  clock_edge(rs, rr);
  clock_edge(rs, rr);
  clock_edge(rs, rr);

  const std::uint32_t canonGon = rc.count(core::ControlEventKind::gate_on);
  const std::uint32_t rogueGon = rr.count(core::ControlEventKind::gate_on);
  CHECK_EQ(canonGon, 3u);       // sequencer: one gate_on per clock (plate is not sounded)
  CHECK_EQ(rogueGon, 1u);       // keyboard: only the plate's own gate_on
  CHECK_TRUE(canonGon != rogueGon);  // the control streams diverge
}

int main() {
  mode_mux_and_keyboard_passthrough();
  arp_emits_one_note_per_clock();
  arp_hold_keeps_chord_through_release();
  seq_advances_steps_and_gates();
  seq_continuous_cv_always_gates();
  per_side_instantiation_independent();
  side_drop_produces_divergent_stream();
  return ::test::finish("test_arp_sequencer");
}
