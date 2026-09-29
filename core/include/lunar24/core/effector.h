// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// effector.h — the DUAL EFFECTOR: two independent slots (left channel, right channel),
// each running one of the 39 cartridge programs (13 cartridges x 3 programs). The panel
// has ONE set of X / Y / Z knobs (plus CV X/Y/Z), a BLEND (dry/wet) and a MASTER volume
// shared by both slots; each slot has its own 1-2-3 program switch.
//
// The manual names each program and what X/Y/Z do, but gives no algorithm or numbers, so
// every curve and range below is a musical default, tuned by ear. Programs are built from a
// small set of DSP blocks:
//   delay line, FDN reverb, pitch shifter (two-tap granular), modulated delay
//   (chorus/flanger/vibrato), phaser, state-variable filter, ring modulator,
//   sample-rate/bit crusher, reverse delay, and two tiny synth voices.
//
// Real-time: all buffers are allocated in the constructor (off the audio thread);
// process() never allocates, locks or does I/O. Program changes crossfade (~15 ms).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lunar24::core {

namespace fx {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

inline double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Exponential map of a 0..1 knob onto [lo, hi].
inline double expMap(double n, double lo, double hi) { return lo * std::pow(hi / lo, clamp(n, 0.0, 1.0)); }
inline double softClip(double x) { return std::tanh(x); }
// Gentle limiter for feedback paths and outputs (unity below ~1, smooth above).
inline double softLimit(double x, double ceiling) { return ceiling * std::tanh(x / ceiling); }
inline double semitonesToRatio(double s) { return std::pow(2.0, s / 12.0); }

// Deterministic white noise (xorshift), -1..1.
struct Noise {
  std::uint32_t s = 0x9E3779B9u;
  explicit Noise(std::uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
  double next() {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<double>(s) * (2.0 / 4294967295.0) - 1.0;
  }
};

// One-pole smoother: y += a * (target - y).
struct Smooth {
  double y = 0.0, a = 1.0;
  void setTime(double seconds, double sr) { a = 1.0 - std::exp(-1.0 / (std::max(seconds, 1e-4) * sr)); }
  double tick(double target) { y += a * (target - y); return y; }
};

struct OnePoleLP {
  double z = 0.0, a = 1.0;
  void setCutoff(double hz, double sr) { a = 1.0 - std::exp(-kTwoPi * clamp(hz, 1.0, 0.49 * sr) / sr); }
  double tick(double x) { z += a * (x - z); return z; }
  void reset() { z = 0.0; }
};

struct DcBlock {
  double x1 = 0.0, y1 = 0.0;
  double tick(double x) { const double y = x - x1 + 0.995 * y1; x1 = x; y1 = y; return y; }
  void reset() { x1 = y1 = 0.0; }
};

// Topology-preserving state-variable filter (Simper). Gives LP/BP/HP/notch at once.
struct Svf {
  double ic1 = 0.0, ic2 = 0.0, g = 0.0, k = 1.4, a1 = 0.0, a2 = 0.0, a3 = 0.0;
  double lp = 0.0, bp = 0.0, hp = 0.0;
  void set(double hz, double q, double sr) {
    g = std::tan(kPi * clamp(hz, 10.0, 0.45 * sr) / sr);
    k = 1.0 / std::max(q, 0.05);
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  void tick(double x) {
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    lp = v2; bp = v1; hp = x - k * v1 - v2;
  }
  double notch() const { return lp + hp; }
  void reset() { ic1 = ic2 = lp = bp = hp = 0.0; }
};

struct Lfo {
  double phase = 0.0;
  double sine(double hz, double sr) {
    phase += hz / sr;
    phase -= std::floor(phase);
    return std::sin(kTwoPi * phase);
  }
  double tri(double hz, double sr) {
    phase += hz / sr;
    phase -= std::floor(phase);
    return 4.0 * std::fabs(phase - 0.5) - 1.0;
  }
  void reset(double p = 0.0) { phase = p; }
};

struct EnvFollower {
  double env = 0.0, att = 0.0, rel = 0.0;
  void set(double attackS, double releaseS, double sr) {
    att = 1.0 - std::exp(-1.0 / (attackS * sr));
    rel = 1.0 - std::exp(-1.0 / (releaseS * sr));
  }
  double tick(double x) {
    const double a = std::fabs(x);
    env += (a > env ? att : rel) * (a - env);
    return env;
  }
  void reset() { env = 0.0; }
};

// Power-of-two ring buffer with fractional (Hermite) reads.
class DelayLine {
 public:
  void init(std::size_t minLen) {
    std::size_t n = 16;
    while (n < minLen + 4) n <<= 1;
    buf_.assign(n, 0.0f);
    mask_ = n - 1;
    w_ = 0;
  }
  void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }
  std::size_t size() const { return buf_.size(); }
  void write(double x) {
    buf_[w_] = static_cast<float>(x);
    w_ = (w_ + 1) & mask_;
  }
  // Delay in samples measured from the most recently written sample (0 = newest).
  double read(double d) const {
    d = clamp(d, 0.0, static_cast<double>(mask_ - 3));
    const double fi = std::floor(d);
    const double f = d - fi;
    const std::size_t i = static_cast<std::size_t>(fi);
    const double y0 = at_(i), y1 = at_(i + 1), ym1 = at_(i == 0 ? 0 : i - 1), y2 = at_(i + 2);
    // 4-point Hermite, interpolating from y0 (delay i) toward y1 (delay i+1).
    const double c1 = 0.5 * (y1 - ym1);
    const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
    const double c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
    return ((c3 * f + c2) * f + c1) * f + y0;
  }
  double readInt(std::size_t d) const { return at_(d); }

