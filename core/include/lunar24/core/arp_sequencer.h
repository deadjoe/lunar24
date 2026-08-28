// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-④ per-side arpeggiator + 16-step keyboard sequencer (design/00 §2g, design/06
// §P4, data flow design/07 §1 §3; @Claude mandate msg 8b19b72a).
//
// This engine sits BETWEEN the P4-① InputStateMachine (translate()) and the P4-③
// KeyboardBehaviour. translate() is the single place a normalized PerformanceInput
// becomes a canonical ControlEvent; KeyboardBehaviour is the single place a
// ControlEvent becomes per-sample pitch / pressure / gate. ArpSeq is an EVENT
// TRANSFORMER in that seam: it consumes the canonical ControlEvents (note-on/off
// chords, clock edges, sync/reset) from translate and emits a transformed stream of
// ControlEvents (gate_off / gate_on / pitch) for KeyboardBehaviour. It emits control
// signals only — never audio.
//
// The per-side arp/seq mode mux is keyboard.mode (registry id 101, frozen positions
// [keyboard, arpeggiator, sequencer], manual L814-818 L866). keyboard.behaviour
// (id 100) is the OTHER, orthogonal single/twin/split value-bank selector handled by
// keyboard_mode.h — a naming collision we resolve by keeping the two concepts in two
// headers.
//
// Per-side instantiation (mandate: "按侧实例化不搞全局单例"): there is NO global
// arp/seq object. Each side owns an ArpSeq instance, configured from its own value
// bank (read through read_side_scalar / the side-bank resolution choke point), so a
// split twin side can hold an arp while the other plays a plain keyboard. The GLOBAL
// clock tempo (keyboard.clock_bpm, id 129) is read once and shared — it is device
// tempo, not a per-side value.
//
// Honest boundaries (design/00 §3, §5) — do NOT treat these as evidence:
//   * The clock multiplication/division RATIO (arp_clock / seq_clock, ids 103 / 111)
//     has no evidenced values (manual L827 L875 only say "multiplication/division
//     ratio"). It is UN-RESOLVED: the selector is read as a raw index into the side's
//     keyboardClockSelectors[..] but NEVER applied as a numeric ratio. The engine
//     advances ONE step per incoming clock edge (nominal 1:1) pending evidence. This
//     is recorded in FINDINGS, not an invented ratio table.
//   * The rhythm GATE PATTERN (arp_rhythm / seq_rhythm, ids 107 / 114) is likewise
//     UN-RESOLVED (manual L851 L904: "applies a rhythmic pattern"; the pattern itself
//     is not enumerated). It is read as a raw selector but NOT applied; the engine
//     passes every clock edge through nominally.
//   * The arp note ORDERING — manual L817 says the arpeggiator "goes through the
//     sequence number of pressed plates". The plate -> sequence-number table is not
//     evidenced, so the chord is ordered by PITCH as a PROVISIONAL fallback and the
//     plate-sequence ordering is left UN-RESOLVED (FINDINGS).
//   * note_off / gate_off carries the SAME press identity as its note_on (GH#8), so
//     the arp chord is a fixed table of held-plate identities: note_on adds by identity,
//     gate_off deletes the EXACT matching identity (never a LIFO pop, so releasing a
//     middle/most-recent chord member in any order leaves the others intact).
//   * norm -> semitone/step maps (interval 1..12, arp length 1..8, seq length 2..16,
//     seq rhythm length 1..8) are documented linear ceilings (manual bounds the
//     ranges; no curve is evidenced). PROVISIONAL; the bpm -> step-Hz law is NOT
//     applied here — the sequencer advances on the incoming clock edge, and tempo is
//     the transport's concern, not this header's.
//
// These boundaries are deliberate: filling the division ratio, the rhythm pattern or
// the plate sequence number with invented values would conjure unevidenced facts.
// What IS engineered here, and what the tests pin, is the STRUCTURE: the mode mux,
// the pass-through, the chord/step advancement on the clock, and the per-side /
// no-global-singleton / side-drop-diverges properties.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include <lunar24/core/control_event.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// -------------------------------------------------------------- arp/seq mode mux --

