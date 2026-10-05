// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
//  per-side arp/seq engine tests (mandate).
//
// The contract this layer exists to uphold:
//   * it SITS between the InputStateMachine (translate) and the
//     KeyboardBehaviour, transforming canonical ControlEvents, never audio;
//   * there is NO global arp/seq singleton — each side owns an ArpSeq instance, so a
//     split twin side can arpeggiate while the other plays a plain keyboard;
//   * the arp/seq mode mux is keyboard.mode (id 101, positions
//     [keyboard/arpeggiator/sequencer]); keyboard mode is a transparent pass-through;
//   * GLOBAL tempo (clock_bpm, id 129) is shared; per-side division ratio and rhythm
//     pattern are UN-RESOLVED and read as raw selectors, NEVER applied numerically.
//
// The value maps (arp interval 1..12, seq length 2..16, etc.) are PROVISIONAL linear
// ceilings; the arp chord plays plates in plate-number order (the manual's "sequence number
// of pressed plates"), other notes in press order. Following the
//  test discipline, these tests pin the STRUCTURE — the mode mux, the chord/step
// advancement on the clock, the per-side independence, and the mandate-#4 divergence
// — not invented exact curves.
//
// Per (mandate #4) the negative is the real bypass path. Two are pinned here:
//   * the SIDE READ (read_side_scalar / side_bank resolution) — the genuine mistake it
//     prevents is reading the global/shared bank for a split-RIGHT side (forgetting the
//     right bank); a rogue reader that ignores the resolved side reads bank 0 and must
//     produce a divergent arp/seq control stream (side_drop, below);
//   * the NO-GLOBAL-SINGLETON hold — the two sides must not share mutable state. The
//     independence test holds DIFFERENT pitches on both sides (both in arpeggiator mode,
//     so both write the chord buffer); if the chord buffer were static/shared, left would
//     read the right side's pitch and emit the wrong note (see per_side_instantiation_
//     independent, Leg B).

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

// Feed the events translate would emit for a note_on chord member: the pitch (the
// note value the arp builds its chord from) and the gate_on, BOTH carrying the same
//  press identity (source/channel/noteId). `id` distinguishes overlapping notes
// from the same producer — two plates pressed together MUST carry different ids or the
// identity chord would treat the second as a re-pitch of the first.
void note_on(core::ArpSeq& s, Recorder& r, double pitch, core::NoteId id) {
  core::ControlEvent p{};
  p.kind = core::ControlEventKind::pitch;
  p.value = static_cast<core::SignalSample>(pitch);
  p.source = 1;
  p.noteId = id;
  s.handleControlEvent(p, r);
  core::ControlEvent g{};
  g.kind = core::ControlEventKind::gate_on;
  g.value = core::SignalSample{1.0};
  g.source = 1;
  g.noteId = id;
  s.handleControlEvent(g, r);
}

// A note from touch plate `plate` (0..11): same as note_on, with the plate number set.
void plate_on(core::ArpSeq& s, Recorder& r, double pitch, core::NoteId id, std::uint8_t plate) {
  core::ControlEvent p{};
  p.kind = core::ControlEventKind::pitch;
  p.value = static_cast<core::SignalSample>(pitch);
  p.source = 1;
  p.noteId = id;
  p.plate = plate;
  s.handleControlEvent(p, r);
}

void note_off(core::ArpSeq& s, Recorder& r, core::NoteId id) {
  core::ControlEvent g{};
  g.kind = core::ControlEventKind::gate_off;
  g.value = core::SignalSample{0.0};
  g.source = 1;
  g.noteId = id;
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
  note_on(s, r, 0.25, 1);
  clock_edge(s, r);
  clock_edge(s, r);
  note_off(s, r, 1);
  // The sink saw every event the engine was handed, none transformed.
  CHECK_EQ(r.n, 5u);  // gate_on+pitch, clock, clock, gate_off
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::clock), 2u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 1u);
}