 private:
  double at_(std::size_t d) const { return buf_[(w_ - 1 - d) & mask_]; }
  std::vector<float> buf_;
  std::size_t mask_ = 0, w_ = 0;
};

// Two-tap granular pitch shifter over a short delay (sin^2 crossfaded taps).
class PitchShifter {
 public:
  void init(double sr) {
    sr_ = sr;
    window_ = 0.060 * sr;  // 60 ms grains: smooth for pads and drones
    line_.init(static_cast<std::size_t>(window_ * 2.0 + 64));
  }
  void reset() { line_.clear(); phase_ = 0.0; }
  void setRatio(double r) { ratio_ = clamp(r, 0.25, 4.0); }
  double process(double x) {
    line_.write(x);
    phase_ += (1.0 - ratio_) / window_;
    phase_ -= std::floor(phase_);
    const double p2 = phase_ + 0.5 - std::floor(phase_ + 0.5);
    const double g1 = std::sin(kPi * phase_);
    const double g2 = std::sin(kPi * p2);
    return g1 * g1 * line_.read(phase_ * window_ + 1.0) + g2 * g2 * line_.read(p2 * window_ + 1.0);
  }

 private:
  DelayLine line_;
  double sr_ = 48000.0, window_ = 2880.0, ratio_ = 1.0, phase_ = 0.0;
};

// 8-line feedback delay network reverb, mono in / mono out, with input diffusion, per-line
// damping and slow modulation. `variant` gives the left/right slots different line lengths.
class Reverb {
 public:
  void init(double sr, int variant) {
    sr_ = sr;
    static constexpr double kLineMs[8] = {37.1, 41.9, 47.3, 53.1, 59.9, 67.3, 73.7, 81.1};
    static constexpr double kApMs[4] = {4.77, 3.59, 12.73, 9.31};
    const double spread = variant == 0 ? 1.0 : 1.083;
    for (int i = 0; i < 8; ++i) {
      baseLen_[i] = kLineMs[i] * spread * 1.6 * 0.001 * sr;  // up to ~140 ms per line
      lines_[i].init(static_cast<std::size_t>(baseLen_[i] + 64));
      lfo_[i].reset(0.125 * i);
    }
    for (int i = 0; i < 4; ++i) {
      apLen_[i] = kApMs[i] * (variant == 0 ? 1.0 : 1.11) * 0.001 * sr;
      ap_[i].init(static_cast<std::size_t>(apLen_[i] + 8));
    }
    set(3.0, 6000.0);
  }
  void reset() {
    for (auto& l : lines_) l.clear();
    for (auto& a : ap_) a.clear();
    for (auto& d : damp_) d.reset();
  }
  void set(double t60Seconds, double dampHz) {
    t60_ = clamp(t60Seconds, 0.1, 120.0);
    for (int i = 0; i < 8; ++i) {
      gain_[i] = std::pow(10.0, -3.0 * baseLen_[i] / (t60_ * sr_));
      damp_[i].setCutoff(dampHz, sr_);
    }
  }
  double process(double x) {
    // Input diffusion (series allpasses).
    double d = x;
    for (int i = 0; i < 4; ++i) {
      const double delayed = ap_[i].read(apLen_[i]);
      const double v = d + 0.62 * delayed;
      ap_[i].write(v);
      d = delayed - 0.62 * v;
    }
    double y[8];
    for (int i = 0; i < 8; ++i) {
      const double mod = (i & 1) ? 6.0 * lfo_[i].sine(0.07 + 0.013 * i, sr_) : 0.0;
      y[i] = lines_[i].read(baseLen_[i] + mod);
    }
    double s[8];
    for (int i = 0; i < 8; ++i) s[i] = damp_[i].tick(y[i]) * gain_[i];
    hadamard8_(s);
    double out = 0.0;
    for (int i = 0; i < 8; ++i) {
      const double in = (i & 1 ? -0.35 : 0.35) * d;
      lines_[i].write(softLimit(s[i] + in, 8.0));
      out += (i & 2 ? -y[i] : y[i]);
    }
    return out * 0.5;
  }