// keyboard.mode (id 101), per side. Values match the frozen registry positions.
enum class ArpSeqMode : std::uint8_t {
  Keyboard = 0,  // transparent pass-through to KeyboardBehaviour
  Arpeggiator = 1,
  Sequencer = 2,
};

// Defensive decode: an unknown raw value degrades to the transparent keyboard case
// (a rogue selector must not conjure a phantom arp/seq).
constexpr ArpSeqMode arp_seq_mode(std::uint8_t m) noexcept {
  switch (m) {
    case 1: return ArpSeqMode::Arpeggiator;
    case 2: return ArpSeqMode::Sequencer;
    default: return ArpSeqMode::Keyboard;
  }
}

// One 16-step sequencer step (a white box mirror of the wire keyboard.seqSteps
// record; the caller maps its own KeyboardSeqStep here so this control-layer header
// stays decoupled from the device-state layout).
struct ArpSeqStep {
  std::uint8_t note = 0;  // note-plate value (semitone space)
  double value = 0.0;     // CV volts (step value)
  std::uint8_t gate = 0;  // step gate (0/1)
};

// The per-side arp/seq parameter set, resolved for ONE side by the caller through
// the side-context choke point (read_side_scalar + the side_bank resolution for the
// steps / clock selectors), the same way read_behaviour_params builds P4-③ params.
// Transport bpm is the GLOBAL clock_bpm, read once and shared across sides.
struct ArpSeqParams {
  std::uint8_t mode = 0;          // -> arp_seq_mode()
  std::uint8_t arpHold = 0;       // arp_hold (102)
  std::uint8_t arpDirection = 0;  // arp_direction (104): 0=forward,1=backward,2=ping-pong,3=random
  std::uint8_t arpVariation = 0;  // arp_variation (105): 0=off,1=x1,2=x2,3=x3
  double arpInterval = 0.0;       // arp_interval (106): norm -> 1..12 semitones (PROVISIONAL)
  double arpLength = 0.0;         // arp_length (108): norm -> 1..8 rhythm steps (PROVISIONAL)
  std::uint8_t seqRun = 0;        // seq_run (109): 0=free,1=keyboard
  double seqLength = 0.0;         // seq_length (110): norm -> 2..16 steps (PROVISIONAL)
  std::uint8_t seqDirection = 0;  // seq_direction (112): 0=forward,1=backward,2=ping-pong,3=random
  std::uint8_t seqCvOutput = 0;   // seq_cv_output (113): 0=continuous,1=gated
  double seqRhythmLength = 0.0;   // seq_rhythm_length (115): norm -> 1..8 (PROVISIONAL)
  // No-value-domain selectors (103 / 107 / 111 / 114). These live in the side's
  // keyboardClockSelectors[..]; their value domain is UN-RESOLVED (division ratio /
  // rhythm pattern not enumerated in the manual). Read as raw indices, NEVER applied
  // as a numeric ratio / pattern (see header FINDINGS note).
  std::uint8_t arpClock = 0;   // arp_clock (103)
  std::uint8_t arpRhythm = 0;  // arp_rhythm (107)
  std::uint8_t seqClock = 0;   // seq_clock (111)
  std::uint8_t seqRhythm = 0;  // seq_rhythm (114)
  std::array<ArpSeqStep, 16> steps{};  // this side's 16 sequencer steps
  double bpm = 120.0;                  // GLOBAL clock_bpm (129)
};

