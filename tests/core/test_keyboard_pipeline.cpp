// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH#8 keyboard identity + fixed-phase end-to-end replay
// (design/00 §2d/§2e, design/06 §P4, design/07 §1 §3; @Codex 9b573d0a).
//
// This layer exists to uphold, through the REAL pipeline
//     PerformanceInput -> InputStateMachine -> EventTimebase (canonical sort)
//                         -> ArpSeq -> KeyboardBehaviour -> tick
//   * a note/touch press keeps ONE stable identity (source, channel, noteId) through the
//     whole pipeline; note_on's pitch/pressure/gate_on all carry it;
//   * note_on is ALL-OR-NONE: translate() writes the whole 3-event transaction or
//     nothing, and EventTimebase enqueueBatch admits the whole batch or leaves both
//     lanes unchanged (never a partial note rescued by parameter-coalesce/reconcile);
//   * KeyboardBehaviour keys note state BY IDENTITY and honours the frozen phase order
//     (pitch/pressure/parameter phase 1, gate-off phase 2, gate-on phase 4): the pitch
//     arrives in phase 1 and latches in phase 4 with that already-arrived pitch — it
//     never "waits for the next pitch after the gate-on" (the old nextPitchIsNewNote_);
//   * overlap: releasing the non-current note leaves pitch/gate/modulation alone; the
//     current note release deterministically returns to a still-held note; only the
//     LAST release closes pressure/vibrato;
//   * tick() advances the portamento EVERY sample (0 -> 2 V glide is monotone, never
//     frozen at current());
//   * reset clears all held identity / gate / pressure / vibrato / portamento and a
//     same-sample later note re-opens afresh;
//   * the arp chord + seq base are kept BY IDENTITY and precisely deleted, so releasing
//     one held plate does not pop a wrong note; the draw is all-or-none observable.
//
// Every assertion is routed through the real replay, not source-grep or an isolated
// helper mirror, so each one goes RED on an implementation that dropped the pitch,
// popped LIFO, froze the glide, admitted a partial note, or overrode the phase order.

#include "mini_test.h"

#include <array>
#include <cmath>
#include <cstdint>

#include <lunar24/core/arp_sequencer.h>
#include <lunar24/core/control_event.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/keyboard_behaviour.h>

namespace core = lunar24::core;

namespace {

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }
double to_double(core::SignalSample v) { return static_cast<double>(v); }

// ------------------------------------------------------------------ test harness --

// The real replay, wired end to end. schedule() routes a PerformanceInput through
// translate() then the EventTimebase's whole-transaction admission at the input's
// absolute sample; sendRaw() injects a raw critical event (a host reset edge) straight
// into the timebase, as the product forwards it. render() runs one processBlock and
// hands each due event through ArpSeq (which may transform) into KeyboardBehaviour,
// recording the ordered stream KeyboardBehaviour observes (for the buffer-invariant).
struct ReplayHarness {
  core::InputStateMachine ism;
  core::EventTimebase tb;
  core::ArpSeq arp;
  core::KeyboardBehaviour kb;
  std::array<core::TimedControlEvent, 256> evbuf{};
  std::array<core::ControlEvent, 256> log{};
  std::uint32_t logSize = 0;
  double fs = 48000.0;

  ReplayHarness(const core::KeyboardBehaviourParams& kbp, const core::ArpSeqParams& ap)
      : ism(nullptr, 0), arp(), kb() {
    kb.configure(kbp, fs);
    arp.configure(ap, fs);
  }

  // Translate + whole-transaction admit at the input's absolute sample.
  void schedule(const core::PerformanceInput& in) {
    std::array<core::ControlEvent, 8> ce{};
    std::uint32_t n = ism.translate(in, ce.data(), static_cast<std::uint32_t>(ce.size()));
    std::array<core::TimedControlEvent, 8> te{};
    for (std::uint32_t i = 0; i < n; ++i) {
      te[i].event = ce[i];
      te[i].sample = in.sample;
    }
    tb.enqueueBatch(te.data(), n);
  }

