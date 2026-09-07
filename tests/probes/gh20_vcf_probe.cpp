// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh20_vcf_probe.cpp — task #87 (GH #20) VCF cross-sample-rate response probe.
//
// Reproduces the audit finding N-3 (and N-4) with REAL-PRODUCT measurements, not an
// analytic re-derivation. The single finding is that the Chamberlin SVF (PolivoksFilter)
// caps the norm->Hz cutoff at sr/8 (polivoks_vcf.h kCutoffCapRatio = 1/8), so:
//   (a) at 44.1 kHz the FREQ knob is INERT for norm >~ 0.813 (cutoff pinned at 5512.5 Hz);
//   (b) an 8 kHz input has a DIFFERENT low-pass gain at 44.1 kHz (fc=5512.5 < 8k, rejected)
//       vs 96 kHz (fc=12000 > 8k, passed) for the SAME knob setting norm=1.
//
// MEASUREMENT PATH — the ONE agreed real entry, reused verbatim from task#85:
//   make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//   DeviceAdapter::renderBlock (processBlock), driven by EngineHarness (tests/host/).
// The probe does NOT compute the analysis (that is tools/gh20_vcf_analyze.py applied to
// these real samples); it only guarantees the DSP that is measured is the product's.
//
// SIGNAL / ATTRIBUTION (how a clean VCF response is isolatable):
//   Physical in0 = EXT.AUDIO terminal (device_adapter inputCh[0]=extCh=0) -> mixer ch4
//   (kChannelExtAudio) -> mixL/mixR -> PolivoksFilter.tick_ -> distortion (vcf_dist=0 is an
//   EXACT dry passthrough, per distortion.h "dist=0 -> output is exactly the dry term x")
//   -> WET L/R. The mixer is equal-power pan x vol (frequency-independent); the VCF input
//   stage (GH#6 tanh) is ~linear at small signal; so the WET magnitude is the VCF transfer
//   magnitude cascaded with ONE constant gain — the ONLY frequency-dependent stage in this
//   chain is the VCF itself, which is the attribution point. vcf_l_mod=0 (registry default)
//   zeroes any CV shift, so vcf_l_freq alone sets the cutoff (no default-CV confound).
//
// MATRIX (every cell required=1 in tools/gh20_manifest.tsv; none are blocked). @Codex correction 1
// restored the low/mid/high res + two legal input levels so a res=0-only sweep cannot mask the
// res-dependence of the response:
//   * cutoff_norm  — sr x norm x freq log sweep, mode=LP, res x level. The analyzer interpolates
//     the -3 dB cutoff per (sr,norm,res,level) and detects the sr/8 plateau (finding N-3a). The
//     plateau onset must be res-INDEPENDENT; the SHAPE per res is the real res-dependence.
//   * crossrate    — sr x mode(LP|BP) x norm x {low_ref, 8000 Hz} x res x level: the analyzer
//     computes the low-band normalised gain and reports the exact sample-rate difference (N-3b),
//     PER res — the res-dependence of the gap (model 2.21/1.78/2.56 dB) is the point.
//   * asym_lr      — sr: vcf_l_freq=0.9, vcf_r_freq=0.3, same input: proves the product
//     consumes the L/R FREQ knobs independently (a same-input asymmetric response).
//   * level        — sr=44.1k, norm=0.9, freq=1k, 2 legal input levels: the VCF input-stage
//     level-dependence (GH#6 tanh at inputDrive=0 => passthrough) as a composite attribution
//     (NOT the VCF response) — must be ~linear.
//   * floor        — per-sr zero-input idle output: the numerical noise floor that bounds
//     every amplitude below (labels the resolvability of the matrix).
//
// A cell that yields no legal stimulus is recorded with signal=<reason> and counts as a hard
// failure (exit non-zero) — the analyzer is the final gate, but a silent/over-scale/
// non-finite/rejected cell must never masquerade as success (BLOCK item ⑤ transfer).
//
// OUTPUT (--out <dir>, default ./report/gh20-probe):
//   gh20_scenarios.tsv   one metadata row per measured point (id, sr, mode, res, norm, lvl,
//                        freq_hz, channel, amp_rms, amp_peak, f0_meas, noise_rms, window,
//                        ok, signal).
//   gh20_cpu.tsv         machine / toolchain / per-sample cost / block-partition evidence.
//
// EXIT CODE: 0 = all good; non-zero = any required cell not produced, a raw write failed, or
//   a four-output finite/block-consistency failure.