 private:
  static void hadamard8_(double* v) {
    for (int h = 1; h < 8; h <<= 1)
      for (int i = 0; i < 8; i += h << 1)
        for (int j = i; j < i + h; ++j) {
          const double a = v[j], b = v[j + h];
          v[j] = a + b;
          v[j + h] = a - b;
        }
    const double n = 1.0 / std::sqrt(8.0);
    for (int i = 0; i < 8; ++i) v[i] *= n;
  }
  double sr_ = 48000.0, t60_ = 3.0;
  DelayLine lines_[8];
  DelayLine ap_[4];
  double baseLen_[8] = {}, apLen_[4] = {}, gain_[8] = {};
  OnePoleLP damp_[8];
  Lfo lfo_[8];
};

}  // namespace fx

// The 13 cartridges, in registry program-id order (program id = cartridge * 3 + slot - 1).
enum class Cartridge : std::uint8_t {
  Cathedral = 0, Magic, Time, Vibrotrem, Filter, Vibe, PitchShifter, Infinity,
  StringRinger, Syntex1, Digital, Generator, Orche, Count
};

// One effector slot: a mono in / mono out processor running one of the 39 programs.
class EffectorSlot {
 public:
  void init(double sr, int variant) {
    sr_ = sr;
    variant_ = variant;
    delay_.init(static_cast<std::size_t>(4.6 * sr));
    mod_.init(static_cast<std::size_t>(0.06 * sr));
    pre_.init(static_cast<std::size_t>(0.6 * sr));
    rev_.init(sr, variant);
    ps1_.init(sr);
    ps2_.init(sr);
    noise_ = fx::Noise(variant == 0 ? 0x1234567u : 0x7654321u);
    timeSmooth_.setTime(0.25, sr);    // delay-time glides like a tape machine
    outPole_ = 1.0 - fx::kTwoPi * 15.0 / sr;
    reset();
  }

  void reset() {
    delay_.clear(); mod_.clear(); pre_.clear();
    rev_.reset(); ps1_.reset(); ps2_.reset();
    svf1_.reset(); svf2_.reset();
    lp1_.reset(); dc_.reset(); dc2_.reset(); env_.reset();
    for (auto& a : apState_) a = 0.0;
    lfo_.reset(variant_ * 0.25);
    phaserFb_ = 0.0; shimmer_ = 0.0;
    osc_ = osc2_ = subPhase_ = 0.0;
    trackedHz_ = 110.0; periodCount_ = 0; zcState_ = false; flip_ = 1.0;
    holdCount_ = 0.0; held_ = 0.0;
    shTarget_ = shValue_ = 0.5; shCount_ = 0.0;
    revPos_ = 0.0; osState_ = 0; osPos_ = 0.0; osGain_ = 0.0; osLen_ = 1.0; osRep_ = 0.0;
    timeSmooth_.y = -1.0;
  }

  // Select a program 0..38. The change crossfades: fade out, reset, switch, fade in.
  void setProgram(int program) {
    program = program < 0 ? 0 : (program > 38 ? 38 : program);
    if (program != program_) pending_ = program;
  }
  int program() const { return pending_ >= 0 ? pending_ : program_; }

  double process(double in, double x, double y, double z) {
    // Crossfaded program switch.
    const double step = 1.0 / (0.015 * sr_);
    if (pending_ >= 0) {
      fade_ -= step;
      if (fade_ <= 0.0) {
        fade_ = 0.0;
        program_ = pending_;
        pending_ = -1;
        reset();
      }
    } else if (fade_ < 1.0) {
      fade_ = std::min(1.0, fade_ + step);
    }
    double wet = run_(in, x, y, z);
    if (!std::isfinite(wet)) { reset(); wet = 0.0; }
    // Remove DC / subsonic energy (< ~15 Hz) that feedback programs can build up.
    outHp_ = wet - outX1_ + outPole_ * outHp_;
    outX1_ = wet;
    return outHp_ * fade_;
  }