// Read ONE side's arp/seq params through the side-context choke point. Like
// read_behaviour_params (P4-③), the per-side SCALAR parameters are read through
// read_side_scalar — never from the global parameters[] — so a split-RIGHT side gets
// its own bank. `bpm` is the GLOBAL clock_bpm (one value per device). `steps` and
// `clockSelectors` are the non-scalar side paths (the seq steps bank and the 4-clock
// row from keyboardClockSelectors[..]); they are resolved by the caller from the
// side_bank() mirror, exactly as the scale editor is read in read_behaviour_params.
template <typename BankReader>
ArpSeqParams read_arp_seq_params(BankReader&& bank, KeyboardMode mode, KeyboardSide side,
                                 double bpm, std::array<ArpSeqStep, 16> steps,
                                 std::array<std::uint8_t, 4> clockSelectors) {
  ArpSeqParams p{};
  p.bpm = bpm;
  p.steps = steps;
  p.arpClock = clockSelectors[0];
  p.arpRhythm = clockSelectors[1];
  p.seqClock = clockSelectors[2];
  p.seqRhythm = clockSelectors[3];
  const auto rd = [&](ParameterId id) { return read_side_scalar(bank, mode, side, id); };
  p.mode           = static_cast<std::uint8_t>(rd(ParameterId::keyboard_mode));
  p.arpHold        = static_cast<std::uint8_t>(rd(ParameterId::keyboard_arp_hold));
  p.arpDirection   = static_cast<std::uint8_t>(rd(ParameterId::keyboard_arp_direction));
  p.arpVariation   = static_cast<std::uint8_t>(rd(ParameterId::keyboard_arp_variation));
  p.arpInterval    = rd(ParameterId::keyboard_arp_interval);
  p.arpLength      = rd(ParameterId::keyboard_arp_length);
  p.seqRun         = static_cast<std::uint8_t>(rd(ParameterId::keyboard_seq_run));
  p.seqLength      = rd(ParameterId::keyboard_seq_length);
  p.seqDirection   = static_cast<std::uint8_t>(rd(ParameterId::keyboard_seq_direction));
  p.seqCvOutput    = static_cast<std::uint8_t>(rd(ParameterId::keyboard_seq_cv_output));
  p.seqRhythmLength = rd(ParameterId::keyboard_seq_rhythm_length);
  return p;
}

// ------------------------------------------------- PROVISIONAL norm -> step maps --

// Documented linear ceilings for the ranges the manual bounds but does not curve.
// PROVISIONAL (design/00 §5 "先量后签") — do not cite as the real law.
inline std::uint8_t arp_interval_semitones(double norm) noexcept {  // 1..12
  if (norm <= 0.0) return 1;
  if (norm >= 1.0) return 12;
  return static_cast<std::uint8_t>(std::lround(1.0 + norm * 11.0));
}
inline std::uint8_t arp_length_steps(double norm) noexcept {  // 1..8
  if (norm <= 0.0) return 1;
  if (norm >= 1.0) return 8;
  return static_cast<std::uint8_t>(std::lround(1.0 + norm * 7.0));
}
inline std::uint8_t seq_length_steps(double norm) noexcept {  // 2..16
  if (norm <= 0.0) return 2;
  if (norm >= 1.0) return 16;
  return static_cast<std::uint8_t>(std::lround(2.0 + norm * 14.0));
}
inline std::uint8_t seq_rhythm_length_steps(double norm) noexcept {  // 1..8
  if (norm <= 0.0) return 1;
  if (norm >= 1.0) return 8;
  return static_cast<std::uint8_t>(std::lround(1.0 + norm * 7.0));
}

// The arp/seq event-transformer engine, ONE per keyboard side. It consumes the
// canonical ControlEvents from translate() and emits a transformed stream (gate_off /
// gate_on / pitch) for the downstream KeyboardBehaviour. In keyboard mode it passes
// every event through untouched; in arp/seq mode it intercepts note/chord + clock and
// generates the sequence.
class ArpSeq {
 public:
  void configure(const ArpSeqParams& p, double sample_rate) {
    params_ = p;
    fs_ = sample_rate;
    reset();
  }

  void reset() {
    chordSize_ = 0;
    arpIndex_ = 0;
    seqIndex_ = 0;
    seqBase_ = 0.0;   // track last state so reset also clears the running note
    seqBaseValid_ = false;
    runningGate_ = false;
    lastArpNoteId_ = 0;
    arpNoteId_ = 0;   // constructed notes get a fresh identity counter after reset
  }

