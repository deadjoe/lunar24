// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-① tests for the unified performance-input state machine + three-path
// equivalence (design/06 §P4, design/07 §1, §3). @Claude's mandate + our scope:
//
//   P4 exit condition: the same control sequence, entered from the panel touch
//   plates, the computer keyboard, or MIDI, yields the SAME internal CV/gate/
//   clock per sample. The three sources are thin adapters that only translate a
//   native message into a PerformanceInput; the InputStateMachine is the SINGLE
//   place a normalized input becomes a canonical ControlEvent.
//
// The three-path equivalence is STRUCTURALLY guaranteed, so the positive test is
// a sanity/regression check and the information is in the negation: a path that
// BYPASSES the shared state machine and interprets its own message produces a
// different internal stream. @Claude was explicit that the negative must be a
// REAL bypass (a code path that constructs ControlEvents directly, never calling
// translate()), not a stub with a changed return value. Every negative here is
// that: a rogue performer that re-interprets the native message itself. Each is
// a genuine, representative error the shared choke point exists to prevent.
//
// @Claude's guards, one test each:
//   * test-1 three-path equivalence (+ absolute anchor) + bypass negative
//   * test-2 velocity/aftertouch fold to pressure ONLY
//   * test-3 MIDI clock maps to a clock edge ONLY
//   * test-4 CC-learn is a data-driven controller -> ParameterId map
//   * test-5 reuse the existing EventTimebase (buffer-invariant), don't reinvent
//
// Honest framing (design/00-status §4/§5): this is CORE only. The app-side
// readers (iPlug2 / IGraphics) that turn a native touch/key/MIDI message into an
// already-normalized 1V/oct PerformanceInput are out of scope and out of this
// test; the adapters here are framework-free stubs that differ only in the
// provenance stamp they attach. The per-sample render below is observation-only
// for this judge — the real keyboard VOICE render is P4-②, not built here.

#include "mini_test.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <lunar24/core/control_event.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/input_state_machine.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------- constants ----

static const core::ControlSourceId kPanel = 10;
static const core::ControlSourceId kKey = 11;
static const core::ControlSourceId kMidi = 12;
static const core::ControlSourceId kRogue = 99;
static const core::ParameterId kCutoffParam = core::ParameterId{42};
static const core::ParameterId kResParam = core::ParameterId{87};

// A data-driven CC-learn map: controller 1 -> cutoff, controller 74 -> resonance.
static const core::CcBinding kBindings[] = {
    {1, kCutoffParam},
    {74, kResParam},
};
static const std::uint32_t kBindingCount =
    static_cast<std::uint32_t>(sizeof(kBindings) / sizeof(kBindings[0]));

// ---------------------------------------------------------------- vocabulary ----

// What a NATIVE source delivers, already pitch-normalized to 1V/oct. The app-side
// reader (iPlug2 / IGraphics) does that normalization; core's contract starts at
// PerformanceInput (design/07 §1). adapter() below is the framework-free stub.
struct NativeMsg {
  core::PerfInputKind kind;
  std::uint64_t sample;
  double pitch;       // 1V/oct for note_on
  double value;       // velocity / pressure / cc value
  std::uint16_t controller;
};

// The three adapters differ ONLY in the provenance stamp (source + channel): the
// messages they deliver are the same semantic performance. This is the funnel:
// three native worlds collapse into one normalized vocabulary.
static std::vector<core::PerformanceInput> adopt(const std::vector<NativeMsg>& seq,
                                                 core::ControlSourceId source,
                                                 std::uint8_t channel) {
  std::vector<core::PerformanceInput> out;
  out.reserve(seq.size());
  std::uint64_t seqn = 0;
  for (const auto& m : seq) {
    core::PerformanceInput p{};
    p.kind = m.kind;
    p.sample = m.sample;
    p.pitch = core::SignalSample(m.pitch);
    p.value = core::SignalSample(m.value);
    p.controller = m.controller;
    p.channel = channel;
    p.source = source;
    p.seq = seqn++;
    out.push_back(p);
  }
  return out;
}