  // Inject a raw critical ControlEvent at an absolute sample (a host reset edge).
  void sendRaw(const core::ControlEvent& ev, std::uint64_t sample) {
    core::TimedControlEvent te{};
    te.event = ev;
    te.sample = sample;
    std::array<core::TimedControlEvent, 1> batch{te};
    tb.enqueueBatch(batch.data(), 1);
  }

  void render(std::uint32_t frames) {
    // A NAMED (lvalue) sink: ArpSeq forwards it by lvalue ref, so a temporary lambda
    // would not bind (handleArp/handleSeq take Sink&). Listener captures `this`.
    auto sink = [this](const core::ControlEvent& e) {
      if (logSize < static_cast<std::uint32_t>(log.size())) log[logSize++] = e;
      kb.handleControlEvent(e);
    };
    std::uint32_t n = tb.processBlock(frames, evbuf.data(),
                                      static_cast<std::uint32_t>(evbuf.size()));
    for (std::uint32_t i = 0; i < n; ++i) arp.handleControlEvent(evbuf[i].event, sink);
  }

  // Tick `n` samples and return the final pitch_cv (vibrato off, so == the glide).
  double glideSettle(std::uint32_t n) {
    double p = 0.0, c = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) kb.tick(&p, &c);
    return p;
  }
  // One sample's pressure_cv.
  double pressureNow() {
    double p = 0.0, c = 0.0;
    kb.tick(&p, &c);
    return c;
  }
  // Tick `n` samples and return the peak-to-peak of the pitch output. Nonzero while
  // the vibrato LFO is modulating (pitch = glide + sine); ~0 once a release has gated
  // the vibrato off AND the glide is settled (pitch = constant glide).
  double pitchRange(std::uint32_t n) {
    double p = 0.0, c = 0.0, mn = 1e30, mx = -1e30;
    for (std::uint32_t i = 0; i < n; ++i) {
      kb.tick(&p, &c);
      if (p < mn) mn = p;
      if (p > mx) mx = p;
    }
    return mx - mn;
  }
};

core::PerformanceInput noteOn(double pitch, core::NoteId id, std::uint64_t sample) {
  core::PerformanceInput in{};
  in.kind = core::PerfInputKind::note_on;
  in.sample = sample;
  in.pitch = static_cast<core::SignalSample>(pitch);
  in.value = static_cast<core::SignalSample>(1.0);  // velocity -> pressure fold
  in.channel = 0;
  in.source = 1;
  in.noteId = id;
  in.seq = sample;  // deterministic same-source tiebreak
  return in;
}
core::PerformanceInput noteOff(core::NoteId id, std::uint64_t sample) {
  core::PerformanceInput in{};
  in.kind = core::PerfInputKind::note_off;
  in.sample = sample;
  in.channel = 0;
  in.source = 1;
  in.noteId = id;
  in.seq = sample;
  return in;
}
core::PerformanceInput clockAt(std::uint64_t sample) {
  core::PerformanceInput in{};
  in.kind = core::PerfInputKind::clock;
  in.sample = sample;
  in.source = 9;
  return in;
}
core::ControlEvent resetEvent() {
  core::ControlEvent r{};
  r.kind = core::ControlEventKind::reset;
  r.value = core::SignalSample{0.0};
  r.source = 9;
  return r;
}
core::ControlEvent syncEvent() {
  core::ControlEvent s{};
  s.kind = core::ControlEventKind::sync;
  s.value = core::SignalSample{0.0};
  s.source = 9;
  return s;
}

// A KeyboardBehaviourParams with a real (nonzero) portamento so the glide/fallback
// assertions are time-based and robust; microtonal scale (pitch CV passes through);
// Pressure output with instant rise/fall (so pressure is exact and gate-sensitive).
core::KeyboardBehaviourParams kb_params(double portamentoSpeed = 0.04,
                                        std::uint8_t pressureOutput = 0,
                                        double rise = 0.0, double fall = 0.0,
                                        double vibratoSpeed = 0.0,
                                        double vibratoDepth = 0.0) {
  core::KeyboardBehaviourParams p{};
  p.scaleEditor = core::kMicrotonalScaleMask;  // no quantise: pitch CV passes through
  p.pressureOutput = pressureOutput;           // 0 Pressure, 1 ASR
  p.pressureRise = rise;                       // instant rise by default
  p.pressureFall = fall;                       // instant fall by default
  p.portamentoSpeed = portamentoSpeed;         // 0.04 -> tau = 0.1 s
  p.portamentoLegato = 0;                      // always glide
  p.vibratoSpeed = vibratoSpeed;
  p.vibratoDepth = vibratoDepth;
  return p;
}

