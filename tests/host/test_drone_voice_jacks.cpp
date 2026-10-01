// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Drone 3/6 panel jacks, heard through the engine: LFO OUT carries the LF square, CV IN
// bends the tone's pitch (1 V/oct), and the S&H section samples on an external CLOCK and
// drives its OUT. Pitch is read as zero crossings per window of the DRY A (VCO A) or WET
// output.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "mini_test.h"
#include <host/standalone_audio_engine.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>

using namespace lunar24;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

void set(core::DeviceStateV1& st, const char* id, double v) {
  const core::ParameterDescriptor* d = core::find_parameter_by_name(id);
  CHECK(d != nullptr);
  if (d) core::state_set_param(st, d->id, v);
}
void cable(core::DeviceStateV1& st, const char* from, const char* to) {
  const core::JackDescriptor* a = core::find_jack_by_name(from);
  const core::JackDescriptor* b = core::find_jack_by_name(to);
  CHECK(a != nullptr && b != nullptr && core::state_connect(st, a->id, b->id));
}

// Zero crossings in each `windowSec` window of one output channel (0/1 = WET L/R, 2 = DRY A).
std::vector<int> crossings(const core::DeviceStateV1& st, int channel, double seconds, double windowSec = 0.05) {
  host::StandaloneAudioEngine engine;
  CHECK(engine.prepare(1, kSr, kBlock, 2, 4));
  CHECK(engine.applyDeviceState(st, kSr, kBlock, 2, 4) == host::StandaloneAudioEngine::StateApplyStatus::Accepted);
  std::vector<double> in(kBlock, 0.0), o[4];
  for (auto& b : o) b.assign(kBlock, 0.0);
  const double* ins[2] = {in.data(), in.data()};
  double* outs[4] = {o[0].data(), o[1].data(), o[2].data(), o[3].data()};
  const int window = int(windowSec * kSr);
  std::vector<int> counts;
  int n = 0, c = 0;
  double prev = 0.0;
  for (int pos = 0; pos < int(seconds * kSr); pos += kBlock) {
    engine.processBlock(ins, outs, 2, 4, kBlock);
    for (int i = 0; i < kBlock; ++i) {
      const double x = o[channel][static_cast<std::size_t>(i)];
      if (prev <= 0.0 && x > 0.0) ++c;
      prev = x;
      if (++n == window) {
        counts.push_back(c);
        n = c = 0;
      }
    }
  }
  counts.erase(counts.begin(), counts.begin() + 2);  // settle
  return counts;
}
int minOf(const std::vector<int>& v) { int m = 1 << 30; for (int x : v) m = x < m ? x : m; return m; }
int maxOf(const std::vector<int>& v) { int m = 0; for (int x : v) m = x > m ? x : m; return m; }
int jumps(const std::vector<int>& v) {
  int j = 0;
  for (std::size_t i = 1; i < v.size(); ++i) j += (v[i] - v[i - 1] > 3 || v[i - 1] - v[i] > 3) ? 1 : 0;
  return j;
}
}  // namespace

