// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// PhotoSensor — the light-sensitive eye of a classic drone voice (DRONE 1/2/4/5).
//
// On the Solar 42N each classic voice has a PHOTO-SENSITIVE DETECTOR: with a MOD button on,
// that generator's pitch follows the light falling on the sensor (manual p.7), so a hand
// over the eye, or just the room light, bends the drone. Software has no light, so this
// models the chain from the light to the pitch:
//
//   room light: a slow Ornstein-Uhlenbeck wander (stops around the room's average)
//     x hand shadow: soft S-curve edge, physiological tremor (the 8-12 Hz band plus broadband
//       irregularity), a slower sway, and flicker while the fingers sweep across the eye
//     -> CdS photoresistor:
//        * conductance follows illuminance as a power law, G ~ E^gamma (gamma ~0.7 for CdS);
//        * it reacts in two parts: a fast one (tens of ms, the datasheet rise/decay times)
//          and a slow tail that takes seconds to settle when the light goes away, like the
//          vactrol models (fast attack, slow decay: Parker & D'Angelo, DAFx-13; Csound
//          `vactrol` defaults 20 ms up / 3 s down);
//        * light-history memory: after a long time in the dark it comes back to the light
//          more slowly;
//        * a little noise.
//     -> the oscillator: the cell sits beside a fixed resistor, so the pitch change is
//        log2((r + G) / (r + 1)) — near-linear around room light, saturating in the dark.
//     -> DroneBank applies it to the MOD-on generators (darker = lower).
//
// The hand is the only input: setShade(0..1) is how close it is (0 = away, 1 = on the eye).
// Everything else moves by itself, so the same shade never sounds the same twice. The
// manual gives no numbers: depths and times are tuned by ear on top of the CdS figures.
//
// Realtime-safe: no allocation, no locks. Deterministic for a given seed (its own splitmix
// stream, advanced at a fixed control rate), so offline renders repeat exactly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lunar24::core {

class PhotoSensor {
 public:
  static constexpr int kControlInterval = 8;          // samples per model step
  // Light and hand.
  static constexpr double kAmbientDriftStops = 0.18;  // room light wander // tuned by ear
  static constexpr double kAmbientSeconds = 9.0;      // how slowly the room light moves
  static constexpr double kFullShadeTransmit = 0.03;  // light left with the hand on the eye
  static constexpr double kTremorDepth = 0.035;       // hand tremor in shade units // tuned by ear
  static constexpr double kSwayDepth = 0.05;          // slower hand sway (~1 Hz) // tuned by ear
  static constexpr double kFlickerDepth = 0.35;       // shadow flicker while the hand moves // tuned by ear
  static constexpr double kSweepSpeedup = 3.0;        // a sweeping hand's shadow moves this much faster
  static constexpr double kSweepDistance = 0.5;       // shade travelled in ~0.25 s = full sweep
  // CdS cell.
  static constexpr double kGamma = 0.7;               // conductance ~ illuminance^gamma (CdS)
  static constexpr double kFastShare = 0.65;          // part of the response that is fast
  static constexpr double kFastUpSeconds = 0.02;      // fast part: brightening ...
  static constexpr double kFastDownSeconds = 0.04;    // ... and darkening (datasheet scale)
  static constexpr double kSlowUpSeconds = 0.25;      // slow tail: brightening ...
  static constexpr double kSlowDownSeconds = 3.0;     // ... and darkening (vactrol-like)
  static constexpr double kDarkMemory = 4.0;          // after a long cover, up to 5x slower back
  static constexpr double kNoiseStops = 0.01;         // cell noise
  // Pitch.
  static constexpr double kSeriesR = 0.25;            // fixed resistor beside the cell (relative)
  static constexpr double kOctavesPerUnit = 0.08;     // detune depth // tuned by ear

  void prepare(std::uint64_t seed, double sampleRate) {
    rng_ = seed ^ 0x50484F544FULL;  // "PHOTO"
    dt_ = kControlInterval / (sampleRate > 0.0 ? sampleRate : 48000.0);
    countdown_ = 0;
    shadeTarget_ = hand_ = lastTarget_ = motion_ = 0.0;
    ambient_ = 0.0;
    tremor_[0] = tremor_[1] = 0.0;
    sway_ = flicker_ = noise_ = 0.0;
    fast_ = slow_ = 0.0;
    darkHistory_ = 0.0;
    octaves_ = 0.0;
  }

  // The hand: 0 = away, 1 = covering the eye.
  void setShade(double shade) { shadeTarget_ = std::clamp(shade, 0.0, 1.0); }
  double shade() const { return shadeTarget_; }
  // Room light drift and cell noise on/off (off = still air, for tests that need a fixed pitch).
  void setAmbientEnabled(bool on) { ambientOn_ = on; }

  // One audio sample. Returns the detune in octaves (0 in average room light, < 0 shaded).
  double tick() {
    if (--countdown_ > 0) return octaves_;
    countdown_ = kControlInterval;
    step_();
    return octaves_;
  }