// This probe writes .tsv with std::fopen, which MSVC's secure-CRT deprecation (C4996)
// promotes to an error under /W4 /WX. Scope the suppression to this TU only (as the GH#19
// probe does) — define it before any CRT header is included.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <lunar24/core/device_state.h>
#include <lunar24/core/state_default.h>
#include <lunar24/registry_ids.hpp>

#include <host/standalone_audio_engine.h>
#include "test_engine_harness.h"   // the ONE shared real entry (tests/host/)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#if !defined(_WIN32)
#include <sys/utsname.h>   // POSIX-only; MSVC has no equivalent.
#endif
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;

namespace {

constexpr std::uint64_t kProbeSeed = 0x4C554E4152ULL;     // LUNAR hex (same as harness).
constexpr std::size_t kWarm = 4096;                       // settle transients.
constexpr std::size_t kWin = 16384;                       // steady-state window.
constexpr std::size_t kBlock = kWarm + kWin;              // one processBlock per cell.

const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};

// The EXT.AUDIO feed is on PHYSICAL in0 (device_adapter inputCh[0]=extCh=0, InputRoute::Distinct
// for kInCh=2). in1 would be the PREAMP (preampCh=1); we leave it zero so no preamp tanh enters.

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

bool allFinite(const std::vector<double>& c) {
  for (double v : c) if (!std::isfinite(v)) return false;
  return true;
}
double rmsOf(const std::vector<double>& c, std::size_t begin) {
  double s = 0.0;
  for (std::size_t i = begin; i < c.size(); ++i) s += c[i] * c[i];
  const std::size_t n = c.size() - begin;
  return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}
double peakOf(const std::vector<double>& c, std::size_t begin) {
  double m = 0.0;
  for (std::size_t i = begin; i < c.size(); ++i) {
    const double a = std::fabs(c[i]);
    if (a > m) m = a;
  }
  return m;
}
// Dominant frequency of the window via zero-crossing count — O(n), independent of the known
// stimulus (so f0_meas is a genuine MEASUREMENT label, not a copy of freq_hz). For a pure sine
// through a linear filter the period is preserved; ZCR tracks it exactly to the window resolution.
// Used ONLY to label f0_meas (the stimulus is a known sine; the analyzer trusts freq_hz).
double dominantHz(const std::vector<double>& c, std::size_t begin, double sr) {
  const std::size_t n = c.size() - begin;
  if (n < 4) return 0.0;
  std::size_t zc = 0;
  for (std::size_t i = begin + 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++zc;
  }
  return sr * static_cast<double>(zc) / (2.0 * static_cast<double>(n));
}

// Noise floor: after fitting (removing) the stimulus sinusoid, the residual RMS is the
// numerical floor that bounds this cell's resolvability (a pure sine in a linear filter has
// no other content). Single render, no second idle pass needed.
double residualNoiseRms(const std::vector<double>& c, std::size_t begin, double freq, double sr) {
  const std::size_t n = c.size() - begin;
  if (n < 4 || freq <= 0.0) return 0.0;
  const double w = 2.0 * 3.14159265358979323846 * freq / sr;
  double reS = 0.0, imS = 0.0, norm = 0.0;
  for (std::size_t i = begin; i < c.size(); ++i) {
    const double ph = w * static_cast<double>(i - begin);
    const double co = std::cos(ph), si = std::sin(ph);
    reS += c[i] * co; imS += c[i] * si;
    norm += co * co + si * si;
  }
  if (norm <= 1e-15) return 0.0;
  const double a = 2.0 * reS / norm, b = -2.0 * imS / norm;   // x(t) ~ a*cos(wt)+b*sin(wt).
  double s = 0.0;
  for (std::size_t i = begin; i < c.size(); ++i) {
    const double ph = w * static_cast<double>(i - begin);
    const double d = c[i] - (a * std::cos(ph) + b * std::sin(ph));
    s += d * d;
  }
  return std::sqrt(s / static_cast<double>(n));
}