  // mode() exposes the decoded mode so a caller can decide whether to drive a voice
  // (the arp/seq being engaged means the direct plate sound is produced here).
  ArpSeqMode mode() const { return arp_seq_mode(params_.mode); }
  double bpm() const { return params_.bpm; }

  // Feed one canonical event from translate(). `sink` receives each ControlEvent the
  // downstream KeyboardBehaviour should observe. In keyboard mode the event is
  // forwarded unchanged; in arp/seq mode the note/chord + clock stream is transformed.
  template <typename Sink>
  void handleControlEvent(const ControlEvent& ev, Sink&& sink) {
    switch (arp_seq_mode(params_.mode)) {
      case ArpSeqMode::Keyboard:
        std::forward<Sink>(sink)(ev);
        return;
      case ArpSeqMode::Arpeggiator:
        handleArp(ev, std::forward<Sink>(sink));
        return;
      case ArpSeqMode::Sequencer:
        handleSeq(ev, std::forward<Sink>(sink));
        return;
    }
  }

 private:
  static constexpr std::uint32_t kMaxChord = 12;  // the touch plates

  // One held plate of the chord, kept BY IDENTITY (source, channel, noteId) so a
  // release can delete EXACTLY the matching note — GH#8 kills the old LIFO stack where
  // releasing a middle note popped the last one instead (and a shared identity let two
  // overlapping notes from the same producer collide).
  struct ChordNote {
    ControlSourceId source = 0;
    std::uint8_t channel = 0;
    NoteId noteId = 0;
    double pitch = 0.0;
  };
  std::uint32_t chordSize() const { return chordSize_; }
  bool chordEmpty() const { return chordSize_ == 0; }
  // GH#8 over-capacity determinism: a pushed plate that finds the chord full is
  // REJECTED (counted, observable), never an out-of-bounds or silent overwrite.
  std::uint32_t chordOverflow() const { return chordOverflow_; }

  // Add (or re-pitch) a held plate by identity. A re-pitch of a note already in the
  // chord updates that note's pitch rather than appending a second record.
  void chordPush(const ControlEvent& ev) {
    for (std::uint32_t i = 0; i < chordSize_; ++i)
      if (chord_[i].source == ev.source && chord_[i].channel == ev.channel &&
          chord_[i].noteId == ev.noteId) {
        chord_[i].pitch = static_cast<double>(ev.value);  // re-pitch the held plate
        return;
      }
    if (chordSize_ >= kMaxChord) { ++chordOverflow_; return; }  // reject, observable
    ChordNote& n = chord_[chordSize_++];
    n.source = ev.source;
    n.channel = ev.channel;
    n.noteId = ev.noteId;
    n.pitch = static_cast<double>(ev.value);
  }
  // Delete the chord member matching the release identity (precise, no LIFO).
  void chordDelete(const ControlEvent& ev) {
    for (std::uint32_t i = 0; i < chordSize_; ++i)
      if (chord_[i].source == ev.source && chord_[i].channel == ev.channel &&
          chord_[i].noteId == ev.noteId) {
        for (std::uint32_t j = i; j + 1 < chordSize_; ++j) chord_[j] = chord_[j + 1];
        --chordSize_;
        return;
      }
  }