  double octaves() const { return octaves_; }
  // What the cell sees, in stops relative to the room's average (0 = room, about -5 = covered).
  double lightStops() const { return kFastShare * fast_ + (1.0 - kFastShare) * slow_; }
  // The same as 0..1 for a lamp: 1 = room light, 0 = dark.
  double light01() const { return std::clamp(std::exp2(lightStops()), 0.0, 1.0); }

 private:
  double unit_() {  // uniform in [-1, 1)
    std::uint64_t z = (rng_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;
  }
  // One-pole low-pass coefficient for a corner at `hz`, and for a time constant.
  double pole_(double hz) const { return 1.0 - std::exp(-2.0 * 3.141592653589793 * hz * dt_); }
  double follow_(double seconds) const { return 1.0 - std::exp(-dt_ / seconds); }

  void step_() {
    // Room light: an Ornstein-Uhlenbeck wander around the average (in stops).
    if (ambientOn_) {
      ambient_ += -ambient_ * dt_ / kAmbientSeconds +
                  kAmbientDriftStops * std::sqrt(2.0 * dt_ / kAmbientSeconds) * unit_() * 1.732;
    }

    // How much the hand has moved lately (fingers sweeping across the eye) drives the flicker:
    // the distance travelled over about the last quarter second.
    motion_ = motion_ * (1.0 - follow_(0.25)) + std::abs(shadeTarget_ - lastTarget_);
    lastTarget_ = shadeTarget_;
    const double moving = std::min(1.0, motion_ / kSweepDistance);
    // The hand moves toward where it is told, a little faster in than out. While the fingers
    // sweep, the shadow edges pass at once (no arm to move), so it follows much faster.
    const bool handIn = shadeTarget_ > hand_;
    const double sweep = 1.0 + kSweepSpeedup * moving;
    hand_ += follow_((handIn ? 0.06 : 0.12) / sweep) * (shadeTarget_ - hand_);

    // A hand in the air is never still: tremor (white noise band-passed to ~7-12 Hz: random
    // input reproduces the physiological spectrum) and a slow sway. Both only matter while
    // the hand is near, so an untouched eye sees only the room.
    const double presence = std::min(1.0, hand_ * 4.0);
    const double n1 = unit_(), n2 = unit_(), n3 = unit_(), n4 = unit_();
    tremor_[0] += pole_(12.0) * (n1 - tremor_[0]);
    tremor_[1] += pole_(7.0) * (tremor_[0] - tremor_[1]);
    const double tremor = (tremor_[0] - tremor_[1]) * 6.0;  // band-pass, ~unit level
    sway_ += pole_(1.2) * (n2 * 3.0 - sway_);
    flicker_ += pole_(25.0) * (n3 * 2.0 - flicker_);
    noise_ += pole_(40.0) * (n4 - noise_);
    double shade = hand_ + presence * (kTremorDepth * tremor + kSwayDepth * sway_) +
                   kFlickerDepth * moving * flicker_;
    shade = std::clamp(shade, 0.0, 1.0);

    // Soft shadow: the light falls off along an S-curve as the hand closes in.
    const double s = shade * shade * (3.0 - 2.0 * shade);
    const double transmit = 1.0 - (1.0 - kFullShadeTransmit) * s;
    const double target = (ambientOn_ ? ambient_ : 0.0) + std::log2(transmit);

    // The CdS cell, in stops: a fast part and a slow tail, each quicker toward light than
    // toward dark; after a long time dark, the way back to the light is slower (memory).
    darkHistory_ += follow_(3.0) * (std::clamp(-lightStops() / 4.0, 0.0, 1.0) - darkHistory_);
    const double memory = 1.0 + kDarkMemory * darkHistory_;
    fast_ += follow_(target > fast_ ? kFastUpSeconds * memory : kFastDownSeconds) * (target - fast_);
    slow_ += follow_(target > slow_ ? kSlowUpSeconds * memory : kSlowDownSeconds) * (target - slow_);

    // Conductance G = E^gamma beside a fixed resistor: the pitch moves by log2((r+G)/(r+1)).
    const double stops = lightStops() + (ambientOn_ ? kNoiseStops * noise_ : 0.0);
    const double g = std::exp2(kGamma * stops);
    octaves_ = kOctavesPerUnit * std::log2((kSeriesR + g) / (kSeriesR + 1.0));
  }

  std::uint64_t rng_ = 0;
  double dt_ = kControlInterval / 48000.0;
  int countdown_ = 0;
  bool ambientOn_ = true;
  double shadeTarget_ = 0.0, hand_ = 0.0, lastTarget_ = 0.0, motion_ = 0.0;
  double ambient_ = 0.0;
  double tremor_[2] = {0.0, 0.0};
  double sway_ = 0.0, flicker_ = 0.0, noise_ = 0.0;
  double fast_ = 0.0, slow_ = 0.0, darkHistory_ = 0.0;
  double octaves_ = 0.0;
};

}  // namespace lunar24::core