// A per-sr stimulus frequency set that stays inside the measurable band (well below Nyquist so
// the input sine is clean) and spans the sr/8 caps so the -3 dB cutoff is interpolable at every
// norm in the plateau zone. Every set contains 8000 Hz (the cross-sample-rate focus, N-3b) and
// a low anchor (60/100/200 Hz) deep inside the pass band so the -3 dB reference is unity. These
// are the SHARED specification: tools/gen_gh20_manifest.py uses the SAME table to emit the
// committed manifest, so the produced-id set must equal the required-id set exactly.
const std::vector<double>& probeFreqs(double sr) {
  static const std::vector<double> f44k = {60, 100, 200, 400, 800, 1200, 1600, 2500,
                                           3500, 4500, 5500, 6500, 7000, 8000, 9000};
  static const std::vector<double> f48k = {60, 100, 200, 400, 800, 1200, 1600, 2500,
                                           3500, 5000, 6000, 7000, 8000, 9000, 10000};
  static const std::vector<double> f88k = {60, 100, 200, 400, 800, 1500, 2500, 4000, 6000,
                                           8000, 10000, 12000, 14000, 16000, 18000};
  static const std::vector<double> f96k = {60, 100, 200, 400, 800, 1500, 2500, 4000, 6000,
                                           8000, 10000, 12000, 15000, 18000, 20000};
  if (sr == 44100.0) return f44k;
  if (sr == 48000.0) return f48k;
  if (sr == 88200.0) return f88k;
  return f96k;
}

// The norm grid swept for the cutoff_norm plateau (21 points, step 0.05, per the GH#20 mandate
// "norm>=21 points"). Norms near/above the sr/8 onset differ per sr (44.1k onset ~0.813, 96k onset
// ~0.926), so the dense grid interpolates the knee precisely; the highest two (0.95, 1.0) are capped
// at EVERY sr (20*1000^0.95 = 14158 > all sr/8 caps) => a uniform plateau probe. The label is a
// manifest-shared token (norm label -> value in gen_gh20_manifest.py). THIS TABLE IS THE SINGLE
// SPEC: it must byte-match the one in tools/gen_gh20_manifest.py NORM_GRID and the analyzer normval.
const std::vector<std::pair<double, char const*>>& normGrid() {
  static const std::vector<std::pair<double, char const*>> g = {
      {0.00, "0"},    {0.05, "0p05"}, {0.10, "0p1"},  {0.15, "0p15"}, {0.20, "0p2"},
      {0.25, "0p25"}, {0.30, "0p3"},  {0.35, "0p35"}, {0.40, "0p4"},  {0.45, "0p45"},
      {0.50, "0p5"},  {0.55, "0p55"}, {0.60, "0p6"},  {0.65, "0p65"}, {0.70, "0p7"},
      {0.75, "0p75"}, {0.80, "0p8"},  {0.85, "0p85"}, {0.90, "0p9"},  {0.95, "0p95"},
      {1.00, "1"},
  };
  return g;
}
// res sweep: {0 flat, 0.5 mid, 1.0 max} — the three LEGAL res values (damp = 2.0 / 1.05 / 0.1). The
// res enters the VCF ONLY via the damp coefficient, so a res=0-only sweep masks res-dependence of the
// 8k cross-rate gain, the stability margin, and the response SHAPE. The id token (tok) and the emitted
// res column (col) byte-match tools/gen_gh20_manifest.py RES_TABLE and the analyzer's grouping key.
// level sweep: {small=0.05, medium=0.20} — the two legal input levels. inputDrive defaults to 0 =>
// the input stage is an EXACT passthrough (no tanh fold), so the response is level-invariant; sweeping
// the two legal levels bounds the VCF attribution at both. The level token/column byte-match LEVEL_TABLE.
struct ResCase { double res; char const* tok; char const* col; };
static const ResCase kSweepRes[] = {
    {0.0, "0", "0"}, {0.5, "0p5", "0.5"}, {1.0, "1", "1"},
};
struct LvlCase { double lvl; char const* tok; char const* col; };
static const LvlCase kSweepLvl[] = {{0.05, "small", "0.05"}, {0.20, "medium", "0.2"}};
// BOTH the cutoff_norm and crossrate groups sweep LP|BP (the mandatory "LP/BP, norm>=21 points").