// A rendered peek at the internal CV/gate/clock AFTER the state machine has
// translated and the events have been admitted to core. Only the lanes the P4
// exit condition names (CV/gate/clock) are observed; parameter/continuous events
// do not touch a frame lane and so are invisible to this judge.
struct Frame {
  double gate;
  double pitch;
  double pressure;
  double clock;
};

using Frames = std::vector<Frame>;

// Render a TimedControlEvent stream (each with an ABSOLUTE sample) straight to
// per-sample frames. This is the reference absolute-timing view.
static Frames render_absolute(const std::vector<core::TimedControlEvent>& evts,
                              std::uint64_t total) {
  auto sorted = evts;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const core::TimedControlEvent& a, const core::TimedControlEvent& b) {
                     return core::timed_event_before(a, b);
                   });
  Frames f(total);
  double gate = 0, pitch = 0, pressure = 0;
  std::size_t i = 0;
  for (std::uint64_t s = 0; s < total; ++s) {
    while (i < sorted.size() && sorted[i].sample == s) {
      switch (sorted[i].event.kind) {
        case core::ControlEventKind::gate_on: gate = 1; break;
        case core::ControlEventKind::gate_off: gate = 0; break;
        case core::ControlEventKind::pitch:     pitch = sorted[i].event.value; break;
        case core::ControlEventKind::pressure:  pressure = sorted[i].event.value; break;
        case core::ControlEventKind::reset:     gate = 0; pitch = 0; pressure = 0; break;
        default: break;
      }
      ++i;
    }
    f[s].gate = gate;
    f[s].pitch = pitch;
    f[s].pressure = pressure;
    f[s].clock = 0;
  }
  for (const auto& e : sorted)
    if (e.event.kind == core::ControlEventKind::clock && e.sample < total)
      f[e.sample].clock = 1;  // a 1-sample pulse — the edge, not a held level
  return f;
}

// The state-machine path: translate() the normalized inputs into canonical
// events, then render. This is the CONFORMING pipeline.
static Frames compliant_frames(const std::vector<core::PerformanceInput>& in,
                               const core::InputStateMachine& sm, std::uint64_t total) {
  std::vector<core::TimedControlEvent> stream;
  for (const auto& pi : in) {
    core::ControlEvent evs[3];
    std::uint32_t n = sm.translate(pi, evs, 3);
    for (std::uint32_t i = 0; i < n; ++i) {
      core::TimedControlEvent te{};
      te.event = evs[i];
      te.sample = pi.sample;
      stream.push_back(te);
    }
  }
  return render_absolute(stream, total);
}

static bool frames_equal(const Frames& a, const Frames& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].gate != b[i].gate || a[i].pitch != b[i].pitch ||
        a[i].pressure != b[i].pressure || a[i].clock != b[i].clock)
      return false;
  return true;
}

// Route an absolute-event stream through the EXISTING EventTimebase scheduler
// under the given block partition, then render. The timebase is the load-bearing
// absolute-timing path P4 inherits; this is just the observability window on top
// of it (the real keyboard VOICE render is P4-②, out of scope here).
static Frames render_through_timebase(const std::vector<core::TimedControlEvent>& evts,
                                      std::uint64_t total,
                                      const std::vector<std::uint32_t>& blocks) {
  core::EventTimebase tb;
  for (const auto& e : evts) (void)tb.enqueue(e);  // small stream; capacity is ample
  Frames f(total);
  double gate = 0, pitch = 0, pressure = 0;
  std::uint64_t abs = 0;
  std::size_t b = 0;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  while (abs < total) {
    std::uint32_t blk = blocks[b % blocks.size()];
    if (static_cast<std::uint64_t>(blk) > total - abs)
      blk = static_cast<std::uint32_t>(total - abs);
    const std::uint32_t n = tb.processBlock(blk, out, core::kEventTimebaseCapacity);
    std::vector<core::TimedControlEvent> evc(out, out + n);
    std::stable_sort(evc.begin(), evc.end(),
                     [](const core::TimedControlEvent& a, const core::TimedControlEvent& b) {
                       return core::control_event_before(a.event, b.event);
                     });
    std::size_t i = 0;
    for (std::uint32_t s = 0; s < blk; ++s) {
      while (i < evc.size() && evc[i].event.sampleOffset == s) {
        switch (evc[i].event.kind) {
          case core::ControlEventKind::gate_on: gate = 1; break;
          case core::ControlEventKind::gate_off: gate = 0; break;
          case core::ControlEventKind::pitch:     pitch = evc[i].event.value; break;
          case core::ControlEventKind::pressure:  pressure = evc[i].event.value; break;
          case core::ControlEventKind::reset:     gate = 0; pitch = 0; pressure = 0; break;
          case core::ControlEventKind::clock:     f[abs + s].clock = 1; break;
          default: break;
        }
        ++i;
      }
      f[abs + s].gate = gate;
      f[abs + s].pitch = pitch;
      f[abs + s].pressure = pressure;
    }
    abs += blk;
    ++b;
  }
  return f;
}