 private:
  // ---- shared building blocks used by several programs --------------------------------------
  double delaySamples_(double seconds) {
    const double target = seconds * sr_;
    if (timeSmooth_.y < 0.0) timeSmooth_.y = target;  // jump on first use after a reset
    return timeSmooth_.tick(target);
  }
  // A little room/hall after a program ("Z = Reverb" on many cartridges).
  double withReverb_(double sig, double amount, double t60 = 2.8) {
    if (amount <= 0.001) { (void)rev_.process(0.0); return sig; }
    rev_.set(t60, 5500.0);
    return sig + amount * 0.6 * rev_.process(sig);
  }
  double pitchSemis_(double n) { return std::round(-12.0 + 24.0 * fx::clamp(n, 0.0, 1.0)); }
  // Simple zero-crossing pitch tracker for the synth cartridges.
  void trackPitch_(double in) {
    lp1_.setCutoff(900.0, sr_);
    const double s = lp1_.tick(in);
    ++periodCount_;
    const bool up = zcState_ ? s > -0.01 : s > 0.01;
    if (up && !zcState_) {
      if (periodCount_ > 8) {
        const double hz = sr_ / periodCount_;
        if (hz > 30.0 && hz < 1200.0) trackedHz_ += 0.2 * (hz - trackedHz_);
      }
      periodCount_ = 0;
      flip_ = -flip_;  // flip-flop: an octave-down square
    }
    zcState_ = up;
  }
  double saw_(double& ph, double hz) {
    const double dt = hz / sr_;
    ph += dt;
    ph -= std::floor(ph);
    double v = 2.0 * ph - 1.0;
    // polyBLEP at the wrap
    if (ph < dt) { const double t = ph / dt; v -= t + t - t * t - 1.0; }
    else if (ph > 1.0 - dt) { const double t = (ph - 1.0) / dt; v -= t * t + t + t + 1.0; }
    return v;
  }
  double sine_(double& ph, double hz) {
    ph += hz / sr_;
    ph -= std::floor(ph);
    return std::sin(fx::kTwoPi * ph);
  }
  // Reverse playback over chunks of `len` samples, two overlapping heads.
  double reverse_(double len) {
    revPos_ += 1.0;
    if (revPos_ >= len) revPos_ -= len;
    const double p1 = revPos_ / len;
    const double p2 = p1 + 0.5 - std::floor(p1 + 0.5);
    const double w1 = std::sin(fx::kPi * p1), w2 = std::sin(fx::kPi * p2);
    return w1 * w1 * delay_.read(2.0 * p1 * len + 1.0) + w2 * w2 * delay_.read(2.0 * p2 * len + 1.0);
  }
  double crush_(double in, double rateHz, double bits) {
    holdCount_ += rateHz / sr_;
    if (holdCount_ >= 1.0) {
      holdCount_ -= std::floor(holdCount_);
      const double q = std::pow(2.0, bits - 1.0);
      held_ = std::round(fx::clamp(in, -1.0, 1.0) * q) / q;
    }
    return held_;
  }
  double phaser_(double in, double depth, double rateHz, double fbAmt) {
    const double l = 0.5 + 0.5 * lfo_.sine(rateHz, sr_);
    const double hz = fx::expMap(l * (0.3 + 0.7 * depth), 150.0, 4000.0);
    const double t = std::tan(fx::kPi * hz / sr_);
    const double a = (t - 1.0) / (t + 1.0);
    double s = in + fbAmt * phaserFb_;
    for (double& st : apState_) {
      const double y = a * s + st;
      st = s - a * y;
      s = y;
    }
    phaserFb_ = s;
    return 0.5 * (in + s);
  }
  double modDelay_(double in, double baseMs, double depthMs, double rateHz, double fbAmt) {
    const double l = lfo_.sine(rateHz, sr_);
    const double d = (baseMs + depthMs * 0.5 * (1.0 + l)) * 0.001 * sr_;
    const double out = mod_.read(d);
    mod_.write(fx::softLimit(in + fbAmt * out, 2.0));
    return out;
  }