// ------------------------------------------ release matches the sounding note --
// The arp latches its notes under the CLOCK's identity (source 9 here) but the last plate
// is released under the PLAYER's identity (source 1). The closing gate_off must carry the
// sounding note's identity, or KeyboardBehaviour (which keys notes by source, channel and
// noteId) never releases it and the arpeggio drones on after every key is let go.
static void arp_release_uses_the_sounding_notes_identity() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;  // arpeggiator
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0, 1);
  note_on(s, r, 0.25, 2);
  clock_edge(s, r);              // a note sounds, latched under the clock's source
  core::ControlEvent on{};
  for (std::uint32_t i = 0; i < r.n; ++i)
    if (r.ev[i].kind == core::ControlEventKind::gate_on) on = r.ev[i];
  CHECK_EQ(on.source, 9u);
  note_off(s, r, 1);
  note_off(s, r, 2);             // last plate released while the note sounds
  const core::ControlEvent& off = r.ev[r.n - 1];
  CHECK_TRUE(off.kind == core::ControlEventKind::gate_off);
  CHECK_EQ(off.noteId, on.noteId);
  CHECK_EQ(off.source, on.source);  // the old code sent source 1: a stuck note
  CHECK_EQ(off.channel, on.channel);
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
  // Distinct noteIds (1, 2) mark two overlapping presses of the same source.
  note_on(s, r, 0.0 / 12.0, 1);   // C
  note_on(s, r, 4.0 / 12.0, 2);   // E
  CHECK_EQ(r.n, 0u);           // arp mode does NOT sound the chord directly

  clock_edge(s, r);  // 1st note
  clock_edge(s, r);  // 2nd note
  clock_edge(s, r);  // 3rd note (wraps)
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 3u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 3u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 2u);  // between consecutive notes

  // First emitted pair is pitch+gate_on over chord[0] (canonical dependency order:
  // the pitch target arrives before the gate that latches it); no leading gate_off.
  CHECK_TRUE(r.ev[0].kind == core::ControlEventKind::pitch);
  CHECK_TRUE(r.ev[1].kind == core::ControlEventKind::gate_on);
  // Forward direction over chord [C, E] with VARIATION off: C, E, C (wraps).
  CHECK_TRUE(r.near(r.pitchAt(0), 0.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(2), 0.0 / 12.0));
}

// Manual p.16: VARIATION x1 plays the progression again transposed by INTERVAL.
static void arp_variation_repeats_transposed() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  p.arpVariation = 1;   // x1
  p.arpInterval = 1.0;  // 12 semitones
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  note_on(s, r, 4.0 / 12.0, 2);
  for (int i = 0; i < 5; ++i) clock_edge(s, r);
  CHECK_TRUE(r.near(r.pitchAt(0), 0.0));
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(2), 1.0));
  CHECK_TRUE(r.near(r.pitchAt(3), 1.0 + 4.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(4), 0.0));
}

// Manual p.16: RHYTHM mutes some clock edges. Length 4 with step 2 muted: of 8 edges,
// 6 play, and the arpeggio does not advance on the muted ones.
static void arp_rhythm_mutes_steps() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  p.arpLength = 3.0 / 7.0;  // 4 steps
  p.arpRhythm = 0x02;       // step 2 (of 1..4) muted
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  note_on(s, r, 4.0 / 12.0, 2);
  for (int i = 0; i < 8; ++i) clock_edge(s, r);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 6u);
  CHECK_TRUE(r.near(r.pitchAt(0), 0.0));        // edge 1
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0)); // edge 3 (edge 2 muted)
}

// The RESET jack restarts the arpeggio at its first note and keeps the held chord.
static void arp_restart_keeps_chord() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  note_on(s, r, 4.0 / 12.0, 2);
  note_on(s, r, 7.0 / 12.0, 3);
  clock_edge(s, r);  // C
  clock_edge(s, r);  // E
  s.restartPattern();
  clock_edge(s, r);  // C again, not G
  clock_edge(s, r);  // E
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 4u);
  CHECK_TRUE(r.near(r.pitchAt(2), 0.0));
  CHECK_TRUE(r.near(r.pitchAt(3), 4.0 / 12.0));
}