  // Forward a constructed event, preserving the source's transport fields AND the
  // GH#8 press identity. `noteId` carries the identity the downstream KeyboardBehaviour
  // keys its note state by.
  template <typename Sink>
  void emit_(Sink& sink, ControlEventKind kind, double value, const ControlEvent& src,
             NoteId noteId) {
    ControlEvent e{};
    e.kind = kind;
    e.value = static_cast<SignalSample>(value);
    e.sampleOffset = src.sampleOffset;
    e.source = src.source;
    e.channel = src.channel;
    e.noteId = noteId;
    e.producerSequence = src.producerSequence;
    sink(e);
  }
  template <typename Sink>
  void emitPitch_(Sink& sink, double pitch, NoteId id, const ControlEvent& src) {
    emit_(sink, ControlEventKind::pitch, pitch, src, id);
  }
  template <typename Sink>
  void emitGateOn_(Sink& sink, NoteId id, const ControlEvent& src) {
    emit_(sink, ControlEventKind::gate_on, 1.0, src, id);
  }
  template <typename Sink>
  void emitGateOff_(Sink& sink, NoteId id, const ControlEvent& src) {
    emit_(sink, ControlEventKind::gate_off, 0.0, src, id);
  }
  template <typename Sink>
  void emitReset_(Sink& sink, const ControlEvent& src) {
    emit_(sink, ControlEventKind::reset, 0.0, src, src.noteId);
  }

  // One new constructed note for KeyboardBehaviour: release the previously-sounding
  // constructed note (GH#8 identity), then state the new pitch and latch its gate.
  // Canonical dependency order (design/07 §3): the pitch target arrives (phase 1)
  // before the gate-on that latches it (phase 4) — never the old "wait for the next
  // pitch after gate-on" protocol. Each constructed note gets a fresh synthetic id.
  template <typename Sink>
  void emitNote(Sink& sink, double pitch_cv, bool previousGate, const ControlEvent& src) {
    NoteId id = ++arpNoteId_;
    if (previousGate) emitGateOff_(sink, lastArpNoteId_, src);
    emitPitch_(sink, pitch_cv, id, src);
    emitGateOn_(sink, id, src);
    lastArpNoteId_ = id;
    runningGate_ = true;
  }
  // Close the currently-sounding constructed note (if any) and clear the running gate.
  template <typename Sink>
  void emitRelease(Sink& sink, bool previousGate, const ControlEvent& src) {
    if (previousGate) emitGateOff_(sink, lastArpNoteId_, src);
    runningGate_ = false;
  }

  // -- arpeggiator ----------------------------------------------------------------
  template <typename Sink>
  void handleArp(const ControlEvent& ev, Sink& sink) {
    switch (ev.kind) {
      case ControlEventKind::pitch:
        // A pressed plate arrives as a pitch event carrying its press IDENTITY
        // (source/channel/noteId); the gate_on only carries the level and pressure is
        // not a chord member. Build the chord from pitch + identity.
        chordPush(ev);
        break;
      case ControlEventKind::gate_off:
        // Precise delete by the release identity (GH#8: no LIFO). HOLD persists the
        // chord through a release.
        if (params_.arpHold != 0) break;
        chordDelete(ev);
        if (chordEmpty()) emitRelease(sink, runningGate_, ev);
        break;
      case ControlEventKind::clock:
        stepArp(sink, ev);
        break;
      case ControlEventKind::sync:
      case ControlEventKind::reset:
        // A sync/reset clears the engine AND delivers a canonical reset downstream so
        // the KeyboardBehaviour truly re-arms (gate/pressure/vibrato/portamento).
        reset();
        emitReset_(sink, ev);
        break;
      default:
        break;  // gate_on / pressure / parameter are not chord members
    }
  }

  template <typename Sink>
  void stepArp(Sink& sink, const ControlEvent& ev) {
    if (chordEmpty()) return;
    // One gate/pitch per incoming clock edge at NOMINAL 1:1 — the division ratio is
    // UN-RESOLVED (see header FINDINGS); the arp_clock selector is not applied here.
    const double pitch = nextArpPitch();
    emitNote(sink, pitch, runningGate_, ev);
    ++arpIndex_;
  }