core::ArpSeqParams arp_params(std::uint8_t mode) {
  core::ArpSeqParams p{};
  p.mode = mode;  // 0 keyboard, 1 arpeggiator, 2 sequencer
  p.arpDirection = 0;
  p.arpInterval = 0.0;  // -> 1 semitone (the minimum, PROVISIONAL)
  p.seqRun = (mode == 2) ? 1 : 0;  // sequencer keyboard mode
  p.seqDirection = 0;
  p.seqCvOutput = 1;       // gated
  p.seqLength = 0.0;       // -> 2 steps
  for (std::uint32_t i = 0; i < 16; ++i) {
    p.steps[i].note = static_cast<std::uint8_t>(i % 16);
    p.steps[i].gate = 1;
  }
  return p;
}

}  // namespace

// ---------------------------------------------------- note_on identity + first latch --

static void note_on_carries_identity_and_first_note_latches() {
  ReplayHarness h(kb_params(), arp_params(0));  // keyboard mode

  // A single note_on: the SAME (source=1, channel=0, noteId=7) on all three events.
  h.schedule(noteOn(1.0, 7, 0));
  h.render(64);
  CHECK_TRUE(h.kb.gate());

  // The three events KeyboardBehaviour observed all carry the same identity.
  CHECK_EQ(h.logSize, 3u);
  CHECK_TRUE(h.log[0].kind == core::ControlEventKind::pitch);
  CHECK_TRUE(h.log[1].kind == core::ControlEventKind::pressure);
  CHECK_TRUE(h.log[2].kind == core::ControlEventKind::gate_on);
  CHECK_EQ(h.log[0].noteId, 7u);
  CHECK_EQ(h.log[1].noteId, 7u);
  CHECK_EQ(h.log[2].noteId, 7u);
  CHECK_EQ(h.log[0].channel, 0u);
  CHECK_EQ(h.log[0].source, 1u);

  // The pitch target arrived in phase 1 and latched in phase 4 with that same pitch:
  // the glide reaches 1.0 V. (An impl with the old "next pitch after gate-on" protocol
  // saw the pitch already consumed and latched at 0.)
  CHECK_TRUE(near(h.glideSettle(96000), 1.0, 5e-4));
}

// ------------------------------------------------------------------ overlap rules --

static void overlap_release_non_current_holds() {
  ReplayHarness h(kb_params(), arp_params(0));
  h.schedule(noteOn(0.5, 1, 0));   // A
  h.schedule(noteOn(1.5, 2, 0));   // B
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  CHECK_TRUE(near(h.glideSettle(96000), 1.5, 5e-4));  // B is current (newest)
  CHECK_TRUE(h.pressureNow() > 0.9);                  // sounding note drives pressure

  // Release A (the NON-current note). Pitch/gate/pressure are left alone.
  h.schedule(noteOff(1, 64));
  h.render(64);
  CHECK_TRUE(h.kb.gate());                                   // B still held
  CHECK_TRUE(near(h.glideSettle(96000), 1.5, 5e-4));         // still gliding to B
  CHECK_TRUE(h.pressureNow() > 0.9);                        // modulation held open

  // Release B (the LAST note) -> closes gate and pressure.
  h.schedule(noteOff(2, 128));
  h.render(64);
  CHECK_TRUE(!h.kb.gate());
  CHECK_TRUE(h.pressureNow() < 1e-3);
}