static void arp_hold_keeps_chord_through_release() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  p.arpHold = 1;  // HOLD
  core::ArpSeq s;
  s.configure(p, 48000);

  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  note_off(s, r, 1);  // released, but HOLD keeps the chord
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 0u);  // no release emitted
  clock_edge(s, r);  // still arpeggiates
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
}

// Turning HOLD off drops the plates let go while it was on and keeps the ones still held.
static void arp_hold_off_drops_released_plates() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  p.arpHold = 1;
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);  // C
  note_on(s, r, 4.0 / 12.0, 2);  // E
  note_off(s, r, 1);             // C let go, HOLD keeps it
  p.arpHold = 0;
  s.configure(p, 48000, r);      // HOLD off: C leaves the chord, E is still held
  CHECK_EQ(r.n, 0u);             // nothing was sounding, nothing to release
  clock_edge(s, r);
  clock_edge(s, r);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 2u);
  CHECK_TRUE(r.near(r.pitchAt(0), 4.0 / 12.0));
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0));
}

// Turning HOLD off with every plate let go stops the arpeggio and releases its note.
static void arp_hold_off_with_no_plate_held_stops() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpHold = 1;
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  note_off(s, r, 1);
  clock_edge(s, r);  // the held chord is sounding
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
  const core::NoteId sounding = r.ev[r.n - 1].noteId;
  p.arpHold = 0;
  s.configure(p, 48000, r);
  CHECK_EQ(r.count(core::ControlEventKind::gate_off), 1u);
  CHECK_TRUE(r.ev[r.n - 1].kind == core::ControlEventKind::gate_off);
  CHECK_EQ(r.ev[r.n - 1].noteId, sounding);
  clock_edge(s, r);  // the arpeggio has stopped
  clock_edge(s, r);
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 1u);
}

// Manual p.16: the arpeggio goes through the pressed plates by plate number, not press order
// and not pitch: a retuned plate keeps its place. MIDI notes (no plate) follow, in press order.
static void arp_orders_plates_by_number() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 0;
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  plate_on(s, r, 7.0 / 12.0, 1, 7);    // G pressed first
  note_on(s, r, 2.0, 9);               // a MIDI note (no plate)
  plate_on(s, r, 14.0 / 12.0, 2, 0);   // plate C, tuned up above G
  plate_on(s, r, 4.0 / 12.0, 3, 4);    // E
  plate_on(s, r, 11.0 / 12.0, 4, 11);  // B, the highest plate, pressed after the MIDI note
  for (int i = 0; i < 5; ++i) clock_edge(s, r);
  CHECK_TRUE(r.near(r.pitchAt(0), 14.0 / 12.0));  // plate 0 first, whatever its pitch
  CHECK_TRUE(r.near(r.pitchAt(1), 4.0 / 12.0));   // plate 4
  CHECK_TRUE(r.near(r.pitchAt(2), 7.0 / 12.0));   // plate 7
  CHECK_TRUE(r.near(r.pitchAt(3), 11.0 / 12.0));  // plate 11
  CHECK_TRUE(r.near(r.pitchAt(4), 2.0));          // then the MIDI note
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
  note_on(s, r, 0.0 / 12.0, 1);  // transposition base C
  CHECK_EQ(r.n, 0u);             // seq does not sound the plate directly

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

static void seq_gate_off_step_is_a_rest() {
  core::ArpSeqParams p = base_params();
  p.mode = 2;
  p.seqRun = 0;
  p.seqCvOutput = 0;  // continuous CV output: the step gate still decides the gate
  p.seqLength = 0.0;
  p.steps[0].note = 0; p.steps[0].gate = 0;  // step 0 has gate flag 0
  core::ArpSeq s;
  s.configure(p, 48000);

  Recorder r;
  note_on(s, r, 0.0 / 12.0, 1);
  clock_edge(s, r);  // step 0 has its gate off -> a rest: no new note
  CHECK_EQ(r.count(core::ControlEventKind::gate_on), 0u);
  // Continuous: the rest step still moves the CV, as a pitch with no note.
  CHECK_EQ(r.count(core::ControlEventKind::pitch), 1u);

  // Gated: a rest holds the last pitch (no pitch event at all).
  p.seqCvOutput = 1;
  core::ArpSeq g;
  g.configure(p, 48000);
  Recorder rg;
  note_on(g, rg, 0.0 / 12.0, 1);
  clock_edge(g, rg);
  CHECK_EQ(rg.count(core::ControlEventKind::gate_on), 0u);
  CHECK_EQ(rg.count(core::ControlEventKind::pitch), 0u);
}

