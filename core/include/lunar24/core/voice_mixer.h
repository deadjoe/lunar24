// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// VoiceMixer — P3-⑤: the ten-channel panoramic VOICE MIXER (manual L1099-1113,
// registry module id12 "Voice mixer"). It aggregates every audio source into a
// stereo pair: 10 mono channels, each with a PAN pot and a VOL pot, fed as
// DRONE1/2/3, EXT.AUDIO, VCO A, VCO B, PREAMP, DRONE4/5/6.
// Signal chain: → MIX/PAN → DUAL LP/BP VCF → DISTORTION → WET OUT L/R (design/01 §1).
//
// PROVENANCE (the frozen registry, generated/lunar24/registry.hpp, is the
// implementation basis; every claim below mirrors it and marks evidence strength):
//
//   * PAN (mixer.ch1_pan .. ch10_pan, id 173..191) — unit "norm", 0..1, default 0.5.
//     The registry module description is explicit: "PAN/VOL are software-normalized
//     0..1 placeholders; the manual gives no taper / dB law / center / default, so
//     all six fieldEvidence are unverified (PAN is not -1..1, VOL is not a
//     linear-gain claim)." The manual (L1105-1110) draws only an L/R spread.
//     There is NO pan law in the evidence. This header therefore uses an
//     EQUAL-POWER two-way pan (theta = 0.5*pi*p; cos/sin) because it is the
//     industry convention for a stereo pan and keeps power constant across the
//     throw; it is a PROVISIONAL modeling choice, NOT a manual fact, and is
//     recorded as an open item (P3 exit).
//   * VOL (mixer.ch1_vol .. ch10_vol, id 174..192) — unit "norm", 0..1, default 0.5.
//     Same provenance: no manual taper/dB law. The VOL is applied as a plain linear
//     amplitude gain (vol in [0,1]). PROVISIONAL, NOT a linear-gain claim; the DB
//     taper is deliberately left to the host UI mapping.
//   * CHANNEL ORDER — manual L1112 (confirmed): ch1 DRONE1, ch2 DRONE2, ch3 DRONE3,
//     ch4 EXT.AUDIO, ch5 VCO A, ch6 VCO B, ch7 PREAMP, ch8 DRONE4, ch9 DRONE5,
//     ch10 DRONE6. kChannelVcoA/kChannelVcoB index the 0-based slots used by the
//     DRY-tap (pre-chain) routing in wet_dry.h.
//   * The mixer's L/R outputs are INTERNAL endpoints (registry: "mixer *_in/out are
//     internal endpoints and are not represented as patchable jacks") — no patchable
//     jacks, no pan/vol CV inputs. Only the panel PAN and VOL knobs.
//   * Output is a raw stereo SUM: each channel contributes in*vol*pan to L/R.
//     No master fader / no headroom normalization is added here (none is in the
//     registry, and inventing one would be an un-evidenced control). The WET OUT
//     headroom (max 2 V) is the concern of wet_dry.h, documented there as a
//     declared-boundary item.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// Ten-channel mono→stereo panoramic mixer. Channel order follows the panel
// (manual L1112). PAN = equal-power spread (provisional); VOL = linear amplitude
// (provisional). No master gain.
class VoiceMixer {
 public:
  // Panel channel order (0-based slots). kNumChannels = 10.
  enum Channel : int {
    kChannelDrone1 = 0,
    kChannelDrone2 = 1,
    kChannelDrone3 = 2,
    kChannelExtAudio = 3,
    kChannelVcoA = 4,   // DRY OUT A is tapped HERE, before the chain.
    kChannelVcoB = 5,   // DRY OUT B is tapped HERE, before the chain.
    kChannelPreamp = 6,
    kChannelDrone4 = 7,
    kChannelDrone5 = 8,
    kChannelDrone6 = 9,
  };
  static constexpr int kNumChannels = 10;

  VoiceMixer() {
    for (int i = 0; i < kNumChannels; ++i) {
      vol_[i] = 0.5;  // registry default.
      pan_[i] = 0.5;  // registry default (0.5 = center).
    }
  }

  // Registry default 0.5. Clamped to [0,1]. Pan is NOT -1..1 (registry explicit).
  void setChannelVol(int ch, double v) {
    if (ch < 0 || ch >= kNumChannels) return;
    vol_[ch] = clamp01_(v);
  }
  void setChannelPan(int ch, double p) {
    if (ch < 0 || ch >= kNumChannels) return;
    pan_[ch] = clamp01_(p);
  }

  double channelVol(int ch) const { return (ch >= 0 && ch < kNumChannels) ? vol_[ch] : 0.0; }
  double channelPan(int ch) const { return (ch >= 0 && ch < kNumChannels) ? pan_[ch] : 0.0; }

  // Render one frame: 10 mono channel samples -> stereo (L, R).
  // Each channel: L += in*vol*panL, R += in*vol*panR, with the equal-power law.
  void tick(const double in[kNumChannels], double& outL, double& outR) const {
    double l = 0.0, r = 0.0;
    for (int i = 0; i < kNumChannels; ++i) {
      const double theta = 0.5 * kPi * pan_[i];   // equal-power pan (provisional).
      const double panL = std::cos(theta);
      const double panR = std::sin(theta);
      const double g = in[i] * vol_[i];
      l += g * panL;
      r += g * panR;
    }
    outL = l;
    outR = r;
  }

  // CONFIRMED default / center (registry 0.5). The pan/vol TAPER is provisional.
  static constexpr double kDefaultNorm = 0.5;
  static constexpr double kMaxNorm = 1.0;

 private:
  static double clamp01_(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  }
  static constexpr double kPi = 3.14159265358979323846;

  double vol_[kNumChannels];
  double pan_[kNumChannels];
};

}  // namespace lunar24::core