static void overlap_release_current_falls_back() {
  ReplayHarness h(kb_params(), arp_params(0));
  h.schedule(noteOn(0.5, 1, 0));   // A
  h.schedule(noteOn(1.5, 2, 0));   // B
  h.render(64);
  CHECK_TRUE(near(h.glideSettle(96000), 1.5, 5e-4));  // B is current

  // Release B (the SOUNDING note) -> deterministically return to the still-held A.
  h.schedule(noteOff(2, 64));
  h.render(64);
  CHECK_TRUE(h.kb.gate());                                   // A still held
  CHECK_TRUE(near(h.glideSettle(96000), 0.5, 5e-4));         // glided back to A's 0.5

  // Release A (last) -> closes.
  h.schedule(noteOff(1, 128));
  h.render(64);
  CHECK_TRUE(!h.kb.gate());
}

// --------------------------------------------------------------------- reset path --

static void reset_clears_and_renotes() {
  // ASR pressure output (mode 1) with a NON-ZERO fall: the reset must be a HARD clear,
  // not a gate(false) that would leave the envelope decaying (non-zero after one tick).
  ReplayHarness h(kb_params(0.04, 1, 0.0, 0.3), arp_params(0));
  h.schedule(noteOn(1.0, 1, 0));
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  CHECK_TRUE(near(h.glideSettle(96000), 1.0, 5e-4));
  CHECK_TRUE(h.pressureNow() > 0.99);  // ASR attack -> sustain ≈ live pressure (1.0)

  // A raw reset edge through the timebase full-clears gate/pressure/portamento. The
  // pressure has a non-zero fall, so gate(false)-only would still be ~1.0 after one tick;
  // the hard reset makes it REALLY 0 on the first tick.
  h.sendRaw(resetEvent(), 64);
  h.render(64);
  CHECK_TRUE(!h.kb.gate());
  CHECK_TRUE(h.glideSettle(96000) < 1e-3);  // portamento reset to 0
  CHECK_TRUE(h.pressureNow() < 1e-3);      // HARD pressure clear, first tick (not decay)

  // A later same-source note re-opens afresh at its own pitch.
  h.schedule(noteOn(0.25, 2, 128));
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  CHECK_TRUE(near(h.glideSettle(96000), 0.25, 5e-4));
}

// ------------------------------------------ portamento glide + buffer invariance --

static void portamento_monotone_and_block_invariant() {
  // Portamento must ADVANCE every sample (the old tick() that just returned current()
  // left the glide frozen at its start); monotone, moving, and bounded by the target.
  {
    ReplayHarness g(kb_params(), arp_params(0));
    g.schedule(noteOn(0.0, 1, 0));
    g.render(64);
    g.schedule(noteOn(2.0, 1, 64));  // re-pitch the same sounding note -> glide 0 -> 2
    g.render(64);
    double p = 0.0, c = 0.0, prev = -1.0, last = 0.0;
    bool monotone = true;
    for (int i = 0; i < 6000; ++i) {
      g.kb.tick(&p, &c);
      if (p < prev - 1e-9) monotone = false;
      prev = p;
      last = p;
    }
    CHECK_TRUE(monotone);          // never jumps backwards
    CHECK_TRUE(last > 1.0);        // genuinely advanced 0 -> 2 (frozen-at-current stays 0)
    CHECK_TRUE(last < 2.0 + 1e-6); // and not overshooting the target
  }

  // Buffer invariance: the ordered stream KeyboardBehaviour observes is identical no
  // matter how the block is partitioned (64/128/256 — the absolute-sample order is the
  // contract, the block is only a delivery detail).
  {
    ReplayHarness a(kb_params(), arp_params(0));
    a.schedule(noteOn(0.0, 1, 0));
    a.schedule(noteOn(1.0, 2, 64));
    a.schedule(noteOn(2.0, 3, 128));
    a.schedule(noteOff(2, 192));
    for (std::uint32_t f : {64u, 64u, 64u, 64u}) a.render(f);

    ReplayHarness b(kb_params(), arp_params(0));
    b.schedule(noteOn(0.0, 1, 0));
    b.schedule(noteOn(1.0, 2, 64));
    b.schedule(noteOn(2.0, 3, 128));
    b.schedule(noteOff(2, 192));
    for (std::uint32_t f : {128u, 128u}) b.render(f);

    CHECK_EQ(a.logSize, b.logSize);
    for (std::uint32_t i = 0; i < a.logSize; ++i) {
      CHECK_EQ(a.log[i].kind, b.log[i].kind);
      CHECK_TRUE(near(to_double(a.log[i].value), to_double(b.log[i].value)));
      CHECK_EQ(a.log[i].noteId, b.log[i].noteId);
      CHECK_EQ(a.log[i].source, b.log[i].source);
      CHECK_EQ(a.log[i].channel, b.log[i].channel);
    }
  }
}