// Ping-pong turns at both ends without repeating them: LENGTH 4 plays 1 2 3 4 3 2 1 2 3 4.
static void seq_ping_pong_turns_at_both_ends() {
  core::ArpSeqParams p = base_params();  // step i plays note i
  p.mode = 2;
  p.seqRun = 0;
  p.seqDirection = 2;
  p.seqLength = 2.0 / 14.0;  // 4 steps
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  for (int i = 0; i < 10; ++i) clock_edge(s, r);
  const int want[10] = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3};
  for (std::uint32_t i = 0; i < 10; ++i)
    CHECK_TRUE(r.near(r.pitchAt(i), want[i] / 12.0));
}

// Random picks steps in no fixed order, within LENGTH, and the same run repeats exactly.
static void seq_random_is_shuffled_and_repeatable() {
  core::ArpSeqParams p = base_params();
  p.mode = 2;
  p.seqRun = 0;
  p.seqDirection = 3;
  p.seqLength = 6.0 / 14.0;  // 8 steps
  core::ArpSeq a, b;
  a.configure(p, 48000);
  b.configure(p, 48000);
  Recorder ra, rb;
  for (int i = 0; i < 16; ++i) { clock_edge(a, ra); clock_edge(b, rb); }
  bool forward = true;
  std::uint32_t seen = 0;
  for (std::uint32_t i = 0; i < 16; ++i) {
    const double v = ra.pitchAt(i) * 12.0;
    const auto step = static_cast<std::uint32_t>(std::lround(v));
    CHECK_TRUE(step < 8u);
    seen |= 1u << step;
    if (step != i % 8u) forward = false;
    CHECK_TRUE(ra.near(ra.pitchAt(i), rb.pitchAt(i)));
  }
  CHECK_TRUE(!forward);
  std::uint32_t distinct = 0;
  for (std::uint32_t k = 0; k < 8; ++k) distinct += (seen >> k) & 1u;
  CHECK_TRUE(distinct >= 5u);  // the old one-multiply hash played one step 16 times
}

// Arp random also moves around the chord from the first steps (same hash).
static void arp_random_moves_around_the_chord() {
  core::ArpSeqParams p = base_params();
  p.mode = 1;
  p.arpDirection = 3;
  core::ArpSeq s;
  s.configure(p, 48000);
  Recorder r;
  for (core::NoteId i = 1; i <= 4; ++i) note_on(s, r, i / 12.0, i);
  for (int i = 0; i < 8; ++i) clock_edge(s, r);
  std::uint32_t seen = 0;
  for (std::uint32_t i = 0; i < 8; ++i)
    seen |= 1u << static_cast<std::uint32_t>(std::lround(r.pitchAt(i) * 12.0));
  CHECK_TRUE(seen != 0x2u && seen != 0x4u && seen != 0x8u && seen != 0x10u);
}

// ----------------------------------------------- per-side, no global singleton --

