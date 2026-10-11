// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/chime.h — the app's "ready" signature, played once when the app has
// started and once after RESET PANEL. A short synth phrase, 2-3 s long, so it is
// recognisably the instrument and not a phone's ding: a rising A-minor arpeggio of detuned
// saws that lands as a chord while its low-pass filter opens, with a bell on the top note. It is added to the WET outputs after the whole machine (no effector, no long
// tail) and is not part of the instrument: the plugins never play it. Tuned by ear.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace lunar24::host {

class Chime {
 public:

  // Any thread: play the phrase from the start of the next rendered block.
  void trigger() { pending_.store(true, std::memory_order_release); }

  // Audio thread: add the phrase into the two WET outputs (either may be null: rendered into
  // nothing, e.g. under MUTE). No allocation, no locks.
  void render(double* left, double* right, int frames, double sampleRate) {
    if (pending_.exchange(false, std::memory_order_acq_rel)) start_(sampleRate);
    if (!active_) return;
    for (int i = 0; i < frames; ++i) {
      double l = 0.0, r = 0.0;
      bool any = false;
      for (std::size_t v = 0; v < count_; ++v) {
        Voice& x = voices_[v];
        if (x.done) continue;
        any = true;
        if (now_ < x.start) continue;
        const double s = x.tick(now_ - x.start, sr_);
        l += s * (1.0 - x.pan);
        r += s * (1.0 + x.pan);
      }
      if (left != nullptr) left[i] += l;
      if (right != nullptr) right[i] += r;
      ++now_;
      if (!any) { active_ = false; break; }
    }
  }
  bool active() const { return active_; }

 private:
  static constexpr double kPi = 3.14159265358979323846;
  enum class Wave { saws, bell };

  // One note: two detuned saws (+ a sine an octave down) through a resonant low-pass whose
  // cutoff glides f0 -> f1, or an FM bell. Envelope: attack, decay to sustain while the gate is
  // held, then an exponential release.
  struct Voice {
    // set by start_
    long start = 0;                 // sample the note starts at
    double gate = 1, hz = 440, level = 0.1, pan = 0;
    Wave wave = Wave::saws;
    double att = 0.01, dec = 0.3, sus = 0.6, rel = 0.5;
    double f0 = 800, f1 = 4000, sweep = 1.0, q = 0.3;
    // running state
    double ph1 = 0, ph2 = 0, ph3 = 0, mph = 0, ic1 = 0, ic2 = 0, envAtGate = -1;
    bool done = true;

    static double polyBlep(double t, double dt) {
      if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
      if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
      return 0.0;
    }
    double env(double t) {
      if (t < gate) {
        const double e = t < att ? t / att : sus + (1.0 - sus) * std::exp(-(t - att) / dec);
        return e;
      }
      if (envAtGate < 0) envAtGate = gate < att ? gate / att : sus + (1.0 - sus) * std::exp(-(gate - att) / dec);
      return envAtGate * std::exp(-(t - gate) / rel);
    }
    double tick(long n, double sr) {
      const double t = n / sr;
      const double e = env(t);
      if (t > gate && e < 1e-3) { done = true; return 0.0; }
      double s = 0.0;
      if (wave == Wave::saws) {
        const double d1 = hz * 1.0040 / sr, d2 = hz * 0.9960 / sr, d3 = hz * 0.5 / sr;  // +-7 cents
        double a = 2.0 * ph1 - 1.0 - polyBlep(ph1, d1);
        double b = 2.0 * ph2 - 1.0 - polyBlep(ph2, d2);
        const double sub = std::sin(2.0 * kPi * ph3);
        ph1 += d1; if (ph1 >= 1.0) ph1 -= 1.0;
        ph2 += d2; if (ph2 >= 1.0) ph2 -= 1.0;
        ph3 += d3; if (ph3 >= 1.0) ph3 -= 1.0;
        const double x = 0.5 * (a + b) + 0.35 * sub;
        // TPT state-variable low-pass, cutoff gliding exponentially f0 -> f1.
        const double k = std::min(t / sweep, 1.0);
        const double fc = std::min(f0 * std::pow(f1 / f0, k), 0.45 * sr);
        const double g = std::tan(kPi * fc / sr), res = 2.0 - 2.0 * q;
        const double v1 = (ic1 + g * (x - ic2)) / (1.0 + g * (g + res)) ;
        const double v2 = ic2 + g * v1;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        s = v2;
      } else {
        // FM bell: modulator at 3.5x, its index falling with the note.
        const double index = 2.5 * std::exp(-t / 0.35);
        s = std::sin(2.0 * kPi * ph1 + index * std::sin(2.0 * kPi * mph));
        ph1 += hz / sr; if (ph1 >= 1.0) ph1 -= 1.0;
        mph += 3.5 * hz / sr; if (mph >= 1.0) mph -= 1.0;
      }
      return level * e * s;
    }
  };

  void add_(double at, double gate, double hz, double level, double pan, Wave w, double att, double dec,
            double sus, double rel, double f0 = 800, double f1 = 4000, double sweep = 1.0, double q = 0.3) {
    if (count_ >= voices_.size()) return;
    Voice& v = voices_[count_++];
    v = Voice{};
    v.start = static_cast<long>(at * sr_);
    v.gate = gate; v.hz = hz; v.level = level; v.pan = pan; v.wave = w;
    v.att = att; v.dec = dec; v.sus = sus; v.rel = rel;
    v.f0 = f0; v.f1 = f1; v.sweep = sweep; v.q = q;
    v.done = false;
  }

  void start_(double sr) {
    if (!(sr > 0.0)) return;
    sr_ = sr;
    now_ = 0;
    count_ = 0;
    // In A, the drones' default key. Tuned by ear.
    // A3 E4 A4 C5 E5, 0.15 s apart, each held until about 1.7 s, the filter opening as they enter.
    add_(0.00, 1.70, 220.0, 0.060, -0.30, Wave::saws, 0.010, 0.40, 0.55, 0.35, 500, 3500, 1.4, 0.35);
    add_(0.15, 1.55, 329.6, 0.055, -0.10, Wave::saws, 0.010, 0.40, 0.55, 0.35, 600, 4000, 1.3, 0.35);
    add_(0.30, 1.40, 440.0, 0.050, 0.10, Wave::saws, 0.010, 0.40, 0.55, 0.35, 700, 4500, 1.2, 0.35);
    add_(0.45, 1.25, 523.3, 0.045, 0.25, Wave::saws, 0.010, 0.40, 0.55, 0.35, 800, 5000, 1.1, 0.35);
    add_(0.60, 1.10, 659.3, 0.045, 0.35, Wave::saws, 0.010, 0.40, 0.55, 0.35, 900, 6000, 1.0, 0.35);
    add_(0.60, 0.20, 1318.5, 0.030, 0.0, Wave::bell, 0.002, 0.30, 0.0, 0.30);  // E6 bell on top
    active_ = count_ > 0;
  }

  std::atomic<bool> pending_{false};
  // audio thread
  bool active_ = false;
  double sr_ = 48000.0;
  long now_ = 0;
  std::size_t count_ = 0;
  std::array<Voice, 8> voices_{};
};

}  // namespace lunar24::host