// A block-LAZY timebase (test 5's negative): it re-derives timing from the
// partition, applying every event due within a block at that block's FIRST
// sample. Edge precision is discarded, so an event not on a block boundary drifts
// relative to the absolute reference — the exact degradation EventTimebase was
// built to prevent. It must be caught by the buffer-invariance judge.
static Frames render_block_lazy(const std::vector<core::TimedControlEvent>& evts,
                                std::uint64_t total,
                                const std::vector<std::uint32_t>& blocks) {
  auto sorted = evts;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const core::TimedControlEvent& a, const core::TimedControlEvent& b) {
                     return core::timed_event_before(a, b);
                   });
  Frames f(total);
  double gate = 0, pitch = 0, pressure = 0;
  std::uint64_t abs = 0;
  std::size_t b = 0, i = 0;
  while (abs < total) {
    std::uint32_t blk = blocks[b % blocks.size()];
    if (static_cast<std::uint64_t>(blk) > total - abs)
      blk = static_cast<std::uint32_t>(total - abs);
    const std::uint64_t blockEnd = abs + blk;
    bool firedClock = false;
    while (i < sorted.size() && sorted[i].sample < blockEnd) {
      switch (sorted[i].event.kind) {
        case core::ControlEventKind::gate_on: gate = 1; break;
        case core::ControlEventKind::gate_off: gate = 0; break;
        case core::ControlEventKind::pitch:     pitch = sorted[i].event.value; break;
        case core::ControlEventKind::pressure:  pressure = sorted[i].event.value; break;
        case core::ControlEventKind::reset:     gate = 0; pitch = 0; pressure = 0; break;
        case core::ControlEventKind::clock:     firedClock = true; break;  // deferred to block start
        default: break;
      }
      ++i;
    }
    for (std::uint32_t s = 0; s < blk; ++s) {
      f[abs + s].gate = gate;
      f[abs + s].pitch = pitch;
      f[abs + s].pressure = pressure;
      f[abs + s].clock = (s == 0 && firedClock) ? 1 : 0;
    }
    abs += blk;
    ++b;
  }
  return f;
}

// A hardcoded-branch CC mapper (test 4's negative): it ignores the data-driven
// learn map and routes ANY controller to an arbitrary fixed target. It is only
// used to prove the "no phantom capability" drop is load-bearing — the real path,
// fed an unbound controller, returns 0 while this fabricated one returns 1.
static std::uint32_t hardcoded_branch_cc(const core::PerformanceInput& in,
                                         core::ControlEvent* out, std::uint32_t capacity) {
  if (capacity < 1u) return 0;
  out[0].kind = core::ControlEventKind::parameter;
  out[0].parameter = core::ParameterId{13};  // arbitrary fabricated target
  out[0].value = in.value;
  out[0].source = in.source;
  out[0].producerSequence = in.seq;
  return 1;
}

// ------------------------------------------------------------------ the sequence -