  // ---- the 39 programs ----------------------------------------------------------------------
  double run_(double in, double x, double y, double z) {
    const auto cart = static_cast<Cartridge>(program_ / 3);
    const int slot = program_ % 3;  // 0,1,2 = programs 1,2,3
    switch (cart) {
      case Cartridge::Cathedral:
        if (slot == 0) {  // Shimmer: X octave up, Y octave down, Z decay
          rev_.set(fx::expMap(z, 1.5, 40.0), 7000.0);
          const double r = rev_.process(in + shimmer_);
          ps1_.setRatio(2.0);
          ps2_.setRatio(0.5);
          // Feed the octaves back into the reverb. High-pass the loop so repeated
          // octave-down passes cannot pile energy up below ~150 Hz.
          const double fbSig = 0.5 * x * ps1_.process(r) + 0.35 * y * ps2_.process(r);
          svf2_.set(150.0, 0.7, sr_);
          svf2_.tick(fbSig);
          shimmer_ = fx::softLimit(svf2_.hp, 1.0);
          return 0.7 * r;
        } else if (slot == 1) {  // Oct up delay: X feedback, Y delay, Z reverb
          const double d = delaySamples_(fx::expMap(y, 0.05, 1.5));
          const double o = delay_.read(d);
          ps1_.setRatio(2.0);
          delay_.write(fx::softLimit(in + 0.92 * x * ps1_.process(o), 2.0));
          return withReverb_(o, z, 4.0);
        } else {  // Space reverb: X feedback, Y delay, Z reverb
          const double d = delaySamples_(fx::expMap(y, 0.03, 1.2));
          const double o = delay_.read(d);
          delay_.write(fx::softLimit(in + 0.9 * x * o, 2.0));
          rev_.set(7.0, 4500.0);
          return 0.5 * o + (0.2 + 0.7 * z) * rev_.process(in + o);
        }

      case Cartridge::Magic: {  // Pitch delays: X feedback, Y delay, Z pitch
        double semis = pitchSemis_(z);
        if (slot == 2) {  // Bell: harmonic intervals only
          static constexpr double kBell[7] = {-12, -5, 0, 7, 12, 19, 24};
          semis = kBell[static_cast<int>(fx::clamp(z, 0.0, 0.999) * 7.0)];
        }
        ps1_.setRatio(fx::semitonesToRatio(semis));
        const double d = delaySamples_(fx::expMap(y, 0.06, 1.5));
        double o;
        if (slot == 1) {  // Reverse pitch delay
          o = ps1_.process(reverse_(d));
          delay_.write(fx::softLimit(in + 0.85 * x * o, 2.0));
        } else {
          o = ps1_.process(delay_.read(d));
          delay_.write(fx::softLimit(in + 0.9 * x * o, 2.0));
        }
        if (slot == 2) {  // a bright ring on top
          svf1_.set(2400.0, 3.0, sr_);
          svf1_.tick(o);
          o = o + 0.6 * svf1_.bp;
          return withReverb_(o, 0.35, 3.5);
        }
        return o;
      }

      case Cartridge::Time: {
        if (slot == 0) {  // Delay reverb: X feedback, Y delay, Z reverb
          const double d = delaySamples_(fx::expMap(y, 0.04, 1.5));
          const double o = delay_.read(d);
          lp1_.setCutoff(6000.0, sr_);
          delay_.write(fx::softLimit(in + 0.95 * x * lp1_.tick(o), 2.0));
          return withReverb_(o, z, 3.0);
        }
        // Delay chorus / delay vibrato: modulated delay with feedback
        const double rate = slot == 1 ? 0.6 : fx::expMap(y, 0.5, 8.0);
        const double seconds = slot == 1 ? fx::expMap(y, 0.04, 1.2) : fx::expMap(y, 0.08, 0.6);
        const double base = delaySamples_(seconds);
        const double mod = z * (slot == 1 ? 0.006 : 0.004) * sr_ * 0.5 * (1.0 + lfo_.sine(rate, sr_));
        const double o = delay_.read(base + mod);
        delay_.write(fx::softLimit(in + 0.92 * x * o, 2.0));
        return o;
      }

      case Cartridge::Vibrotrem: {  // X depth, Y rate, Z reverb
        const double rate = fx::expMap(y, 0.1, 12.0);
        double o;
        if (slot == 0) {  // Tremolo
          o = in * (1.0 - x * 0.5 * (1.0 + lfo_.sine(rate, sr_)));
        } else if (slot == 1) {  // Vibrato
          o = modDelay_(in, 1.0, 1.0 + 6.0 * x, rate, 0.0);
        } else {  // Chorus
          o = 0.6 * in + 0.7 * modDelay_(in, 12.0, 1.0 + 12.0 * x, rate * 0.5, 0.15);
        }
        return withReverb_(o, z);
      }

      case Cartridge::Filter: {
        if (slot == 0) {  // Auto wah: X filter amount, Y envelope sensitivity, Z reverb
          env_.set(0.004, 0.12, sr_);
          const double e = env_.tick(in) * (1.0 + 12.0 * y);
          const double hz = 250.0 * std::pow(2.0, fx::clamp(e, 0.0, 1.0) * (1.0 + 5.0 * x));
          svf1_.set(hz, 4.0, sr_);
          svf1_.tick(in);
          return withReverb_(1.4 * svf1_.bp + 0.2 * svf1_.lp, z);
        } else if (slot == 1) {  // HP/LP: X HP cutoff, Y LP cutoff, Z resonance
          const double q = 0.6 + 9.0 * z * z;
          svf1_.set(fx::expMap(x, 20.0, 4000.0), q, sr_);
          svf1_.tick(in);
          svf2_.set(fx::expMap(y, 150.0, 20000.0), q, sr_);
          svf2_.tick(svf1_.hp);
          return svf2_.lp;
        } else {  // Notch: X cut 1, Y cut 2, Z resonance
          const double q = 0.4 + 6.0 * z;
          svf1_.set(fx::expMap(x, 60.0, 6000.0), q, sr_);
          svf1_.tick(in);
          svf2_.set(fx::expMap(y, 120.0, 12000.0), q, sr_);
          svf2_.tick(svf1_.notch());
          return svf2_.notch();
        }
      }

      case Cartridge::Vibe: {
        if (slot == 0) {  // Phaser: X depth, Y rate, Z reverb
          return withReverb_(phaser_(in, x, fx::expMap(y, 0.05, 6.0), 0.45), z);
        } else if (slot == 1) {  // Flanger: X depth, Y rate, Z reverb
          const double o = modDelay_(in, 0.6, 0.5 + 6.0 * x, fx::expMap(y, 0.03, 4.0), 0.55);
          return withReverb_(0.6 * (in + o), z);
        } else {  // Resonance flanger: X resonance, Y rate, Z mod depth
          const double o = modDelay_(in, 0.4, 0.3 + 7.0 * z, fx::expMap(y, 0.03, 4.0), 0.96 * x);
          return 0.5 * (in + o);
        }
      }

      case Cartridge::PitchShifter: {
        if (slot == 0) {  // SynthTaver (analog octaver): X oct down, Y oct up, Z direct
          trackPitch_(in);
          env_.set(0.003, 0.08, sr_);
          const double e = env_.tick(in);
          const double down = flip_ * e * 1.2;
          const double up = dc_.tick(std::fabs(in)) * 1.5;
          return x * down + y * up + z * in;
        }
        if (slot == 1) {  // Octaver: X oct down, Y oct up, Z direct
          ps1_.setRatio(0.5);
          ps2_.setRatio(2.0);
          return x * ps1_.process(in) + y * ps2_.process(in) + z * in;
        }
        // Harmonizer: X pitch 1, Y pitch 2, Z voice mix
        ps1_.setRatio(fx::semitonesToRatio(pitchSemis_(x)));
        ps2_.setRatio(fx::semitonesToRatio(pitchSemis_(y)));
        const double voices = 0.5 * (ps1_.process(in) + ps2_.process(in));
        return (1.0 - z) * in + z * 1.4 * voices;
      }

      case Cartridge::Infinity: {
        if (slot == 0) {  // Resonance reverb: X pre-delay, Y pre-delay mod, Z decay
          const double base = 0.005 + 0.45 * x;
          const double wob = 0.004 * y * lfo_.sine(0.3 + 2.0 * y, sr_);
          const double d = delaySamples_(base) + wob * sr_;
          const double p = pre_.read(d);
          pre_.write(fx::softLimit(in + 0.45 * p, 2.0));  // resonant pre-delay comb
          rev_.set(fx::expMap(z, 2.0, 90.0), 6000.0);
          return 0.75 * rev_.process(p);
        }
        // O.D.D (oscillating dirty delay) / Resonance delay: X feedback, Y delay, Z pitch
        ps1_.setRatio(fx::semitonesToRatio(pitchSemis_(z)));
        const double seconds = slot == 1 ? fx::expMap(y, 0.03, 1.5) : fx::expMap(y, 0.002, 0.08);
        const double o = delay_.read(delaySamples_(seconds));
        lp1_.setCutoff(slot == 1 ? 3500.0 : 9000.0, sr_);
        const double shifted = ps1_.process(lp1_.tick(o));
        const double fbGain = slot == 1 ? 1.25 * x : 0.985 * x;
        const double dirt = slot == 1 ? 0.002 * noise_.next() : 0.0;
        delay_.write(fx::softClip(in + fbGain * dc2_.tick(shifted) + dirt));
        return o;
      }

      case Cartridge::StringRinger: {
        if (slot == 0) {  // Synthetic ring: X frequency, Y resonance, Z sub
          const double hz = fx::expMap(x, 30.0, 2000.0);
          const double car = sine_(osc_, hz) + z * (sine_(osc2_, hz * 0.5) > 0 ? 0.7 : -0.7);
          const double rm = in * car;
          const double o = mod_.read(sr_ / hz);
          mod_.write(fx::softLimit(rm + 0.97 * y * o, 2.0));  // string-like comb at the carrier
          return 0.7 * (rm + o);
        }
        if (slot == 1) {  // Ring mod: X frequency, Y rate (carrier wobble), Z reverb
          const double wob = std::pow(2.0, 0.5 * lfo_.sine(fx::expMap(y, 0.05, 8.0), sr_));
          return withReverb_(in * sine_(osc_, fx::expMap(x, 20.0, 3000.0) * wob), z);
        }
        // S&H ring mod: X pitch speed (glide), Y S&H rate, Z carrier frequency
        shCount_ += fx::expMap(y, 0.3, 30.0) / sr_;
        if (shCount_ >= 1.0) { shCount_ -= 1.0; shTarget_ = 0.5 + 0.5 * noise_.next(); }
        shValue_ += (1.0 - std::exp(-fx::expMap(x, 2.0, 400.0) / sr_)) * (shTarget_ - shValue_);
        const double hz = fx::expMap(z, 40.0, 1500.0) * std::pow(2.0, 2.0 * (shValue_ - 0.5));
        return in * sine_(osc_, hz);
      }

      case Cartridge::Syntex1: {  // bass synth following the input's pitch and loudness
        trackPitch_(in);
        env_.set(0.005, 0.15, sr_);
        const double e = fx::clamp(env_.tick(in) * 3.0, 0.0, 1.5);
        double hz = trackedHz_;
        double tone;
        if (slot == 0) {  // Vibe synth: X vibrato rate, Y resonance, Z sub
          hz *= std::pow(2.0, 0.03 * lfo_.sine(fx::expMap(x, 0.5, 9.0), sr_));
          tone = saw_(osc_, hz);
          svf1_.set(hz * 6.0, 0.7 + 9.0 * y, sr_);
        } else if (slot == 1) {  // Pulse synth: X tremolo rate, Y resonance, Z sub
          (void)saw_(osc_, hz);
          tone = osc_ < 0.35 ? 0.8 : -0.8;
          tone *= 1.0 - 0.6 * (0.5 + 0.5 * lfo_.sine(fx::expMap(x, 0.5, 12.0), sr_));
          svf1_.set(hz * 5.0, 0.7 + 9.0 * y, sr_);
        } else {  // Acid synth: X tone, Y color, Z sub
          tone = saw_(osc_, hz);
          svf1_.set(fx::expMap(x, 80.0, 5000.0) * (1.0 + 3.0 * y * e), 0.8 + 12.0 * y, sr_);
        }
        subPhase_ += hz * 0.5 / sr_;
        subPhase_ -= std::floor(subPhase_);
        const double sub = subPhase_ < 0.5 ? 0.8 : -0.8;
        svf1_.tick(tone + z * sub);
        return fx::softClip(1.2 * svf1_.lp) * e;
      }

      case Cartridge::Digital: {
        const double g = slot == 1 ? 1.0 : fx::expMap(z, 1.0, 12.0);  // input gain
        const double bits = 5.0 + 11.0 * x;
        double rate = fx::expMap(x, 300.0, sr_);
        if (slot == 1) {  // LFO DAC: Y LFO speed, Z LFO amount
          rate *= std::pow(2.0, -4.0 * z * (0.5 + 0.5 * lfo_.sine(fx::expMap(y, 0.05, 12.0), sr_)));
        } else if (slot == 2) {  // Envelope crusher: Y envelope amount
          env_.set(0.002, 0.1, sr_);
          rate *= std::pow(2.0, -6.0 * y * fx::clamp(env_.tick(in) * 4.0, 0.0, 1.0));
        }
        const double c = crush_(fx::softClip(in * g * 0.5) * 2.0, rate, bits);
        if (slot == 0) {  // Filter DAC: Y cutoff
          svf1_.set(fx::expMap(y, 100.0, 18000.0), 1.2, sr_);
          svf1_.tick(c);
          return svf1_.lp;
        }
        return c;
      }

      case Cartridge::Generator: {  // self-oscillating mini synths (they sound without input)
        if (slot == 0) {  // FM tone: X pitch 1, Y pitch 2, Z FM 2->1
          const double m = sine_(osc2_, fx::expMap(y, 20.0, 2000.0));
          const double hz = fx::expMap(x, 30.0, 1500.0) * (1.0 + 4.0 * z * z * m);
          return 0.5 * sine_(osc_, std::fabs(hz)) + 0.3 * in;
        }
        if (slot == 1) {  // Ramp: X LFO rate, Y pitch, Z pitch mod +/-
          const double l = lfo_.tri(fx::expMap(x, 0.05, 20.0), sr_);
          const double hz = fx::expMap(y, 30.0, 1500.0) * std::pow(2.0, 3.0 * (z - 0.5) * 2.0 * l);
          return 0.4 * saw_(osc_, hz) + 0.3 * in;
        }
        // Voice: X cutoff, Y pitch, Z LP->HP
        const double src = 0.5 * saw_(osc_, fx::expMap(y, 40.0, 800.0)) + 0.25 * noise_.next();
        svf1_.set(fx::expMap(x, 80.0, 12000.0), 3.0, sr_);
        svf1_.tick(src);
        return 0.6 * ((1.0 - z) * svf1_.lp + z * svf1_.hp) + 0.3 * in;
      }

      case Cartridge::Orche: {  // reverse delays
        if (slot == 2) {  // Free-run loop: X delay time, Y feedback, Z delay mod (LFO <-> random)
          const double len = delaySamples_(fx::expMap(x, 0.1, 2.2));
          double modS = 0.0;
          if (z < 0.5) {
            modS = (0.5 - z) * 2.0 * 0.01 * sr_ * lfo_.sine(0.4, sr_);
          } else {
            shCount_ += 2.0 / sr_;
            if (shCount_ >= 1.0) { shCount_ -= 1.0; shTarget_ = noise_.next(); }
            shValue_ += 0.0005 * (shTarget_ - shValue_);
            modS = (z - 0.5) * 2.0 * 0.015 * sr_ * shValue_;
          }
          const double o = reverse_(len + modS);
          delay_.write(fx::softLimit(in + 0.9 * y * o, 2.0));
          return o;
        }
        // One-shot: record a phrase when the input crosses the threshold, then play it
        // backwards, repeating with decaying feedback. X time, Y feedback, Z threshold.
        const double seconds = slot == 0 ? fx::expMap(x, 0.3, 4.0) : fx::expMap(x, 0.05, 0.8);
        delay_.write(in);
        env_.set(0.002, 0.2, sr_);
        const double e = env_.tick(in);
        const double thr = fx::expMap(z, 0.005, 0.8);
        if (osState_ == 0) {
          if (e > thr) { osState_ = 1; osLen_ = seconds * sr_; osPos_ = 0.0; }
          return 0.0;
        }
        if (osState_ == 1) {  // recording
          osPos_ += 1.0;
          if (osPos_ >= osLen_) { osState_ = 2; osPos_ = 0.0; osGain_ = 1.0; osRep_ = 0.0; }
          return 0.0;
        }
        // playing backwards. Replay n starts n*len after recording ended, so the phrase
        // (end -> start) sits at delay n*len + 2*pos.
        osPos_ += 1.0;
        const double p = osPos_ / osLen_;
        const double win = std::sin(fx::kPi * fx::clamp(p, 0.0, 1.0));
        const double o = osGain_ * std::sqrt(win) * delay_.read(osRep_ * osLen_ + 2.0 * osPos_);
        if (osPos_ >= osLen_) {
          osGain_ *= 0.95 * y;
          ++osRep_;
          osPos_ = 0.0;
          if (osGain_ < 0.02 || (osRep_ + 2.0) * osLen_ > static_cast<double>(delay_.size() - 8)) {
            osState_ = 0;
          }
        }
        return o;
      }

      case Cartridge::Count:
        break;
    }
    return in;
  }