// ---------------------------------------- translate all-or-none + batch atomics --

static void translate_and_batch_all_or_none() {
  // translate() is all-or-none: a note_on needs 3 slots, writes nothing unless all 3.
  core::InputStateMachine ism(nullptr, 0);
  std::array<core::ControlEvent, 8> outs{};
  core::PerformanceInput no = noteOn(1.0, 1, 0);
  CHECK_EQ(ism.translate(no, outs.data(), 0), 0u);
  CHECK_EQ(ism.translate(no, outs.data(), 1), 0u);
  CHECK_EQ(ism.translate(no, outs.data(), 2), 0u);
  CHECK_EQ(ism.translate(no, outs.data(), 3), 3u);
  CHECK_TRUE(outs[0].kind == core::ControlEventKind::pitch);
  CHECK_TRUE(outs[1].kind == core::ControlEventKind::pressure);
  CHECK_TRUE(outs[2].kind == core::ControlEventKind::gate_on);
  CHECK_EQ(outs[0].noteId, 1u);
  CHECK_EQ(outs[1].noteId, 1u);
  CHECK_EQ(outs[2].noteId, 1u);
  // A note_off needs exactly 1; capacity 0 -> nothing.
  CHECK_EQ(ism.translate(noteOff(1, 0), outs.data(), 0), 0u);
  CHECK_EQ(ism.translate(noteOff(1, 0), outs.data(), 1), 1u);

  // enqueueBatch(continuous full) rejects the whole note batch, both lanes unchanged.
  {
    core::EventTimebase tb;
    for (std::uint32_t i = 0; i < 64; ++i) {  // distinct ParameterId -> no coalesce
      core::TimedControlEvent e{};
      e.event.kind = core::ControlEventKind::parameter;
      e.event.parameter = static_cast<core::ParameterId>(i + 1);
      e.sample = i;
      tb.enqueue(e);
    }
    const std::uint32_t before = tb.pending();  // 64 continuous
    std::array<core::TimedControlEvent, 3> nb{};
    nb[0].event.kind = core::ControlEventKind::pitch;   nb[0].sample = 100;
    nb[1].event.kind = core::ControlEventKind::pressure; nb[1].sample = 100;
    nb[2].event.kind = core::ControlEventKind::gate_on;  nb[2].sample = 100;
    CHECK_TRUE(!tb.enqueueBatch(nb.data(), 3));  // no continuous room
    CHECK_EQ(tb.batchRejected(), 1u);
    CHECK_EQ(tb.pending(), before);  // neither lane changed
  }
  // enqueueBatch(critical full) rejects the note batch the same way.
  {
    core::EventTimebase tb;
    for (std::uint32_t i = 0; i < 64; ++i) {  // distinct noteId gate_ons fill critical
      core::TimedControlEvent e{};
      e.event.kind = core::ControlEventKind::gate_on;
      e.event.noteId = static_cast<core::NoteId>(i + 1);
      e.sample = i;
      tb.enqueue(e);
    }
    const std::uint32_t before = tb.pending();
    std::array<core::TimedControlEvent, 3> nb{};
    nb[0].event.kind = core::ControlEventKind::pitch;   nb[0].sample = 100;
    nb[1].event.kind = core::ControlEventKind::pressure; nb[1].sample = 100;
    nb[2].event.kind = core::ControlEventKind::gate_on;  nb[2].sample = 100;
    CHECK_TRUE(!tb.enqueueBatch(nb.data(), 3));  // no critical room
    CHECK_EQ(tb.pending(), before);
  }
  // A rejected whole-batch transaction must NOT disturb the #2 pressure diagnostics:
  // it was rejected at admission (queue never mutated), so continuousOverflow /
  // criticalOverflow / reconcile / pending are all UNCHANGED — only batchRejected+1.
  // (The old code bumped the failing lane's overflow without a reconcile, breaking the
  // #2 "overflow implies reconcile" invariant — GH#8 must not re-introduce that.)
  {
    core::EventTimebase tb;
    for (std::uint32_t i = 0; i < 64; ++i) {  // 64 distinct params fill continuous exactly
      core::TimedControlEvent e{};
      e.event.kind = core::ControlEventKind::parameter;
      e.event.parameter = static_cast<core::ParameterId>(i + 1);
      e.sample = i;
      tb.enqueue(e);
    }
    CHECK_EQ(tb.continuousOverflow(), 0u);  // 64 fit exactly (no per-event overflow)
    const std::uint32_t cont = tb.continuousOverflow(), crit = tb.criticalOverflow();
    const bool rec = tb.reconcilePending();
    const std::uint32_t pend = tb.pending();
    std::array<core::TimedControlEvent, 3> nb{};
    nb[0].event.kind = core::ControlEventKind::pitch;   nb[0].sample = 100;
    nb[1].event.kind = core::ControlEventKind::pressure; nb[1].sample = 100;
    nb[2].event.kind = core::ControlEventKind::gate_on;  nb[2].sample = 100;
    CHECK_TRUE(!tb.enqueueBatch(nb.data(), 3));  // continuous full -> reject
    CHECK_EQ(tb.batchRejected(), 1u);
    CHECK_EQ(tb.continuousOverflow(), cont);   // UNCHANGED (no ++continuousOverflow_)
    CHECK_EQ(tb.criticalOverflow(), crit);     // UNCHANGED
    CHECK_EQ(tb.reconcilePending(), rec);      // UNCHANGED (no spurious reconcile)
    CHECK_EQ(tb.pending(), pend);              // nothing was enqueued
  }
  {
    core::EventTimebase tb;
    for (std::uint32_t i = 0; i < 64; ++i) {  // 64 gate_ons fill critical exactly
      core::TimedControlEvent e{};
      e.event.kind = core::ControlEventKind::gate_on;
      e.event.noteId = static_cast<core::NoteId>(i + 1);
      e.sample = i;
      tb.enqueue(e);
    }
    CHECK_EQ(tb.criticalOverflow(), 0u);      // 64 fit exactly (no per-event overflow)
    CHECK_TRUE(!tb.reconcilePending());
    const std::uint32_t crit = tb.criticalOverflow();
    const std::uint32_t pend = tb.pending();
    std::array<core::TimedControlEvent, 3> nb{};
    nb[0].event.kind = core::ControlEventKind::pitch;   nb[0].sample = 100;
    nb[1].event.kind = core::ControlEventKind::pressure; nb[1].sample = 100;
    nb[2].event.kind = core::ControlEventKind::gate_on;  nb[2].sample = 100;
    CHECK_TRUE(!tb.enqueueBatch(nb.data(), 3));  // critical full -> reject
    CHECK_EQ(tb.batchRejected(), 1u);
    CHECK_EQ(tb.criticalOverflow(), crit);     // UNCHANGED
    CHECK_TRUE(!tb.reconcilePending());        // UNCHANGED (false, no spurious reconcile)
    CHECK_EQ(tb.pending(), pend);
  }
  // An empty batch is no transaction: neither admitted nor rejected must be counted.
  {
    core::EventTimebase tb;
    core::TimedControlEvent empty{};
    CHECK_TRUE(!tb.enqueueBatch(&empty, 0));
    CHECK_EQ(tb.batchAdmitted(), 0u);
    CHECK_EQ(tb.batchRejected(), 0u);
  }
  // An empty timebase admits the whole transaction and dispatches it in the canonical
  // dependency order (pitch, pressure, gate_on) — never a reordered partial note.
  {
    core::EventTimebase tb;
    std::array<core::TimedControlEvent, 3> nb{};
    nb[0].event.kind = core::ControlEventKind::pitch;   nb[0].sample = 0;
    nb[1].event.kind = core::ControlEventKind::pressure; nb[1].sample = 0;
    nb[2].event.kind = core::ControlEventKind::gate_on;  nb[2].sample = 0;
    CHECK_TRUE(tb.enqueueBatch(nb.data(), 3));
    CHECK_EQ(tb.batchAdmitted(), 1u);
    std::array<core::TimedControlEvent, 8> out{};
    const std::uint32_t n = tb.processBlock(64, out.data(),
                                            static_cast<std::uint32_t>(out.size()));
    CHECK_EQ(n, 3u);
    CHECK_TRUE(out[0].event.kind == core::ControlEventKind::pitch);
    CHECK_TRUE(out[1].event.kind == core::ControlEventKind::pressure);
    CHECK_TRUE(out[2].event.kind == core::ControlEventKind::gate_on);
  }
}