// Configure the state so the ONLY audible mixer input is the EXT.AUDIO channel (ch4) and the
// VCF chain downstream is transparent distortion: muting everything except ch4 removes the
// VCO/drone/preamp contributions; dist=0+gain=0 makes distortion an exact dry passthrough.
void configureIsolatedVcf(DeviceStateV1& st, double freqL, double freqR, double resL, double resR,
                          bool lpL, bool lpR, double mixVol) {
  // Mute ALL mixer channels then un-mute only ch4 (EXT.AUDIO). mixer_ch(n)_vol stride = 2.
  for (int c = 1; c <= 10; ++c) {
    const auto pid = static_cast<ParameterId>(static_cast<int>(ParameterId::mixer_ch1_vol) + (c - 1) * 2);
    slot(st, pid) = (c == 4) ? mixVol : 0.0;
  }
  slot(st, ParameterId::vcf_dist) = 0.0;   // distortion dry passthrough (exact).
  slot(st, ParameterId::vcf_gain) = 0.0;   // gain=0 -> distorted term = dry (no hardness).
  slot(st, ParameterId::vcf_l_freq) = freqL;
  slot(st, ParameterId::vcf_r_freq) = freqR;
  slot(st, ParameterId::vcf_l_res) = resL;
  slot(st, ParameterId::vcf_r_res) = resR;
  slot(st, ParameterId::vcf_l_bp_lp) = lpL ? 1.0 : 0.0;   // 1=LP, 0=BP.
  slot(st, ParameterId::vcf_r_bp_lp) = lpR ? 1.0 : 0.0;
  slot(st, ParameterId::vcf_l_mod) = 0.0;   // zero CV shift -> knob freq alone sets cutoff.
  slot(st, ParameterId::vcf_r_mod) = 0.0;
  slot(st, ParameterId::vcf_link) = 0.0;
}

struct Cap {
  bool ok = false;
  std::string signal;
  double ampRms = 0.0, ampPeak = 0.0, f0 = 0.0, noise = 0.0;
};

// Render one stimulus and capture the steady-state WET channel amplitude. `feed0` supplies the
// physical in0 (EXT.AUDIO) value each frame; in1 (preamp) is pinned to 0.
Cap measure(const DeviceStateV1& st, double sr, double freq, double amp,
            const std::string& chan) {
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr, static_cast<int>(kBlock))) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  const double w = 2.0 * 3.14159265358979323846 * freq / sr;
  bool ok = h.renderBlock(static_cast<int>(kBlock),
      [&](std::size_t f, double& in0, double& in1) {
        in0 = amp * std::sin(w * static_cast<double>(f));
        in1 = 0.0;
      });
  if (!ok) { c.signal = "render-failed"; return c; }
  const std::vector<double>* src = nullptr;
  if (chan == "wetL") src = &h.wetL();
  else if (chan == "wetR") src = &h.wetR();
  else { c.signal = "bad-chan"; return c; }
  if (src->size() < kBlock) { c.signal = "short-render"; return c; }
  if (!allFinite(*src)) { c.signal = "non-finite"; return c; }
  c.ampRms = rmsOf(*src, kWarm);
  c.ampPeak = peakOf(*src, kWarm);
  c.f0 = dominantHz(*src, kWarm, sr);
  c.noise = residualNoiseRms(*src, kWarm, freq, sr);
  // Over-scale guard (un-scaled-ideal substitution detector, BLOCK item ①): reject only an output a
  // REAL product response cannot produce. The measured bounded max across this matrix is the res=1
  // resonance peak = 0.709 (lvl 0.20); res=0/0.5 stay < 0.08. The per-content bound is 1.0 — a
  // full-scale / genuinely un-scaled ideal substitute (>= 1.0 at these test levels) is the over-scale
  // signature. It is NOT the flat-res 0.55 from the res=0-only sweep (0.55 falsely rejects the real
  // res=1 resonance; a res-swept product legitimately exceeds it). NOTE: we do NOT reject on small
  // amplitude here — a low-pass at a frequency far above its cutoff legitimately attenuates to ~0
  // (that is the response we are measuring), so a stopband cell is recorded (tiny ampRms) and the
  // analyzer decides whether it is above the machine noise floor. The silent-substitute / empty-data
  // detection is the analyzer's job, applied to the deep-passband reference cells where the signal
  // MUST be present.
  if (c.ampPeak > 1.0) { c.signal = "over-scale"; c.ok = false; return c; }
  c.ok = true;
  return c;
}