static void per_side_instantiation_independent() {
  // Two sides of the same device. Each owns its own ArpSeq instance (no shared global).
  // Two independent legs, because they test two different properties:
  //
  //   Leg A (mode routing, L724 visual): LEFT arpeggiates, RIGHT is a plain keyboard.
  //     Different *settings* -> different output. This is the "split twin: one side
  //     arpeggiates, the other plays a keyboard" routing concern.
  //   Leg B (STATE isolation, the global-singleton negative): the two instances must
  //     not share any mutable state. This is what makes a `static` hold buffer visible
  //     (mutation: `double chord_[..]` -> `static inline` so ALL ArpSeq share
  //     ONE chord buffer). Both sides must WRITE that buffer and they must hold
  //     DIFFERENT pitches. If both held the same pitch a shared buffer would write the
  //     same value at index 0 and the corruption would be masked — the classic "test
  //     the property with an input that can't expose it" falseness. With left holding C
  //     and right holding G, a shared buffer makes left read G on its next clock and
  //     emit G+interval instead of C+interval (red).
  {
    // Leg A: LEFT arpeggiator, RIGHT keyboard passthrough.
    core::ArpSeqParams left = base_params();
    left.mode = 1;  // arpeggiator
    left.arpDirection = 0;
    core::ArpSeqParams right = base_params();
    right.mode = 0;  // keyboard

    core::ArpSeq sl, sr;
    sl.configure(left, 48000);
    sr.configure(right, 48000);

    Recorder rl, rr;
    note_on(sl, rl, 0.0 / 12.0, 1);
    note_on(sr, rr, 0.0 / 12.0, 1);
    clock_edge(sl, rl);
    clock_edge(sr, rr);

    CHECK_EQ(rl.count(core::ControlEventKind::pitch), 1u);   // left arpeggiates
    CHECK_EQ(rl.count(core::ControlEventKind::gate_on), 1u);
    CHECK_EQ(rr.count(core::ControlEventKind::pitch), 1u);   // right: plate pitch passthrough
    CHECK_EQ(rr.count(core::ControlEventKind::gate_on), 1u);
    CHECK_TRUE(rl.near(rl.pitchAt(0), 0.0));
    CHECK_TRUE(rr.near(rr.pitchAt(0), 0.0));
  }
  {
    // Leg B: BOTH arpeggiators, holding DIFFERENT pitches, so a shared hold buffer is
    // corrupted by the second side and the first side's next note is wrong. This is the
    // negative that catches a global (static) chord buffer.
    core::ArpSeqParams left = base_params();
    left.mode = 1;  // arpeggiator (writes the chord buffer)
    left.arpDirection = 0;
    core::ArpSeqParams right = base_params();
    right.mode = 1;  // arpeggiator (writes the chord buffer)
    right.arpDirection = 0;

    core::ArpSeq sl, sr;
    sl.configure(left, 48000);
    sr.configure(right, 48000);

    Recorder rl, rr;
    note_on(sl, rl, 0.0 / 12.0, 1);   // LEFT holds C (0 V)
    note_on(sr, rr, 7.0 / 12.0, 1);   // RIGHT holds G (+7 semitones) — DIFFERENT pitch
    clock_edge(sl, rl);
    clock_edge(sr, rr);

    // LEFT (correct) = C; RIGHT (correct) = G.
    CHECK_TRUE(rl.near(rl.pitchAt(0), 0.0));
    CHECK_TRUE(rr.near(rr.pitchAt(0), 7.0 / 12.0));
    // The two streams differ — a shared chord buffer makes left read chord_[0]==G and
    // emit G instead of C, so the equal-pitch values are the tell.
    CHECK_TRUE(!rl.near(rl.pitchAt(0), rr.pitchAt(0)));
  }
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
  note_on(cs, rc, 0.0 / 12.0, 1);
  note_on(rs, rr, 0.0 / 12.0, 1);
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
  arp_release_uses_the_sounding_notes_identity();
  arp_emits_one_note_per_clock();
  arp_hold_keeps_chord_through_release();
  arp_hold_off_drops_released_plates();
  arp_hold_off_with_no_plate_held_stops();
  arp_variation_repeats_transposed();
  arp_rhythm_mutes_steps();
  arp_restart_keeps_chord();
  seq_advances_steps_and_gates();
  seq_gate_off_step_is_a_rest();
  seq_ping_pong_turns_at_both_ends();
  seq_random_is_shuffled_and_repeatable();
  arp_random_moves_around_the_chord();
  arp_orders_plates_by_number();
  per_side_instantiation_independent();
  side_drop_produces_divergent_stream();
  return ::test::finish("test_arp_sequencer");
}