// ---------------------------------------------------------- arp/seq identity rules --

static void arp_release_uses_held_identity() {
  ReplayHarness h(kb_params(), arp_params(1));  // arpeggiator
  h.schedule(noteOn(0.5, 1, 0));  // A
  h.schedule(noteOn(1.5, 2, 0));  // B
  h.render(64);
  CHECK_TRUE(!h.kb.gate());  // arp intercepts the plates; nothing sounds directly

  // Release A: the chord becomes [B] precisely (not a LIFO pop that drops B).
  h.schedule(noteOff(1, 64));
  h.render(64);

  // Clock -> the arp emits B + interval (1 semitone) as the sound.
  h.schedule(clockAt(128));
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  // B survived the release, so the arpeggiated pitch is B+interval, NOT A+interval.
  CHECK_TRUE(near(h.glideSettle(96000), 1.5 + 1.0 / 12.0, 5e-4));
}

static void seq_base_release_uses_remaining() {
  ReplayHarness h(kb_params(), arp_params(2));  // sequencer, keyboard run
  h.schedule(noteOn(0.0, 1, 0));             // base C
  h.schedule(noteOn(7.0 / 12.0, 2, 0));      // base G
  h.render(64);
  CHECK_TRUE(!h.kb.gate());

  // Release C (the initial base): the transposition base becomes the REMAINING held G.
  h.schedule(noteOff(1, 64));
  h.render(64);

  // Clock -> seq step 0 (note 0) transposed by the remaining base G.
  h.schedule(clockAt(128));
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  CHECK_TRUE(near(h.glideSettle(96000), 7.0 / 12.0, 5e-4));  // G + 0, not C + 0
}

