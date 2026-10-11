// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/chime.h — the app's short "ready" chime, played once when the app has
// started and once after RESET PANEL. It is added to the WET outputs after the whole machine
// (no effector, no tail) and is not part of the instrument: the plugins never play it.
#pragma once

#include <array>
#include <atomic>
#include <cmath>

namespace lunar24::host {

class Chime {
 public:
  // The candidate voicings the owner chooses between by ear (tuned by ear).
  enum class Style { glass, rise, wood };
  explicit Chime(Style style = Style::rise) : style_(style) {}

  // Any thread: play the chime from the start of the next rendered block.
  void trigger() { pending_.store(true, std::memory_order_release); }

  // Audio thread: add the chime into the two WET outputs. No allocation, no locks.
  void render(double* left, double* right, int frames, double sampleRate) {
    if (pending_.exchange(false, std::memory_order_acq_rel)) start_(sampleRate);
    if (!active_) return;
    for (int i = 0; i < frames; ++i) {
      double l = 0.0, r = 0.0;
      bool any = false;
      for (Partial& p : partials_) {
        if (p.amp == 0.0) continue;
        if (p.delay > 0) { --p.delay; any = true; continue; }
        // A 3 ms raised-cosine onset (no click), then an exponential decay.
        const double onset = p.age < attack_ ? 0.5 - 0.5 * std::cos(kPi * p.age / attack_) : 1.0;
        const double s = p.amp * p.env * onset * std::sin(p.phase);
        p.phase += p.step;
        if (p.phase > 2.0 * kPi) p.phase -= 2.0 * kPi;
        p.env *= p.decay;
        ++p.age;
        if (p.env < 1e-3) p.amp = 0.0;  // 60 dB down (below -80 dBFS): this partial is done
        l += s * (1.0 - p.pan);
        r += s * (1.0 + p.pan);
        any = true;
      }
      if (left != nullptr) left[i] += l;
      if (right != nullptr) right[i] += r;
      if (!any) { active_ = false; break; }
    }
  }
  bool active() const { return active_; }

 private:
  static constexpr double kPi = 3.14159265358979323846;
  struct Partial {
    double amp = 0.0, env = 1.0, decay = 1.0, phase = 0.0, step = 0.0, pan = 0.0;
    int delay = 0;   // samples before this partial starts
    int age = 0;     // samples since it started
  };

  void start_(double sr) {
    if (!(sr > 0.0)) return;
    for (Partial& p : partials_) p = Partial{};
    attack_ = 0.003 * sr;
    // freq Hz, level, decay time constant s, start s, pan -1..1
    auto add = [&](int i, double hz, double level, double tau, double at, double pan) {
      Partial& p = partials_[static_cast<std::size_t>(i)];
      p.amp = level;
      p.step = 2.0 * kPi * hz / sr;
      p.decay = std::exp(-1.0 / (tau * sr));
      p.delay = static_cast<int>(at * sr);
      p.pan = pan;
    };
    switch (style_) {
      case Style::glass:  // one bell-like E6: a pure tone plus an inharmonic shimmer partial
        add(0, 1318.5, 0.090, 0.16, 0.0, 0.0);
        add(1, 1318.5 * 2.76, 0.025, 0.06, 0.0, 0.0);
        add(2, 1318.5 * 5.40, 0.010, 0.03, 0.0, 0.0);
        break;
      case Style::rise:  // two soft notes, A5 then E6 (a fifth, the drones' key of A)
        add(0, 880.0, 0.080, 0.14, 0.0, -0.25);
        add(1, 1760.0, 0.012, 0.06, 0.0, -0.25);
        add(2, 1318.5, 0.080, 0.18, 0.11, 0.25);
        add(3, 2637.0, 0.012, 0.07, 0.11, 0.25);
        break;
      case Style::wood:  // a short marimba-like pair, E5 then A5
        add(0, 659.3, 0.075, 0.10, 0.0, -0.2);
        add(1, 659.3 * 3.93, 0.022, 0.025, 0.0, -0.2);
        add(2, 880.0, 0.075, 0.12, 0.09, 0.2);
        add(3, 880.0 * 3.93, 0.022, 0.025, 0.09, 0.2);
        break;
    }
    active_ = true;
  }

  Style style_;
  std::atomic<bool> pending_{false};
  bool active_ = false;  // audio thread
  double attack_ = 1.0;
  std::array<Partial, 4> partials_{};
};

}  // namespace lunar24::host