  double sr_ = 48000.0;
  int variant_ = 0;
  int program_ = 0;
  int pending_ = -1;
  double fade_ = 1.0;
  double outHp_ = 0.0, outX1_ = 0.0, outPole_ = 0.998;

  fx::DelayLine delay_, mod_, pre_;
  fx::Reverb rev_;
  fx::PitchShifter ps1_, ps2_;
  fx::Svf svf1_, svf2_;
  fx::OnePoleLP lp1_;
  fx::DcBlock dc_, dc2_;
  fx::EnvFollower env_;
  fx::Lfo lfo_;
  fx::Noise noise_;
  fx::Smooth timeSmooth_;
  double apState_[6] = {};
  double phaserFb_ = 0.0, shimmer_ = 0.0;
  double osc_ = 0.0, osc2_ = 0.0, subPhase_ = 0.0;
  double trackedHz_ = 110.0;
  int periodCount_ = 0;
  bool zcState_ = false;
  double flip_ = 1.0;
  double holdCount_ = 0.0, held_ = 0.0;
  double shTarget_ = 0.5, shValue_ = 0.5, shCount_ = 0.0;
  double revPos_ = 0.0;
  int osState_ = 0;
  double osPos_ = 0.0, osGain_ = 0.0, osLen_ = 1.0, osRep_ = 0.0;
};

// The dual effector: two slots sharing the X/Y/Z/BLEND/MASTER controls.
class DualEffector {
 public:
  static constexpr int kProgramCount = 39;