int main() {
  // LFO OUT: patched into VCO A's V/OCT, the square flips the VCO between two pitches.
  {
    core::DeviceStateV1 st = core::make_default_device_state(1);
    set(st, "envelope_a.hold", 1);
    const std::vector<int> plain = crossings(st, 2, 2.0);
    set(st, "drone_3.rate", 0.6);  // about 2.4 Hz
    cable(st, "drone_3.cv_out", "vco_a.v_oct_in");
    const std::vector<int> lfo = crossings(st, 2, 2.0);
    CHECK(maxOf(plain) - minOf(plain) <= 2);
    CHECK(maxOf(lfo) > 2 * minOf(lfo) + 5);
  }
  // CV IN: a positive joystick voltage raises drone 3's pitch.
  {
    core::DeviceStateV1 st = core::make_default_device_state(1);
    for (int ch = 1; ch <= 10; ++ch) {
      char id[24];
      std::snprintf(id, sizeof id, "mixer.ch%d_vol", ch);
      set(st, id, ch == 3 ? 1.0 : 0.0);
    }
    set(st, "drone_3.hold", 1);
    set(st, "drone_3.noise", 0);
    set(st, "drone_3.pitch", 0.3);
    const std::vector<int> base = crossings(st, 0, 1.0);
    set(st, "joystick.x", 0.6);
    cable(st, "joystick.x_out", "drone_3.cv_in");
    const std::vector<int> up = crossings(st, 0, 1.0);
    CHECK(minOf(up) * 10 > maxOf(base) * 17);  // about an octave up at +1 V
  }
  // S&H: clocked from LFO A, its OUT moves VCO A's pitch at every clock; with no clock
  // at all (internal LF stopped, no cable) it holds.
  {
    core::DeviceStateV1 st = core::make_default_device_state(1);
    set(st, "envelope_a.hold", 1);
    set(st, "drone_3.rate", 0);
    cable(st, "drone_3.sh_out", "vco_a.v_oct_in");
    CHECK_EQ(jumps(crossings(st, 2, 6.0, 0.2)), 0);
  }
  {
    core::DeviceStateV1 st = core::make_default_device_state(1);
    set(st, "envelope_a.hold", 1);
    set(st, "drone_3.rate", 0);
    set(st, "lfo_a.rate", 0.8);
    set(st, "lfo_a.wave", 1);  // square: clean clock edges
    cable(st, "drone_3.sh_out", "vco_a.v_oct_in");
    cable(st, "lfo_a.cv_out", "drone_3.clock_in");
    CHECK(jumps(crossings(st, 2, 6.0, 0.2)) >= 2);
  }
  // The S&H samples the voice's white noise even with NOISE (what is heard) at zero.
  {
    core::DeviceStateV1 st = core::make_default_device_state(1);
    set(st, "envelope_a.hold", 1);
    set(st, "drone_3.rate", 0);
    set(st, "drone_3.noise", 0);
    set(st, "lfo_a.rate", 0.8);
    set(st, "lfo_a.wave", 1);
    cable(st, "drone_3.sh_out", "vco_a.v_oct_in");
    cable(st, "lfo_a.cv_out", "drone_3.clock_in");
    CHECK(jumps(crossings(st, 2, 6.0, 0.2)) >= 2);
  }
  // GATE IN: with the keyboard's GATE L patched in, a drone (classic 1 and Papa 3) is silent
  // until a key is held and fades out after it is released.
  for (int drone : {1, 3}) {
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    for (int v = 0; v < 6; ++v) e.postDroneKey(v, v == drone - 1);
    const std::string gateIn = "drone_" + std::to_string(drone) + ".gate_in";
    CHECK(e.postConnect(core::find_jack_by_name("keyboard.gate_left_main_out")->id,
                        core::find_jack_by_name(gateIn)->id));
    e.postParameter(core::ParameterId::mixer_ch5_vol, 0.0);  // VCO A / B out of the way
    e.postParameter(core::ParameterId::mixer_ch6_vol, 0.0);
    e.postParameter(core::ParameterId::effector_blend, 0.0);
    std::vector<double> l(256), r(256);
    double* outs[2] = {l.data(), r.data()};
    auto rms = [&](int blocks) {
      double sum = 0;
      for (int b = 0; b < blocks; ++b) {
        e.processBlock(nullptr, outs, 0, 2, 256);
        for (double x : l) sum += x * x;
      }
      return std::sqrt(sum / (blocks * 256.0));
    };
    core::InputStateMachine in{nullptr, 0};
    std::uint64_t seq = 0;
    auto send = [&](core::PerfInputKind k) {
      core::PerformanceInput p{};
      p.kind = k;
      p.value = 0.8;
      p.noteId = 7;
      p.source = 1;
      p.seq = ++seq;
      core::ControlEvent ev[3];
      const std::uint32_t n = in.translate(p, ev, 3);
      for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    };
    rms(1900);
    CHECK(rms(200) < 1e-4);  // gate low: silent
    send(core::PerfInputKind::note_on);
    rms(1900);
    CHECK(rms(200) > 1e-3);  // key held: the drone sounds
    send(core::PerfInputKind::note_off);
    rms(1900);
    CHECK(rms(200) < 1e-4);  // released: silent again
  }
  return test::finish("test_drone_voice_jacks");
}