bool writeTsv(const std::string& path, const std::vector<std::string>& lines) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
  std::fclose(f);
  return true;
}
std::string safeTsv(const std::string& s) { return s.empty() ? "-" : s; }
std::string f2(double v) { char b[64]; std::snprintf(b, sizeof(b), "%.9g", v); return std::string(b); }
std::string isr(double sr) { return std::to_string(static_cast<long long>(sr)); }
std::string ifreq(double freq) { return std::to_string(static_cast<long long>(std::llround(freq))); }

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh20-probe";
  for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
  std::error_code ec; std::filesystem::create_directories(out, ec);

  int gCode = 0;
  std::vector<std::string> rows;
  rows.push_back("id\tsr_hz\tmode\tres\tnorm\tlvl\tfreq_hz\tchannel\tamp_rms\tamp_peak\t"
                 "f0_meas_hz\tnoise_rms\twindow\tok\tsignal");

  auto emit = [&](const std::string& id, double sr, const std::string& mode, double res,
                  double norm, double lvl, double freq, const std::string& chan,
                  const Cap& c, std::size_t window) {
    rows.push_back(id + "\t" + f2(sr) + "\t" + mode + "\t" + f2(res) + "\t" + f2(norm) +
                   "\t" + f2(lvl) + "\t" + f2(freq) + "\t" + chan + "\t" +
                   f2(c.ok ? c.ampRms : -1.0) + "\t" + f2(c.ok ? c.ampPeak : -1.0) + "\t" +
                   f2(c.f0) + "\t" + f2(c.noise) + "\t" + std::to_string(window) + "\t" +
                   std::to_string(c.ok ? 1 : 0) + "\t" + safeTsv(c.signal));
    if (!c.ok) { std::fprintf(stderr, "FATAL %s: required cell not produced [%s]\n", id.c_str(), c.signal.c_str()); gCode |= 1; }
  };

  const double kLvlSmall = 0.05, kLvlMed = 0.20;   // the two legal device-normalised input levels.
  const double kMixVol = 0.5;

  // ---- Floor per sample rate: zero-input idle output bounds every amplitude. ----
  // (An idle signal at the EXT input is the machine's own noise + the constant chain gain.)
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    configureIsolatedVcf(st, 0.5, 0.5, 0.0, 0.0, true, true, kMixVol);
    Cap c;
    EngineHarness h;
    if (h.load(st, sr, (int)kBlock) && h.runtime() != nullptr) {
      bool ok = h.renderBlock((int)kBlock, [](std::size_t, double& in0, double& in1){ in0 = 0.0; in1 = 0.0; });
      c.ampRms = ok ? rmsOf(h.wetL(), kWarm) : -1.0;
      c.ampPeak = ok ? peakOf(h.wetL(), kWarm) : -1.0;
      c.ok = ok;
    } else { c.signal = "load-rejected"; }
    emit("floor_" + std::to_string((int)sr), sr, "lp", 0.0, 0.5, 0.0, 0.0, "wetL", c, kWin);
  }

  // ---- cutoff_norm: sr x mode(LP|BP) x norm x freq sweep, res x level. The analyzer finds the -3 dB
  //      cutoff per (sr,mode,norm,res,level) and detects the sr/8 plateau (finding N-3a). The plateau
  //      onset must be res-INDEPENDENT (the cap does not read res), while the response SHAPE per res
  //      is a real difference — this is the correction-1 res coverage. BOTH modes are swept at the full
  //      21-point norm (mandate "LP/BP, norm>=21 points"); the mode is embedded in the id. ----
  {
    for (double sr : kSrs) {
      const auto freqs = probeFreqs(sr);
      for (int modeIdx : {0, 1}) {                 // 0 = BP, 1 = LP.
        const bool lp = (modeIdx == 1);
        for (const auto& np : normGrid()) {
          const double norm = np.first;
          const char* nlabel = np.second;
          for (double freq : freqs) {
            for (const ResCase& rc : kSweepRes) {
              for (const LvlCase& lc : kSweepLvl) {
                DeviceStateV1 st = make_default_device_state(kProbeSeed);
                configureIsolatedVcf(st, norm, norm, rc.res, rc.res, lp, lp, kMixVol);
                Cap c = measure(st, sr, freq, lc.lvl, "wetL");
                emit("cutoff_norm_sr" + isr(sr) + "_" + (lp ? "lp" : "bp") +
                     "_r" + rc.tok + "_lvl" + lc.tok + "_n" + nlabel + "_f" + ifreq(freq),
                     sr, lp ? "lp" : "bp", rc.res, norm, lc.lvl, freq, "wetL", c, kWin);
              }
            }
          }
        }
      }
    }
  }

  // ---- crossrate: sr x mode(LP|BP) x norm(>=0.85) x {low_ref, 8000 Hz} x res x level — the
  //      sample-rate gain difference at the same knob setting (finding N-3b). The gap is the
  //      RES-DEPENDENT finding (model 2.21 / 1.78 / 2.56 dB); per-res here is the point (correction 1). ----
  {
    for (double sr : kSrs) {
      for (int modeIdx : {0, 1}) {                 // 0 = BP, 1 = LP.
        const bool lp = (modeIdx == 1);
        for (const auto& np : normGrid()) {
          const double norm = np.first;
          const char* nlabel = np.second;
          if (norm < 0.85) continue;   // only the cap-zone / high-res norms (the 8k cross-rate focus).
          for (double freq : {100.0, 8000.0}) {
            for (const ResCase& rc : kSweepRes) {
              for (const LvlCase& lc : kSweepLvl) {
                DeviceStateV1 st = make_default_device_state(kProbeSeed);
                configureIsolatedVcf(st, norm, norm, rc.res, rc.res, lp, lp, kMixVol);
                Cap c = measure(st, sr, freq, lc.lvl, "wetL");
                emit("crossrate_sr" + isr(sr) + "_" + (lp ? "lp" : "bp") +
                     "_r" + rc.tok + "_lvl" + lc.tok + "_n" + nlabel + "_f" + ifreq(freq),
                     sr, lp ? "lp" : "bp", rc.res, norm, lc.lvl, freq, "wetL", c, kWin);
              }
            }
          }
        }
      }
    }
  }

  // ---- asym_lr: vcf_l_freq != vcf_r_freq, SAME input. The product must consume the L/R FREQ
  //      knobs independently (a real same-input asymmetric response), else the VCF knobs are
  //      not actually routed. wetL != wetR is the negative-control-normal evidence. ----
  {
    for (double sr : kSrs) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      configureIsolatedVcf(st, 0.90, 0.30, 0.0, 0.0, true, true, kMixVol);   // LP, L high / R low.
      const double freq = 1000.0;
      Cap cL = measure(st, sr, freq, kLvlSmall, "wetL");
      Cap cR = measure(st, sr, freq, kLvlSmall, "wetR");
      emit("asym_lr_sr" + isr(sr) + "_wetL", sr, "lp", 0.0, 0.90, kLvlSmall, freq, "wetL", cL, kWin);
      emit("asym_lr_sr" + isr(sr) + "_wetR", sr, "lp", 0.0, 0.30, kLvlSmall, freq, "wetR", cR, kWin);
    }
  }

  // ---- level: sr=44.1k, LP, res=0, norm=0.9, freq=1k, 2 legal input levels — the VCF input-stage
  //      level-dependence (GH#6 tanh at inputDrive=0 => exact passthrough) as a composite attribution,
  //      NOT the VCF response. small->medium must be ~linear (12.04 dB). ----
  {
    const char* llabels[] = {"small", "medium"};
    const double lvals[] = {kLvlSmall, kLvlMed};
    for (int i = 0; i < 2; ++i) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      configureIsolatedVcf(st, 0.90, 0.90, 0.0, 0.0, true, true, kMixVol);
      Cap c = measure(st, 44100.0, 1000.0, lvals[i], "wetL");
      emit("level_" + std::string(llabels[i]), 44100.0, "lp", 0.0, 0.90, lvals[i], 1000.0, "wetL", c, kWin);
    }
  }

  // ---- Machine evidence: four-output finite/block-consistency + block-partition + prep/cost.
  {
    std::string machine = "unknown", sysrel = "unknown", compiler = "unknown";
#if !defined(_WIN32)
    struct utsname un;
    if (uname(&un) == 0) { machine = un.machine; sysrel = std::string(un.sysname) + " " + un.release; }
#else
    machine = "windows";
#endif
#ifdef _MSC_VER
    compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    compiler = __VERSION__;
#endif
    const double sr = 48000.0;
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    configureIsolatedVcf(st, 0.5, 0.5, 0.0, 0.0, true, true, kMixVol);
    long fourFail = 0; std::string fourNote = "clean";
    for (double s2 : kSrs) {
      EngineHarness h;
      if (!h.load(st, s2, (int)kBlock)) { ++fourFail; fourNote = "load-fail@" + std::to_string((int)s2); continue; }
      if (!h.renderBlock((int)kBlock, [](std::size_t, double& in0, double& in1){ in0 = 0; in1 = 0; })) {
        ++fourFail; fourNote = "render-fail@" + std::to_string((int)s2); continue;
      }
      if (!allFinite(h.dryA()) || !allFinite(h.dryB()) || !allFinite(h.wetL()) || !allFinite(h.wetR())) {
        ++fourFail; fourNote = "non-finite@" + std::to_string((int)s2); continue;
      }
    }
    if (fourFail) gCode |= 4;

    // Block-partition invariance: per-sample renderBlock(1) repeated vs one renderBlock(kBlock).
    double bpMax = 0.0; std::string bpNote = "clean";
    {
      EngineHarness hf, hb;
      if (!hf.load(st, sr, (int)kBlock)) bpNote = "frame-load-fail";
      else if (!hb.load(st, sr, (int)kBlock)) bpNote = "block-load-fail";
      else {
        bool fok = true;
        for (int i = 0; i < (int)kBlock && fok; ++i)
          fok = hf.renderBlock(1, [](std::size_t, double& in0, double& in1){ in0 = 0; in1 = 0; });
        if (!fok) bpNote = "frame-render-fail";
        else if (!hb.renderBlock((int)kBlock, [](std::size_t, double& in0, double& in1){ in0 = 0; in1 = 0; }))
          bpNote = "block-render-fail";
      }
      if (bpNote == "clean") {
        for (int ch = 0; ch < 4; ++ch) {
          const auto& a = hf.out(ch);
          const auto& b = hb.out(ch);
          if (a.size() != b.size()) { bpNote = "len@" + std::to_string(ch); break; }
          for (std::size_t i = 0; i < a.size(); ++i) {
            const double d = std::fabs(a[i] - b[i]);
            if (d > bpMax) bpMax = d;
          }
        }
        if (bpMax > 1e-9) bpNote = "mismatch";
      }
      if (bpMax > 1e-9) gCode |= 8;
    }

    std::vector<std::string> crows;
    crows.push_back("id\tmachine\tos\tcompiler\tvalue\tunit\tsr_hz");
    crows.push_back("cpu_four_output\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(fourFail) + "\tfinite_block_failcount\t" + std::to_string((int)sr));
    crows.push_back("cpu_four_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(fourNote) + "\tnote\t" + std::to_string((int)sr));
    crows.push_back("cpu_blockpartition\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + f2(bpMax) + "\tblock_partition_maxdiff\t" + std::to_string((int)sr));
    crows.push_back("cpu_blockpartition_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(bpNote) + "\tnote\t" + std::to_string((int)sr));
    std::string cpuTsv = out + "/gh20_cpu.tsv";
    if (!writeTsv(cpuTsv, crows)) { std::fprintf(stderr, "FATAL: could not write %s\n", cpuTsv.c_str()); gCode |= 2; }
  }

  std::string tsv = out + "/gh20_scenarios.tsv";
  if (!writeTsv(tsv, rows)) { std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str()); gCode |= 2; }
  std::size_t produced = 0, blocked = 0;
  for (const auto& r : rows) {
    if (r.rfind("id\t", 0) == 0) continue;
    // signal is the 15th field (index 14 after splitting on tab).
    std::vector<std::string> flds;
    std::size_t p = 0;
    for (std::size_t q = r.find('\t'); q != std::string::npos; q = r.find('\t', p)) {
      flds.push_back(r.substr(p, q - p)); p = q + 1;
    }
    flds.push_back(r.substr(p));
    if (flds.size() > 14) {
      const std::string sig = flds[14];
      if (sig.empty() || sig == "-") ++produced; else ++blocked;
    }
  }
  std::fprintf(stderr, "gh20_vcf_probe: %zu cells produced, %zu blocked, exit_code=%d.\n",
               produced, blocked, gCode);
  return gCode;
}