  void init(double sampleRate) {
    slot_[0].init(sampleRate, 0);
    slot_[1].init(sampleRate, 1);
    for (auto& s : smooth_) { s.setTime(0.03, sampleRate); s.y = 0.5; }
  }

  // Panel knobs, all 0..1 (registry `norm`).
  void setX(double v) { knob_[0] = fx::clamp(v, 0.0, 1.0); }
  void setY(double v) { knob_[1] = fx::clamp(v, 0.0, 1.0); }
  void setZ(double v) { knob_[2] = fx::clamp(v, 0.0, 1.0); }
  void setBlend(double v) { knob_[3] = fx::clamp(v, 0.0, 1.0); }
  void setMaster(double v) { knob_[4] = fx::clamp(v, 0.0, 1.0); }
  // CV X/Y/Z in virtual volts: +10 V adds a full knob turn.
  void setCv(int i, double volts) { if (i >= 0 && i < 3) cv_[i] = volts; }

  // The inserted cartridge per side (0..12) and the 1-2-3 switch per side (0..2).
  void setCartridge(int side, int cartridge) {
    cart_[side & 1] = cartridge < 0 ? 0 : (cartridge > 12 ? 12 : cartridge);
    update_(side & 1);
  }
  void setSelect(int side, int slot) {
    sel_[side & 1] = slot < 0 ? 0 : (slot > 2 ? 2 : slot);
    update_(side & 1);
  }
  int program(int side) const { return slot_[side & 1].program(); }
  double x() const { return knob_[0]; }
  double y() const { return knob_[1]; }
  double z() const { return knob_[2]; }
  double blend() const { return knob_[3]; }
  double master() const { return knob_[4]; }

