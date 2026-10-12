// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/panel_diagnostics.h>
#include <host/rt_log.h>
#include <lunar24/core/input_state_machine.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lunar24;
using namespace lunar24::host::diag;
using Clock = std::chrono::steady_clock;

static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

int main() {
  // ---- the audio-thread ring: order, time, overflow ----
  {
    auto rt = std::make_unique<RtLog>();
    std::vector<RtEvent> got;
    std::thread producer([&] {
      for (int i = 0; i < 1000; ++i) while (!rt->push(RtKind::midi_note_on, 0, i % 128, 100, 1)) std::this_thread::yield();
    });
    while (got.size() < 1000) rt->drain([&](const RtEvent& e) { got.push_back(e); });
    producer.join();
    bool ordered = true;
    for (std::size_t i = 0; i < got.size(); ++i) ordered = ordered && std::size_t(got[i].b) == i % 128 && (i == 0 || got[i].when >= got[i - 1].when);
    CHECK(ordered);
    std::uint32_t pushed = 0;
    for (std::uint32_t i = 0; i < RtLog::kCapacity; ++i) pushed += rt->push(RtKind::midi_cc, 0, 1, 2) ? 1u : 0u;
    CHECK_EQ(pushed, RtLog::kCapacity);
    CHECK(!rt->push(RtKind::midi_cc, 0, 1, 2));
    CHECK_EQ(rt->takeDropped(), 1u);
    CHECK_EQ(rt->takeDropped(), 0u);
  }

  // ---- event text: what the owner reads ----
  {
    RtEvent e;
    e.kind = RtKind::midi_note_on; e.a = 0; e.b = 60; e.c = 100; e.d = 1;
    CHECK_EQ(rtText(e), std::string("note-on ch=1 note=60(C4) vel=100 side=R"));
    e.kind = RtKind::midi_note_off; e.d = 1;
    CHECK(has(rtText(e), "held-by-sustain"));
    e.kind = RtKind::midi_sustain; e.c = 0; e.d = 3;
    CHECK_EQ(rtText(e), std::string("sustain up ch=1 released=3"));
    CHECK(rtLevel(RtKind::midi_cc) == Level::trace);
    CHECK(rtLevel(RtKind::event_rejected) == Level::warn);
    CHECK(rtCat(RtKind::event_rejected) == Cat::engine);
  }

  // ---- which MIDI notes are down, as the drained events tell it ----
  {
    MidiHeldMirror m;
    auto ev = [](RtKind k, unsigned a, unsigned b, unsigned c = 0, int d = 0) {
      RtEvent e; e.kind = k; e.a = std::uint8_t(a); e.b = std::uint8_t(b); e.c = std::uint8_t(c); e.d = std::int16_t(d);
      return e;
    };
    CHECK_EQ(m.describe(), std::string("midi notes down: none; held by sustain: none"));
    m.apply(ev(RtKind::midi_note_on, 0, 60, 100));
    m.apply(ev(RtKind::midi_note_on, 1, 64, 100));
    m.apply(ev(RtKind::midi_note_off, 1, 64, 0, 1));  // released under the pedal
    CHECK_EQ(m.describe(), std::string("midi notes down: ch1:C4; held by sustain: ch2:E4"));
    m.apply(ev(RtKind::midi_sustain, 1, 64, 0, 1));    // pedal up on channel 2
    m.apply(ev(RtKind::midi_note_off, 0, 60));
    CHECK_EQ(m.describe(), std::string("midi notes down: none; held by sustain: none"));
    // A second input device (lanes 16-31): named in the text, reset on its own.
    m.apply(ev(RtKind::midi_note_on, 0, 60, 100));
    m.apply(ev(RtKind::midi_note_on, 16, 60, 100));
    CHECK_EQ(m.describe(), std::string("midi notes down: ch1:C4 in2 ch1:C4; held by sustain: none"));
    m.apply(ev(RtKind::midi_input_reset, 1, 0, 0, 1));  // device 1 (the second) was closed
    CHECK_EQ(m.describe(), std::string("midi notes down: ch1:C4; held by sustain: none"));
    CHECK_EQ(rtText(ev(RtKind::midi_note_on, 16 + 9, 36, 90)), std::string("note-on ch=10 in=2 note=36(C2) vel=90 side=L"));
    CHECK_EQ(rtText(ev(RtKind::midi_note_on, 9, 36, 90)), std::string("note-on ch=10 note=36(C2) vel=90 side=L"));
    CHECK_EQ(rtText(ev(RtKind::midi_input_reset, 1, 0, 0, 2)), std::string("input 2 reset: released 2 held notes"));
    CHECK_EQ(rtText(ev(RtKind::midi_input_reset, 0, 0, 0, 2)), std::string("input reset: released 2 held notes"));
    m.apply(ev(RtKind::midi_note_off, 0, 60));
    m.apply(ev(RtKind::midi_note_on, 0, 62, 90));
    m.apply(ev(RtKind::swap_reset, 0, 0));
    m.lost();
    CHECK(has(m.describe(), "down: none") && has(m.describe(), "may be incomplete"));
  }

  // ---- sound / silence, and the effector tail ----
  {
    LevelWatch w;
    std::vector<std::string> lines;
    auto emit = [&](Level, const std::string& t) { lines.push_back(t); };
    auto t = Clock::now();
    const auto ms20 = std::chrono::milliseconds(20);
    // update(now, wet, dry, effector in, audio input, running)
    w.update(t, 0.f, 0.f, 0.f, 0.f, true, emit);
    CHECK(lines.empty());
    w.update(t += ms20, 0.5f, 0.f, 0.3f, 0.f, true, emit);
    CHECK(w.sounding() && lines.size() == 1 && has(lines[0], "output sounding: peak -6.0 dBFS"));
    for (int i = 0; i < 20; ++i) w.update(t += ms20, 0.00005f, 0.f, 0.00005f, 0.f, true, emit);
    CHECK(w.sounding() && lines.size() == 1);  // a 400 ms dip is not silence
    // A gating LFO: the effector input drops out for a second at a time. Nothing is logged.
    for (int cycle = 0; cycle < 5; ++cycle) {
      for (int i = 0; i < 50; ++i) w.update(t += ms20, 0.3f, 0.f, 0.f, 0.f, true, emit);
      for (int i = 0; i < 50; ++i) w.update(t += ms20, 0.3f, 0.f, 0.2f, 0.f, true, emit);
    }
    std::size_t news = 0;
    for (const auto& l : lines) news += has(l, "levels") ? 0 : 1;
    CHECK_EQ(news, std::size_t(1));  // only the summaries every 10 s
    CHECK(has(w.describe(t), "effector input sounding"));
    // The sources stop; the effector rings on for 3 s, then dies away.
    for (int i = 0; i < 150; ++i) w.update(t += ms20, 0.2f, 0.f, 0.f, 0.f, true, emit);
    CHECK(has(w.describe(t), "effector input silent for 3.0 s (what sounds on WET is only the effector tail)"));
    for (int i = 0; i < 30; ++i) w.update(t += ms20, 0.f, 0.f, 0.f, 0.f, true, emit);
    CHECK(!w.sounding() && has(lines.back(), "output silent; the last 3.0 s of WET were only the effector tail"));
    // DRY bypasses the effector: DRY alone fading out is no effector tail.
    for (int i = 0; i < 100; ++i) w.update(t += ms20, 0.f, 0.2f, 0.f, 0.f, true, emit);
    CHECK(w.sounding() && has(w.describe(t), "silent for") && !has(w.describe(t), "tail"));
    w.update(t += std::chrono::seconds(11), 0.f, 0.2f, 0.f, 0.f, true, emit);
    CHECK(has(lines.back(), "levels (10 s max dBFS): wet -inf, dry -14.0, effector in -inf, audio input -inf"));
    for (int i = 0; i < 30; ++i) w.update(t += ms20, 0.f, 0.f, 0.f, 0.f, true, emit);
    CHECK(has(lines.back(), "output silent (below -80 dBFS for 0.5 s)"));
    // A silent output writes no summaries, however loud the (unrouted) microphone is.
    const std::size_t quiet = lines.size();
    for (int i = 0; i < 1600; ++i) w.update(t += ms20, 0.f, 0.f, 0.f, 0.5f, true, emit);
    CHECK_EQ(lines.size(), quiet);
    w.update(t, 0.3f, 0.f, 0.f, 0.f, true, emit);
    w.update(t, 0.f, 0.f, 0.f, 0.f, false, emit);
    CHECK(has(lines.back(), "no audio stream") && !w.sounding());
  }

  // ---- the machine as text ----
  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  host::StandaloneAudioEngine& e = *engine;
  CHECK(e.prepare(1, 48000.0, 256, 0, 2));
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};
  auto run = [&](int blocks) {
    for (int b = 0; b < blocks; ++b) e.processBlock(nullptr, outs, 0, 2, 256);
  };
  core::InputStateMachine input{nullptr, 0};
  std::uint64_t seq = 0;
  auto note = [&](core::PerfInputKind kind) {
    core::PerformanceInput p{};
    p.kind = kind;
    p.value = static_cast<core::SignalSample>(0.8);
    p.noteId = 5;
    p.source = 1;
    p.seq = ++seq;
    core::ControlEvent ev[3];
    const std::uint32_t n = input.translate(p, ev, 3);
    for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    run(2);
  };
  CHECK(e.postParameter(core::ParameterId::envelope_a_hold, 1));
  CHECK(e.postDroneKey(0, true));
  CHECK(e.postConnect(core::JackId::keyboard_gate_left_main_out, core::JackId::envelope_b_gate_in));
  note(core::PerfInputKind::note_on);
  CHECK_EQ(e.keyboardHeldNotes(0), 1u);  // the running keyboard reports the held note
  e.takeEffectorInputPeak();
  run(8);
  CHECK(e.takeEffectorInputPeak() > 0.001f);  // the open drone reaches the effector
  CHECK_EQ(e.takeEffectorInputPeak(), 0.f);   // taken: nothing new until the next block
  {
    const std::string s = machineSnapshot(e, "mark 1");
    CHECK(has(s, "snapshot begin reason=mark 1"));
    CHECK(has(s, "drone_keys=100000"));
    CHECK(has(s, "dsp keyboard: left holds 1 note(s), mode keyboard"));
    CHECK(has(s, "cable keyboard.gate_left_main_out -> envelope_b.gate_in"));
    CHECK(has(s, "cables=1"));
    CHECK(has(s, "param envelope_a.hold=1\n"));
    CHECK(s.size() < Logger::kMaxRecordBytes);
    // The hex is the complete device state.
    const auto start = s.find("device_state_hex=") + std::strlen("device_state_hex=");
    std::vector<std::uint8_t> wire;
    for (auto i = start; i + 1 < s.size() && s[i] != '\n'; i += 2)
      wire.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    core::DeviceStateV1 decoded;
    CHECK(core::decode_device_state(wire.data(), wire.size(), &decoded));
    CHECK(decoded.inputCable[static_cast<unsigned>(core::JackId::envelope_b_gate_in)] == 1);
  }

  // ---- panel changes, coalesced ----
  {
    PanelWatch w;
    std::vector<std::string> recs;
    auto emit = [&](const std::string& t) { recs.push_back(t); };
    auto t = Clock::now();
    w.poll(e, t, emit);
    CHECK(recs.size() == 1 && has(recs[0], "reason=first state"));
    CHECK(e.postParameter(core::ParameterId::vco_a_tune, 0.1));
    CHECK(e.postParameter(core::ParameterId::vco_a_tune, 0.2));
    CHECK(e.postDisconnect(core::JackId::envelope_b_gate_in));
    CHECK(e.postDroneKey(0, false));
    note(core::PerfInputKind::note_off);
    w.poll(e, t + std::chrono::milliseconds(100), emit);
    CHECK_EQ(recs.size(), std::size_t(1));  // not yet: at most twice a second
    w.poll(e, t + std::chrono::milliseconds(500), emit);
    CHECK_EQ(recs.size(), std::size_t(2));
    const std::string& c = recs.back();
    CHECK(has(c, "param vco_a.tune=0.2") && !has(c, "vco_a.tune=0.1"));
    CHECK(has(c, "cable removed keyboard.gate_left_main_out -> envelope_b.gate_in"));
    CHECK(has(c, "drone key 1 closed"));
    CHECK(has(c, "dsp keyboard holds left=0 right=0"));
    w.poll(e, t + std::chrono::seconds(1), emit);
    CHECK_EQ(recs.size(), std::size_t(2));  // nothing changed, nothing written
  }

  // ---- engine loss counters: only growth is news ----
  {
    EngineWatch w;
    std::vector<std::string> recs;
    auto emit = [&](Level, const std::string& t) { recs.push_back(t); };
    w.poll(e, emit);
    w.poll(e, emit);
    CHECK(recs.empty());
  }
  return test::finish("test_diag_panel");
}