// One canonical performed phrase, exercised identically from all three sources.
// Absolute samples chosen so the downstream partition boundary never coincides
// with an event (block splits land mid-hold, not on an edge) — otherwise a
// buffer-invariant check could pass for the wrong reason. All values are exact
// binary fractions so the float CV lanes compare exactly.
static const std::vector<NativeMsg>& phrase() {
  static const std::vector<NativeMsg> s = {
      {core::PerfInputKind::note_on, 10, 1.0, 0.5, 0},
      {core::PerfInputKind::aftertouch, 24, 0.0, 0.75, 0},
      {core::PerfInputKind::note_off, 32, 0.0, 0.0, 0},
      {core::PerfInputKind::note_on, 40, 2.0, 0.5, 0},
      {core::PerfInputKind::clock, 48, 0.0, 1.0, 0},
      {core::PerfInputKind::clock, 56, 0.0, 1.0, 0},
      {core::PerfInputKind::cc, 60, 0.0, 0.25, 1},  // bound controller 1 -> cutoff
      {core::PerfInputKind::note_off, 64, 0.0, 0.0, 0},
  };
  return s;
}

static std::uint64_t phrase_total() {
  std::uint64_t m = 0;
  for (const auto& x : phrase())
    if (x.sample > m) m = x.sample;
  return m + 1;
}

// -------------------------------------------------rogue (the real bypass) --------

enum class Rogue : std::uint8_t {
  pitch_double,        // re-interprets an already-1V/oct pitch as needing 2x
  velocity_to_cutoff,  // routes note velocity to a cutoff target, not pressure
  clock_to_pitch,      // mis-routes a MIDI clock edge as a pitch cell
};

// A rogue performer that BYPASSES InputStateMachine: it builds ControlEvents
// directly from the native messages and never calls translate(). The three modes
// are distinct, real mis-interpretations a second interpretation point would
// introduce. Each must produce a frame the conforming path does not.
static std::vector<core::TimedControlEvent> rogue_stream(const std::vector<NativeMsg>& seq,
                                                         Rogue mode) {
  std::vector<core::TimedControlEvent> out;
  std::uint64_t seqn = 0;
  for (const auto& m : seq) {
    core::ControlEvent e{};
    e.source = kRogue;
    e.producerSequence = seqn++;
    switch (m.kind) {
      case core::PerfInputKind::note_on: {
        core::ControlEvent g = e; g.kind = core::ControlEventKind::gate_on;
        g.value = core::SignalSample{1};
        out.push_back({g, m.sample});
        core::ControlEvent p = e; p.kind = core::ControlEventKind::pitch;
        p.value = core::SignalSample((mode == Rogue::pitch_double) ? m.pitch * 2.0 : m.pitch);
        out.push_back({p, m.sample});
        if (mode == Rogue::velocity_to_cutoff) {
          core::ControlEvent c = e; c.kind = core::ControlEventKind::parameter;
          c.parameter = kCutoffParam; c.value = core::SignalSample(m.value);
          out.push_back({c, m.sample});
        } else {
          core::ControlEvent pr = e; pr.kind = core::ControlEventKind::pressure;
          pr.value = core::SignalSample(m.value);
          out.push_back({pr, m.sample});
        }
        break;
      }
      case core::PerfInputKind::note_off: {
        core::ControlEvent g = e; g.kind = core::ControlEventKind::gate_off;
        g.value = core::SignalSample{0};
        out.push_back({g, m.sample});
        break;
      }
      case core::PerfInputKind::clock: {
        if (mode == Rogue::clock_to_pitch) {
          core::ControlEvent p = e; p.kind = core::ControlEventKind::pitch;
          p.value = core::SignalSample(m.value);
          out.push_back({p, m.sample});
        } else {
          core::ControlEvent c = e; c.kind = core::ControlEventKind::clock;
          c.value = core::SignalSample(m.value);
          out.push_back({c, m.sample});
        }
        break;
      }
      case core::PerfInputKind::aftertouch: {
        core::ControlEvent pr = e; pr.kind = core::ControlEventKind::pressure;
        pr.value = core::SignalSample(m.value);
        out.push_back({pr, m.sample});
        break;
      }
      case core::PerfInputKind::cc: {
        // A bound cc still yields a parameter event; cc touches no frame lane, so
        // it cannot contaminate the frame-difference verdict.
        core::ControlEvent c = e; c.kind = core::ControlEventKind::parameter;
        c.parameter = kCutoffParam; c.value = core::SignalSample(m.value);
        out.push_back({c, m.sample});
        break;
      }
    }
  }
  return out;
}