// --------------------------------------------------------- over-capacity rejects --

static void over_capacity_rejections_observable() {
  ReplayHarness h(kb_params(), arp_params(0));
  for (core::NoteId id = 1; id <= 13; ++id) h.schedule(noteOn(0.1 * id, id, 0));
  h.render(64);
  // 12 plates fit; the 13th is rejected and observable, never a silent overwrite.
  CHECK_TRUE(h.kb.overCapacity() >= 1u);
  CHECK_TRUE(h.kb.gate());  // the notes that fit still sound
}

// ------------------------------------------------- arp note-switch canonical order --

static void arp_note_switch_is_canonical_pitch_then_gateoff_then_gateon() {
  ReplayHarness h(kb_params(), arp_params(1));  // arpeggiator
  h.schedule(noteOn(0.5, 1, 0));  // A held
  h.schedule(noteOn(1.5, 2, 0));  // B held
  h.render(64);
  CHECK_TRUE(!h.kb.gate());  // arp intercepts the plates; nothing sounds directly

  // First clock -> the initial constructed note (no previous gate to release).
  h.schedule(clockAt(64));
  h.render(64);
  CHECK_TRUE(h.kb.gate());

  // Second clock (previousGate=true): the note switch MUST be emitted as
  // pitch -> gate_off(previous) -> gate_on(new) — the canonical dependency (design/07
  // §3), NOT the old gate_off(previous)->pitch->gate_on. ArpSeq sits AFTER the timebase,
  // so this order is emitted directly and is never re-sorted by the comparator.
  h.schedule(clockAt(128));
  h.render(64);
  CHECK_EQ(h.logSize, 5u);  // 2 (clock 1) + 3 (clock 2) constructed events
  CHECK_TRUE(h.log[2].kind == core::ControlEventKind::pitch && h.log[2].noteId == 2u);
  CHECK_TRUE(h.log[3].kind == core::ControlEventKind::gate_off && h.log[3].noteId == 1u);
  CHECK_TRUE(h.log[4].kind == core::ControlEventKind::gate_on && h.log[4].noteId == 2u);
  // No reordering bug could leave the gate_off BEFORE the new pitch.
  CHECK_TRUE(h.log[3].kind != core::ControlEventKind::pitch);
  CHECK_TRUE(h.log[2].kind != core::ControlEventKind::gate_off);
}