  // Select the next arp pitch from the held chord, by direction then interval and a
  // variation octave shift. The chord is ordered by PITCH (PROVISIONAL — the manual's
  // "sequence number of pressed plates" ordering is UN-RESOLVED, header FINDINGS).
  double nextArpPitch() const {
    const std::uint32_t n = chordSize_;
    std::uint32_t idx = 0;
    switch (params_.arpDirection % 4u) {
      case 1:  idx = (n > 0) ? (n - 1 - (arpIndex_ % n)) : 0; break;
      case 2:  idx = (arpIndex_ % (n * 2)); if (idx >= n) idx = (n * 2 - 1) - idx; break;
      case 3:  idx = arpIndex_ % n; break;  // random held as a deterministic walk (PROVISIONAL)
      default: idx = arpIndex_ % n; break;
    }
    const double semitone = static_cast<double>(arp_interval_semitones(params_.arpInterval));
    const double octave = static_cast<double>(params_.arpVariation);  // x0/x1/x2/x3 octaves
    return chord_[idx].pitch + (semitone + octave * 12.0) / 12.0;  // CV: 1 V/oct
  }

  // -- sequencer ---------------------------------------------------------------- --
  template <typename Sink>
  void handleSeq(const ControlEvent& ev, Sink& sink) {
    switch (ev.kind) {
      case ControlEventKind::pitch:
        // The pressing plates from note_on -> pitch form the transposition base
        // (manual L818: the sequencer is "transposed by the active note plate values").
        // Base = first held plate by insertion order (PROVISIONAL), tracked by identity.
        chordPush(ev);
        seqBase_ = chord_[0].pitch;
        seqBaseValid_ = true;
        break;
      case ControlEventKind::gate_off:
        chordDelete(ev);  // precise delete by identity, not a pop
        if (params_.seqRun != 0) {  // keyboard mode: stop when no plate held
          if (chordEmpty()) {
            seqBaseValid_ = false;
            emitRelease(sink, runningGate_, ev);
          } else {
            // Deterministic base = first REMAINING held plate, never a popped wrong note.
            seqBase_ = chord_[0].pitch;
          }
        }
        break;
      case ControlEventKind::clock:
        stepSeq(sink, ev);
        break;
      case ControlEventKind::sync:
      case ControlEventKind::reset:
        reset();
        emitReset_(sink, ev);
        break;
      default:
        break;
    }
  }

  template <typename Sink>
  void stepSeq(Sink& sink, const ControlEvent& ev) {
    if (params_.seqRun != 0 && !seqBaseValid_) return;  // keyboard mode: gated on plates
    const ArpSeqStep& st = params_.steps[seqIndex_ % params_.steps.size()];
    const double pitch = (seqBaseValid_ ? seqBase_ : 0.0) + static_cast<double>(st.note) / 12.0;

    const bool gated = params_.seqCvOutput == 0 ? true : (st.gate != 0);
    if (gated) emitNote(sink, pitch, runningGate_, ev);
    else       emitRelease(sink, runningGate_, ev);
    const std::uint32_t len = seq_length_steps(params_.seqLength);
    seqIndex_ = advanceIndex(seqIndex_, len, params_.seqDirection % 4u);
  }

  // Forward/backward/ping-pong index advance within a circular run of `len`.
  static std::uint32_t advanceIndex(std::uint32_t index, std::uint32_t len, std::uint32_t dir) {
    if (len == 0) return 0;
    switch (dir) {
      case 1: return (index == 0) ? (len - 1) : (index - 1);
      case 2: {  // ping-pong over 2*len
        const std::uint32_t m = index % (len * 2);
        return m < len ? m : (len * 2 - 1 - m);
      }
      default: return (index + 1) % len;
    }
  }

  ArpSeqParams params_{};
  double fs_ = 0.0;
  ChordNote chord_[kMaxChord];
  std::uint32_t chordSize_ = 0;
  std::uint32_t chordOverflow_ = 0;
  std::uint32_t arpIndex_ = 0;
  std::uint32_t seqIndex_ = 0;
  double seqBase_ = 0.0;
  bool seqBaseValid_ = false;
  bool runningGate_ = false;
  NoteId arpNoteId_ = 0;      // synthetic identity for each constructed note
  NoteId lastArpNoteId_ = 0;  // the currently-sounding constructed note's id
};

}  // namespace lunar24::core