static Frames rogue_frames(const std::vector<NativeMsg>& seq, Rogue mode, std::uint64_t total) {
  return render_absolute(rogue_stream(seq, mode), total);
}

// ------------------------------------------------------------------- test 1 -----

// The P4 exit condition: the same phrase from panel / computer-keyboard / MIDI
// yields an identical internal CV/gate/clock, per sample. Plus an absolute anchor
// (known values at known samples) so the identity is not "consistent but wrong."
static void three_path_equivalence_and_bypass() {
  const core::InputStateMachine sm(kBindings, kBindingCount);
  const std::uint64_t total = phrase_total();

  const auto panel = compliant_frames(adopt(phrase(), kPanel, 0), sm, total);
  const auto keyboard = compliant_frames(adopt(phrase(), kKey, 1), sm, total);
  const auto midi = compliant_frames(adopt(phrase(), kMidi, 2), sm, total);

  // Three paths -> identical internal result. (Source id is only a tiebreak in
  // same-sample ordering and never leaks into a rendered lane.)
  CHECK_TRUE(frames_equal(panel, keyboard));
  CHECK_TRUE(frames_equal(panel, midi));

  // Absolute anchor: this is the value the CV/gate/clock holds, not just "the same
  // across sources." All lanes are exact binary fractions.
  CHECK_EQ(panel[5].gate, 0.0);                 // silent before the first note
  CHECK_EQ(panel[10].gate, 1.0);                // note latched at its sample
  CHECK_EQ(panel[10].pitch, 1.0);               // 1V/oct
  CHECK_EQ(panel[10].pressure, 0.5);            // velocity folded to pressure
  CHECK_EQ(panel[24].pressure, 0.75);           // aftertouch fold
  CHECK_EQ(panel[32].gate, 0.0);                // note_off drops the gate
  CHECK_EQ(panel[40].pitch, 2.0);               // second note, an octave up
  CHECK_EQ(panel[48].clock, 1.0);               // clock edge is a 1-sample pulse
  CHECK_EQ(panel[49].clock, 0.0);
  CHECK_EQ(panel[56].clock, 1.0);
  CHECK_EQ(panel[60].pressure, 0.5);            // cc (a parameter) does not touch pressure
  CHECK_EQ(panel[64].gate, 0.0);                // final note_off

  // The real negative: a path that skips the shared state machine and interprets
  // the message itself re-scales an already-normalized pitch — a second
  // interpretation point. Its internal result must DIFFER from the conforming one.
  const auto rogue_pitch = rogue_frames(phrase(), Rogue::pitch_double, total);
  CHECK_FALSE(frames_equal(panel, rogue_pitch));
  CHECK_EQ(rogue_pitch[10].pitch, 2.0);         // the deviation is observable
}

// ------------------------------------------------------------------- test 2 -----