  void process(double& l, double& r) {
    double k[5];
    for (int i = 0; i < 5; ++i) {
      const double target = i < 3 ? fx::clamp(knob_[i] + cv_[i] / 10.0, 0.0, 1.0) : knob_[i];
      k[i] = smooth_[i].tick(target);
    }
    const double wl = slot_[0].process(l, k[0], k[1], k[2]);
    const double wr = slot_[1].process(r, k[0], k[1], k[2]);
    // Equal-power dry/wet blend, then master (knob 0.5 = unity), then a soft output limit.
    const double dryG = std::cos(0.5 * fx::kPi * k[3]);
    const double wetG = std::sin(0.5 * fx::kPi * k[3]);
    const double m = 2.0 * k[4];
    l = fx::softLimit(m * (dryG * l + wetG * wl), 1.9);
    r = fx::softLimit(m * (dryG * r + wetG * wr), 1.9);
  }

 private:
  void update_(int side) { slot_[side].setProgram(cart_[side] * 3 + sel_[side]); }

  EffectorSlot slot_[2];
  fx::Smooth smooth_[5];
  double knob_[5] = {0.5, 0.5, 0.5, 0.5, 0.5};
  double cv_[3] = {0.0, 0.0, 0.0};
  int cart_[2] = {0, 0};
  int sel_[2] = {0, 0};
};

}  // namespace lunar24::core