// --------------------------------------------------------- sync is NOT a reset --

static void arp_sync_releases_run_not_reset() {
  ReplayHarness h(kb_params(), arp_params(1));  // arpeggiator
  h.schedule(noteOn(0.5, 1, 0));
  h.render(64);
  h.schedule(clockAt(64));  // arp sounds synthetic id 1
  h.render(64);
  CHECK_TRUE(h.kb.gate());

  // A sync restarts the arp/seq run: it must release the currently-sounding synthetic
  // note (a gate-off), NOT send a canonical reset (which would hard-clear the downstream
  // KeyboardBehaviour's pressure/vibrato/portamento — a reset's job, not a sync's).
  h.sendRaw(syncEvent(), 128);
  h.render(64);
  CHECK_TRUE(!h.kb.gate());
  CHECK_TRUE(h.log[h.logSize - 1].kind == core::ControlEventKind::gate_off);
  CHECK_EQ(h.log[h.logSize - 1].noteId, 1u);  // the running synthetic note was released
  for (std::uint32_t i = 0; i < h.logSize; ++i)
    CHECK_TRUE(h.log[i].kind != core::ControlEventKind::reset);  // no reset forwarded
}

// ------------------------------------------- vibrato overlap: last release closes it --

// A NON-ZERO vibrato: the pitch output must keep oscillating while any note is held,
// through a non-last release, and stop only on the LAST release. (A gate(false)-on-every-
// release mutation would stop the vibrato after the first release while another note is
// still held — that is the GH#8 partial-release bug this detector exists to catch.)
static void vibrato_overlap_last_release_stops() {
  ReplayHarness h(kb_params(0.04, 0, 0.0, 0.0, 0.5, 0.5), arp_params(0));
  h.schedule(noteOn(1.0, 1, 0));  // A
  h.schedule(noteOn(1.0, 2, 0));  // B (same pitch -> no glide motion once settled)
  h.render(64);
  CHECK_TRUE(h.kb.gate());

  // Settle the glide to 1.0 (both notes at the same pitch, so portamento is done), then
  // confirm the vibrato is modulating the pitch. The window MUST span a whole LFO cycle
  // (7.5 Hz -> 6400 samples) — a 200-sample window covers only ~11° of the cycle and
  // measures a phase-dependent sliver, not the true peak-to-peak.
  h.glideSettle(96000);
  CHECK_TRUE(h.pitchRange(12800) > 0.05);  // vibrato running (2 notes held)

  // Release the NON-current note A (B is still held): vibrato MUST continue.
  h.schedule(noteOff(1, 64));
  h.render(64);
  CHECK_TRUE(h.kb.gate());
  CHECK_TRUE(h.pitchRange(12800) > 0.05);  // still vibrating: B is not the last release

  // Release B (the LAST note): vibrato STOPS and the pitch returns to the constant glide.
  h.schedule(noteOff(2, 128));
  h.render(64);
  CHECK_TRUE(!h.kb.gate());
  CHECK_TRUE(h.pitchRange(12800) < 1e-3);  // gated off: no modulation on a dead voice
}

int main() {
  note_on_carries_identity_and_first_note_latches();
  overlap_release_non_current_holds();
  overlap_release_current_falls_back();
  reset_clears_and_renotes();
  portamento_monotone_and_block_invariant();
  translate_and_batch_all_or_none();
  arp_release_uses_held_identity();
  seq_base_release_uses_remaining();
  over_capacity_rejections_observable();
  arp_note_switch_is_canonical_pitch_then_gateoff_then_gateon();
  arp_sync_releases_run_not_reset();
  vibrato_overlap_last_release_stops();
  return ::test::finish("test_keyboard_pipeline");
}