// velocity/aftertouch fold to the original's SINGLE pressure dimension.
static void velocity_folds_to_pressure_only() {
  const core::InputStateMachine sm(kBindings, kBindingCount);

  // note_on emits gate_on + pitch + pressure; it must NEVER emit a parameter
  // (the keyboard has no separate velocity-capability to target).
  core::PerformanceInput note{};
  note.kind = core::PerfInputKind::note_on;
  note.sample = 10;
  note.pitch = core::SignalSample{1.0};
  note.value = core::SignalSample{0.5};
  note.source = kMidi;
  core::ControlEvent evs[3];
  std::uint32_t n = sm.translate(note, evs, 3);
  CHECK_EQ(n, 3u);
  bool sawGate = false, sawPitch = false, sawPressure = false, sawParam = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    switch (evs[i].kind) {
      case core::ControlEventKind::gate_on:  sawGate = true; CHECK_EQ(evs[i].value, 1.0); break;
      case core::ControlEventKind::pitch:    sawPitch = true; CHECK_EQ(evs[i].value, 1.0); break;
      case core::ControlEventKind::pressure: sawPressure = true; CHECK_EQ(evs[i].value, 0.5); break;
      case core::ControlEventKind::parameter: sawParam = true; break;
      default: break;
    }
  }
  CHECK_TRUE(sawGate);
  CHECK_TRUE(sawPitch);
  CHECK_TRUE(sawPressure);
  CHECK_FALSE(sawParam);

  // aftertouch only ever maps to pressure, never to a parameter.
  core::PerformanceInput at{};
  at.kind = core::PerfInputKind::aftertouch;
  at.sample = 24;
  at.value = core::SignalSample{0.75};
  at.source = kMidi;
  core::ControlEvent ev[2];
  n = sm.translate(at, ev, 2);
  CHECK_EQ(n, 1u);
  CHECK_EQ(ev[0].kind, core::ControlEventKind::pressure);
  CHECK_EQ(ev[0].value, 0.75);

  // The bypass negative: a rogue path folds velocity into a CUTOFF target. Its
  // pressure lane stays 0 where the conforming path folds it to 0.5 — so it is
  // caught. This is a real error (a "helpful" adapter inventing a target).
  const std::uint64_t total = phrase_total();
  const core::InputStateMachine smc(kBindings, kBindingCount);
  const auto conform = compliant_frames(adopt(phrase(), kMidi, 2), smc, total);
  const auto rogue_v = rogue_frames(phrase(), Rogue::velocity_to_cutoff, total);
  CHECK_FALSE(frames_equal(conform, rogue_v));
  CHECK_EQ(rogue_v[10].pressure, 0.0);  // the deviation: pressure was not folded
  CHECK_EQ(conform[10].pressure, 0.5);
}

// ------------------------------------------------------------------- test 3 -----

// MIDI clock maps to a clock edge only — never to a pitch, gate or parameter.
static void clock_maps_to_clock_edge_only() {
  const core::InputStateMachine sm(kBindings, kBindingCount);
  core::PerfInputKind ck = core::PerfInputKind::clock;
  core::PerformanceInput c{};
  c.kind = ck;
  c.sample = 48;
  c.value = core::SignalSample{1.0};
  c.source = kMidi;
  core::ControlEvent ev[2];
  std::uint32_t n = sm.translate(c, ev, 2);
  CHECK_EQ(n, 1u);
  CHECK_EQ(ev[0].kind, core::ControlEventKind::clock);
  CHECK_EQ(ev[0].value, 1.0);

  // The bypass negative: a rogue mis-routes the clock edge as a PITCH cell. The
  // frame shows a pitch jump where the conforming path only advances the clock.
  const std::uint64_t total = phrase_total();
  const core::InputStateMachine smc(kBindings, kBindingCount);
  const auto conform = compliant_frames(adopt(phrase(), kMidi, 2), smc, total);
  const auto rogue_c = rogue_frames(phrase(), Rogue::clock_to_pitch, total);
  CHECK_FALSE(frames_equal(conform, rogue_c));
  CHECK_EQ(rogue_c[48].clock, 0.0);  // the clock edge was consumed
  CHECK_EQ(conform[48].clock, 1.0);
  CHECK_EQ(rogue_c[48].pitch, 1.0);  // ...and turned into a spurious pitch
}

// ------------------------------------------------------------------- test 4 -----

// CC-learn is a DATA-driven controller -> ParameterId map: one code path, any
// binding. An unbound controller is dropped, never fabricated into a target.
static void cc_learn_is_data_driven_map() {
  const core::InputStateMachine sm(kBindings, kBindingCount);

  core::PerformanceInput cc1{};
  cc1.kind = core::PerfInputKind::cc;
  cc1.sample = 60;
  cc1.value = core::SignalSample{0.25};
  cc1.controller = 1;  // -> cutoff
  cc1.source = kMidi;
  core::ControlEvent ev[1];
  std::uint32_t n = sm.translate(cc1, ev, 1);
  CHECK_EQ(n, 1u);
  CHECK_EQ(ev[0].kind, core::ControlEventKind::parameter);
  CHECK_EQ(ev[0].parameter, kCutoffParam);  // data: controller 1 -> cutoff

  // Same code path, different data: controller 74 -> resonance, not cutoff.
  core::PerformanceInput cc74{};
  cc74.kind = core::PerfInputKind::cc;
  cc74.sample = 60;
  cc74.value = core::SignalSample{0.75};
  cc74.controller = 74;
  cc74.source = kMidi;
  n = sm.translate(cc74, ev, 1);
  CHECK_EQ(n, 1u);
  CHECK_EQ(ev[0].parameter, kResParam);
  CHECK(ev[0].parameter != kCutoffParam);  // the second binding is not a copy

  // No phantom capability: a controller with no binding is DROPPED (returns 0),
  // never mapped to a target the machine does not have.
  core::PerformanceInput unbound{};
  unbound.kind = core::PerfInputKind::cc;
  unbound.sample = 60;
  unbound.value = core::SignalSample{0.9};
  unbound.controller = 7;  // no binding
  unbound.source = kMidi;
  n = sm.translate(unbound, ev, 1);
  CHECK_EQ(n, 0u);

  // A hardcoded-branch mapper would ignore the map and fab a fixed target for ANY
  // controller — the representative bug. Show the real data-driven behaviour is
  // NOT it: the fabricated path fires (returns 1) on the unbound controller, and
  // targets something other than the "real" answer. So the data-driven drop is
  // load-bearing, not a tautology.
  core::ControlEvent fev[1];
  n = hardcoded_branch_cc(unbound, fev, 1);
  CHECK_EQ(n, 1u);
  CHECK_TRUE(fev[0].kind == core::ControlEventKind::parameter);
  CHECK(fev[0].parameter != kResParam);  // its fabricated target is arbitrary
}

// ------------------------------------------------------------------- test 5 -----

// Reuse the existing EventTimebase: the input path inherits its buffer-invariant
// absolute-sample timing across a mixed, non-uniform partition.
static void timebase_reused_buffer_invariant() {
  const core::InputStateMachine sm(kBindings, kBindingCount);
  const std::uint64_t total = phrase_total();

  // The same TimedControlEvent stream, delivered under two different block
  // partitions. EventTimebase is the scheduler; absolute timing must not change.
  std::vector<core::TimedControlEvent> stream;
  for (const auto& pi : adopt(phrase(), kMidi, 2)) {
    core::ControlEvent evs[3];
    std::uint32_t n = sm.translate(pi, evs, 3);
    for (std::uint32_t i = 0; i < n; ++i) {
      core::TimedControlEvent te{};
      te.event = evs[i];
      te.sample = pi.sample;
      stream.push_back(te);
    }
  }

  const std::vector<std::uint32_t> uniform = {64};
  const std::vector<std::uint32_t> mixed = {64, 100, 37, 128, 7, 256, 91};  // legacy P2-①

  const auto ref = render_absolute(stream, total);
  const auto via_uniform = render_through_timebase(stream, total, uniform);
  const auto via_mixed = render_through_timebase(stream, total, mixed);
  CHECK_TRUE(frames_equal(ref, via_uniform));
  CHECK_TRUE(frames_equal(ref, via_mixed));  // buffer invariance

  // The negative: a block-LAZY timebase that re-derives timing from the partition
  // applies a due event at the block's first sample instead of its true offset.
  // That drifts from the reference — proving EventTimebase is the load-bearing
  // absolute scheduler, not a reinvention.
  const auto lazy = render_block_lazy(stream, total, mixed);
  CHECK_FALSE(frames_equal(ref, lazy));
}

int main() {
  three_path_equivalence_and_bypass();
  velocity_folds_to_pressure_only();
  clock_maps_to_clock_edge_only();
  cc_learn_is_data_driven_map();
  timebase_reused_buffer_invariant();
  return ::test::finish("input_equivalence");
}
