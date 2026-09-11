// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_alias_probe.cpp — task #85 (GH #19) REAL-PRODUCT anti-aliasing probe.
//
// Drives the REAL product output through the ONE agreed entry (the same chain the host uses):
//     make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//     standalone_audio_engine.processBlock (encapsulated by EngineHarness),
// and captures the rendered audio for the FULL required-scenario matrix declared by
// tools/gh19_manifest.tsv (BLOCK item ①: a committed contract the analyzer enforces). It does NOT
// compute the aliasing analysis — that is the verified Python method (tools/gh19_alias_analyze.py)
// applied to these real samples. This file is the guard that the measurement truly samples the
// product's DSP, not an ideal() stand-in or a silent buffer, and that every REQUIRED cell is either
// produced or an explicit hard failure (exit non-zero), never a silent pass.
//
// OBSERVATION POINTS (each is a real production tap, verified against the runtime):
//   dryA  = VCO A, tapped BEFORE the chain  (core/include/lunar24/core/machine_runtime.h:117)
//   dryB  = VCO B, tapped BEFORE the chain  (machine_runtime.h:118)
//   droneChannel(0) = pre-mixer classic drone-1 channel bus (machine_runtime.h:1633)
//   droneChannel(2) = pre-mixer classic drone-4 channel bus (classicGroupOfDrone_(drone_4) == 2)
//   drone3Channel()/drone6Channel() = new-drone PapaVoice audio bus (machine_runtime.h:1642-1643)
//   wetL  = the WET mix output channel (device_layout WET_L=0)
//
// CELLS (all required=1 in the manifest; every one must be produced — none are declared blocked):
//   * vco_a_tri / vco_b_tri — VCO A/B default triangle, f by oct_sel x tune, tap dryA / dryB.
//   * drone1_classic        — classic drone gen-1 single carrier (mute_2..5=1), tap droneChannel(0).
//                             Role LOW: group 0 gen 0 (drone_bank.h:373-378 band 30-120 Hz).
//   * drone4_classic        — classic drone gen-4 single carrier on drone_4 (mute_1..3,5=1), tap
//                             droneChannel(2). Group 2 gen 3, but at the same 30-semis-down VOLT as
//                             the LOW cell its generators land at 97-224 Hz — i.e. inside the LOW
//                             band, so this row alone does NOT discharge RULING ③ (@Kimi cd345dbb);
//                             it is kept as a non-vacuity probe of the same accessor.
//   * drone4_classic_v0     — the row that DOES discharge RULING ③: same drone_4 gen 4 recipe with
//                             the VOLT knob at 0.0 => 0 semis down, so the excited generators land at
//                             ~551/835/1266 Hz inside the 420-1800 Hz kHigh band (@Kimi e6e645d2
//                             ruling 1, approving these 12 cells). The `_v0` suffix is load-bearing:
//                             a cell id must name its volt lane or baseline and acceptance rows can
//                             be paired across lanes and still look entirely plausible.
//   * drone3_schmitt / drone6_schmitt — new-drone clean periodic Schmitt tone (pitch>0, fm/am/noise=0).
//   * preamp_ac             — preamp tanh input stage on the WET bus, driven by an AC sine on ch1.
//   * wet_chain             — composite kVcfPath WET output (COMPOSITE cell, not a module attribution).
//
// A cell that produces no legal stimulus is recorded with signal=<reason> AND counts as a hard
// failure (exit non-zero) because it is a REQUIRED manifest cell — the manifest's `required=0` rows
// (sub, saw/pulse/morph) are deferred module probes deliberately NOT produced here.
//
// OUTPUT (--out <dir>, default ./report/gh19-probe):
//   gh19_scenarios.tsv    one metadata row per recorded cell (with measured f0 / peak)
//   gh19_scnNNN.raw       little-endian f64 samples of that cell's observation window
//   gh19_cpu.tsv          four-output finite/block-consistency + block-partition + prep/callback cost.
//   gh19_fidelity.tsv     classic cells only: runtime raw vs probe-built drift-ON raw (must be bit
//                         identical) and drift-ON vs drift-OFF (must differ) -- the provenance proof
//                         for ruling B, where the classic cells' emitted raw is the drift-OFF
//                         self-built render. See the classic block below.
//
// EXIT CODE (BLOCK item ⑤: production failure / raw-write failure / block mismatch must surface):
//   0 = all good; non-zero = any required cell not produced, a raw write failed, a four-output
//   finite/block-consistency check failed, a block-partition mismatch was observed, or (bit 16) a
//   classic-cell probe-fidelity assertion failed.

// This probe writes .raw/.tsv with std::fopen, which MSVC's secure-CRT deprecation
// (C4996) promotes to an error under this repo's /W4 /WX warning policy. No other test
// target uses fopen, so the suppression is scoped to this single test/probe translation
// unit only (defined before any CRT header is included).
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <lunar24/core/device_state.h>
#include <lunar24/core/drone_bank.h>   // probe-built classic bank (ruling B, driftEnabled=false)
#include <lunar24/core/state_default.h>
#include <lunar24/registry_ids.hpp>

#include <host/standalone_audio_engine.h>
#include "test_engine_harness.h"   // the ONE shared real entry (tests/host/)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#if !defined(_WIN32)
#include <sys/utsname.h>   // POSIX-only; MSVC has no equivalent (machine info falls back below)
#endif
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;

namespace {

constexpr std::uint64_t kProbeSeed = 0x4C554E4152ULL;     // same LUNAR hex as the harness default
constexpr std::size_t kWarm = 8192;                       // settle transients
constexpr std::size_t kWin = 16384;                       // analysis window (after warmup)
const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

double peakOf(const std::vector<double>& c) {
  double m = 0.0;
  for (double v : c) {
    const double a = std::fabs(v);
    if (a > m) m = a;
  }
  return m;
}
bool allFinite(const std::vector<double>& c) {
  for (double v : c) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}
// Zero crossings per window -> measured frequency (2 crossings per period for a triangle).
double zcrFreq(const std::vector<double>& c, double sr) {
  int z = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  }
  return static_cast<double>(z) * sr / (2.0 * static_cast<double>(c.size()));
}

double medianOf(std::vector<double> v) {
  if (v.empty()) return -1.0;
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}
double stdevOf(const std::vector<double>& v, double mean) {
  if (v.size() < 2) return 0.0;
  double s = 0.0;
  for (double x : v) { const double d = x - mean; s += d * d; }
  return std::sqrt(s / (static_cast<double>(v.size()) - 1.0));
}

struct Cap {
  bool ok = false;
  std::string signal;        // "" = produced; else a hard-failure reason ("silent", "rejected-*")
  std::vector<double> x;
  double peak = 0.0;
  double f0 = 0.0;
};

// Capture `tap` from a single engine render of `frames` frames at `sr` under the given state.
// `sampled` selects a runtime getter tap (drone buses); otherwise it reads an output channel buffer.
// `domain` = the tap's voltage domain, which fixes the scale-guard bounds:
//   "device" -> physical device output (normalized/0.5 scale): peak must be in [1e-4, 0.55].
//   "volt"   -> a pre-mixer diagnostic bus (volts): peak must be in [1e-3, 5.5].
// A "device" tap whose peak breaches 0.55 is exactly the un-scaled-ideal substitution detector.
Cap capture(const DeviceStateV1& st, double sr, std::size_t frames, const std::string& tap,
            double preampV, const std::string& domain = "device") {
  const double lo = (domain == "volt") ? 1e-3 : 1e-4;
  const double hi = (domain == "volt") ? 5.5 : 0.55;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  if (tap == "drone1" || tap == "drone4" || tap == "drone3" || tap == "drone6") {
    std::vector<double> d;
    d.reserve(frames);
    bool ok = h.renderSampled(static_cast<int>(frames), preampV,
        [&](const lunar24::core::SynthRuntime& rt) {
          double v;
          if (tap == "drone1") v = rt.droneChannel(0);
          // classicGroupOfDrone_(drone_4) == 2 (machine_runtime.h:3456-3463), so the drone-4 cell
          // reads VoiceMixer::kChannelDrone4 via droneChannel(2) -- the same accessor the drone-1
          // cell uses, not a re-derivation.
          else if (tap == "drone4") v = rt.droneChannel(2);
          else if (tap == "drone3") v = rt.drone3Channel();
          else v = rt.drone6Channel();
          d.push_back(v);
        });
    if (!ok) { c.signal = "render-failed"; return c; }
    if (d.size() != frames) { c.signal = "short-sampled"; return c; }
    c.x = std::move(d);
  } else {
    if (!h.render(static_cast<int>(frames), preampV)) { c.signal = "render-failed"; return c; }
    const std::vector<double>* src = nullptr;
    if (tap == "dry_a") src = &h.dryA();
    else if (tap == "dry_b") src = &h.dryB();
    else if (tap == "wet_l") src = &h.wetL();
    else { c.signal = "bad-tap"; return c; }
    if (src->size() < frames) { c.signal = "short-render"; return c; }
    c.x.assign(src->begin(), src->begin() + frames);
  }
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; c.ok = false; return c; }
  if (c.peak < lo) { c.signal = "silent"; c.ok = false; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; c.ok = false; return c; }   // substitution detector
  c.ok = true;
  return c;
}

// AC-input capture: feeds a zero-centred sine on physical ch1 (the preamp feed) so the nonlinear
// input stage sees a real periodic stimulus — BLOCK item ④: production accepts an AC input via the
// processBlock input array, so the preamp is NOT a product blocker (it only needs an AC stimulus,
// not an oversample comparison, to produce a measurable cell).
Cap captureAc(const DeviceStateV1& st, double sr, std::size_t frames, double amp, double acHz,
              const std::string& tap, const std::string& domain = "device") {
  const double lo = (domain == "volt") ? 1e-3 : 1e-4;
  const double hi = (domain == "volt") ? 5.5 : 0.55;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  const double w = 2.0 * 3.14159265358979323846 * acHz / sr;
  bool ok = h.renderFeed(static_cast<int>(frames),
      [&](std::size_t f, double& in0, double& in1) {
        in0 = 0.0;
        in1 = amp * std::sin(w * static_cast<double>(f));
      });
  if (!ok) { c.signal = "render-failed"; return c; }
  const std::vector<double>* src = nullptr;
  if (tap == "wet_l") src = &h.wetL();
  else if (tap == "dry_a") src = &h.dryA();
  else if (tap == "dry_b") src = &h.dryB();
  else { c.signal = "bad-tap"; return c; }
  if (src->size() < frames) { c.signal = "short-render"; return c; }
  c.x.assign(src->begin(), src->begin() + frames);
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; c.ok = false; return c; }
  if (c.peak < lo) { c.signal = "silent"; c.ok = false; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; c.ok = false; return c; }
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
bool writeRaw(const std::string& path, const std::vector<double>& x) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  for (double v : x) std::fwrite(&v, sizeof(double), 1, f);
  std::fclose(f); return true;
}

std::string safeTsv(const std::string& s) { return s.empty() ? "-" : s; }

// ---------------------------------------------------------------------------------------------
// Classic-cell render off the SHIPPED runtime path (ruling B, @Kimi f260fd91).
//
// The S2 acceptance column pins phi ANALYTICALLY at the frequency the accumulator actually ran at.
// That frequency is a CONSTANT only when the drift model is inert: with drift off, tickGroup's
// `effFreq = base*(1+tolerance) + driftNow` collapses to `base*(1+tolerance)` (drone_bank.h:275-283),
// so phi enters as 2*pi*frac(warm*f0/sr) exactly and the only free parameter left is the scale. With
// drift on, the same accumulator is frequency-MODULATED and no constant-frequency reference can
// represent it (measured: the free-phase best fit on those arms is -1 dB order vs -33..-44 dB on the
// drift-off arms -- ~30 dB apart, with no oracle involved).
//
// driftEnabled=false is UNREACHABLE through the product: SynthRuntime's member is constructed with
// the default (drift ON, machine_runtime.h:428) and DroneBank exposes no setter. So the classic cells
// are rendered here by a probe-built bank. That swap is only admissible if the self-built bank IS the
// shipped path, which is not assumed: the probe renders BOTH ways and asserts the drift-ON raws are
// bit-identical (gh19_fidelity.tsv). The configuration below therefore replicates the runtime's
// control mapping and its canonical execution block line for line:
//   * mute gens 1..4                    -- runtime: drone_1_mute_2..5 = 1
//   * setTune(0, (n-0.5)*24.0)          -- runtime: classicDroneTuneSemisFromNorm (machine_runtime.h:2789)
//   * setVolt(0, 30.0)                  -- runtime: classicDroneVoltSemisDownFromNorm(0.5) (machine_runtime.h:2792)
//   * setGroupHold/setGroupGate(0,true) -- runtime: drone_1_gate_hold=1 (the gate is never set from
//                                          params, so it stays at kDefaultGroupGateOpen = true)
//   * tickGroup(0, out5) per frame, channel = sum of the 5 group samples
//                                       -- runtime: machine_runtime.h:3069-3078, droneChannel(0)
struct ClassicSelf {
  Cap cap;
  double f0_model = 0.0;   // analytic accumulator rate used below ONLY when drift is off; else 0.
};

// `group` is a CLASSIC group index (0..3) and `gen` a generator WITHIN it (0..4). The ROLE is a
// property of the generator, not of the group (drone_bank.h:373-378: gen 0,1 = kLow 30-120 Hz,
// gen 2 = kMedium 140-380, gens 3,4 = kHigh 420-1800), so a HIGH-role cell is this same recipe with
// `gen` pointing at a high generator of its own group -- nothing else about the render changes.
ClassicSelf captureClassicSelf(int group, std::size_t gen, double sr, double tuneNorm,
                               double voltSemisDown, bool driftEnabled) {
  const double lo = 1e-3;   // same "volt"-domain scale guard as capture(...)
  const double hi = 5.5;
  ClassicSelf out;
  lunar24::core::DroneBank bank(kProbeSeed, sr, lunar24::core::DroneBank::kMaxVoices, driftEnabled);
  const std::size_t flat = static_cast<std::size_t>(group) * lunar24::core::DroneBank::kGensPerVoice + gen;
  // setMute/setTune take the FLAT voice index; setVolt takes the GROUP (drone_bank.h:201-211). Only
  // `group` is ever ticked below, so the other groups' mute flags cannot enter this raw.
  for (std::size_t i = 0; i < lunar24::core::DroneBank::kMaxVoices; ++i) bank.setMute(i, i != flat);
  bank.setTune(flat, (tuneNorm - 0.5) * 24.0);
  bank.setVolt(group, voltSemisDown);
  bank.setGroupHold(group, true);
  bank.setGroupGate(group, true);
  const std::size_t total = kWarm + kWin;
  std::vector<double> d;
  d.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    double out5[lunar24::core::DroneBank::kGensPerVoice] = {};
    bank.tickGroup(group, out5);
    double s = 0.0;
    for (std::size_t g = 0; g < lunar24::core::DroneBank::kGensPerVoice; ++g) s += out5[g];
    d.push_back(s);
  }
  // The analytic constant rate, read from the SAME instance that produced the samples (never
  // re-derived from the requested tune): base*2^(tune/12)*2^(-volt/12)*(1+tolerance). NOTE:
  // DroneBank::effectiveFreqHz (drone_bank.h:317-321) is NOT this value -- it applies tolerance to
  // freqBaseHz instead of to the transposed base, so it must not be used as the frequency oracle.
  if (!driftEnabled) {
    const double tune = bank.tuneOf(flat), volt = bank.voltOf(flat);
    out.f0_model = bank.freqBaseHz(flat) * std::pow(2.0, tune / 12.0) * std::pow(2.0, -volt / 12.0) *
                   (1.0 + bank.toleranceOf(flat));
  }
  out.cap.x = std::move(d);
  out.cap.peak = peakOf(out.cap.x);
  out.cap.f0 = zcrFreq(out.cap.x, sr);
  if (!allFinite(out.cap.x)) { out.cap.signal = "non-finite"; return out; }
  if (out.cap.peak < lo) { out.cap.signal = "silent"; return out; }
  if (out.cap.peak > hi) { out.cap.signal = "over-scale"; return out; }
  out.cap.ok = true;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh19-probe";
  for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
  // Portable recursive create — the previous "mkdir -p '" + out + "'" invokes a POSIX shell;
  // on the MSVC CI runner that resolves to cmd.exe where `mkdir` has no -p and the literal
  // single quotes are kept, so the output dir is never created and fopen("wb") fails below.
  std::error_code ec; std::filesystem::create_directories(out, ec); /* best effort */

  int gCode = 0;   // 0 = clean; OR-in failure flags on any produced-but-failed / write-fail / block mismatch.

  std::vector<std::string> rows;
  rows.push_back("id\tpath\tsignal\tsr_hz\tf0_target_hz\tf0_meas_hz\tpeak\tchannel\twave\traw\tsamples"
                 "\tf0_model_hz");
  int scnIdx = 0;

  // Classic-cell provenance evidence (ruling B): one row per classic cell, written to
  // gh19_fidelity.tsv and asserted by the analyzer's probe-contract gate.
  std::vector<std::string> frows;
  frows.push_back("id\tsamples\trt_vs_selfON_max_abs_diff\tbit_identical"
                  "\tdriftON_vs_OFF_max_abs_diff\tdrift_off_differs");

  auto fmtNum = [](double v, int prec) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*g", prec, v);
    return std::string(b);
  };

  // Emit a scenario row. Every PRODUCED cell gets an indexed raw sample file. A required cell that
  // is NOT produced is a hard failure (exit non-zero) — the analyzer is the final gate, but the probe
  // must fail loudly so a silent/over-scale/non-finite/rejected cell never masquerades as success.
  // `f0_model_hz` (classic cells only) is the analytic constant accumulator rate the emitted raw ran
  // at; the analyzer pins phi with it. It is "-" for every other cell.
  auto emit = [&](const std::string& id, const std::string& path, double sr, double ft, const Cap& c,
                  const std::string& channel, const std::string& wave,
                  const std::string& f0_model = "-") {
    std::string raw = "";
    if (c.ok) {
      raw = "gh19_scn" + std::to_string(++scnIdx) + ".raw";
      if (!writeRaw(out + "/" + raw, c.x)) {
        std::fprintf(stderr, "FATAL %s: raw write failed (%s)\n", id.c_str(), raw.c_str());
        gCode |= 2;
      }
    } else {
      std::fprintf(stderr, "FATAL %s: required cell not produced [%s]\n", id.c_str(), c.signal.c_str());
      gCode |= 1;
    }
    rows.push_back(id + "\t" + path + "\t" + safeTsv(c.signal) +
                   "\t" + std::to_string(sr) +
                   "\t" + std::to_string(ft) +
                   "\t" + std::to_string(c.f0) +
                   "\t" + std::to_string(c.peak) +
                   "\t" + channel + "\t" + wave +
                   "\t" + raw + "\t" + std::to_string(c.x.size()) +
                   "\t" + f0_model);
  };

  // ---- VCO A default triangle: 4 SRS x 3 frequencies. Reachable, clean, periodic (analytic target).
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      double oct = (ft == 220.0) ? 0.0 : 1.0;
      double tune = (ft == 880.0) ? 1.0 : 0.0;
      slot(st, ParameterId::vco_a_oct_sel) = oct;
      slot(st, ParameterId::vco_a_tune) = tune;
      Cap c = capture(st, sr, kWarm + kWin, "dry_a", 0.0);
      emit("vco_a_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft),
           "vco_a_tri", sr, ft, c, "dry_a", "tri");
    }
  }

  // ---- VCO B triangle: 4 SRS x 3 frequencies (item ③ coverage for the B generator, and item ⑥
  //      verification of the A/B shared VCO call point). VCO-B shares the Vco class with VCO-A, but the
  //      DEFAULT graph feeds vco_b.cv_in live from vco_a.dry_out (the task#83 A->B route,
  //      machine_runtime.h:2226-2237), so with a non-zero vco_b_cv_amt VCO-B is audio-rate FM-ed by
  //      VCO-A's triangle and dryB is NOT a clean single carrier. To measure VCO-B as a STEADY
  //      carrier (comparable to VCO-A, and to verify the shared oscillator), we set the legal panel
  //      knob vco_b_cv_amt=0 so the CV input has zero depth. The default (cv_amt>0) FM-spread is a
  //      documented product finding, not the clean-triangle target — see the report.
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      double oct = (ft == 220.0) ? 0.0 : 1.0;
      double tune = (ft == 880.0) ? 1.0 : 0.0;
      slot(st, ParameterId::vco_b_oct_sel) = oct;
      slot(st, ParameterId::vco_b_tune) = tune;
      slot(st, ParameterId::vco_b_cv_amt) = 0.0;   // steady single carrier (see note above)
      Cap c = capture(st, sr, kWarm + kWin, "dry_b", 0.0);
      emit("vco_b_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft),
           "vco_b_tri", sr, ft, c, "dry_b", "tri");
    }
  }

  // ---- Classic drone gen-1 SINGLE carrier: mute_2..5=1 (only gen-1 audible), gate hold on. ----
  //
  // RULING B (@Kimi f260fd91): the raw this cell SHIPS is the probe-built bank's drift-OFF render,
  // not the runtime render -- the analytic phi pin is exact only where the accumulator rate is a
  // constant, and driftEnabled=false is unreachable from the product. The runtime render is still
  // taken, and `gh19_fidelity.tsv` asserts (a) runtime == self-built drift-ON bit for bit (the fork
  // detector: it is what proves the self-built bank is the same path) and (b) drift-ON != drift-OFF
  // (the non-vacuity control: it is what proves the shipped arm really is the drift-OFF arm). Both
  // are hard failures here and in the analyzer's probe-contract gate. See captureClassicSelf.
  // Roles are a property of the generator WITHIN a group (drone_bank.h:373-378). RULING ③
  // (@Kimi cd345dbb) requires a HIGH-role classic cell in the acceptance gate, so the same recipe is
  // run across three lanes: the LOW cell on drone_1's gen 1 (group 0, gen 0 -- the 30-120 Hz band),
  // and the HIGH cell on drone_4's gen 4 (group 2, gen 3, classicGroupOfDrone_(drone_4) == 2) at
  // TWO volt settings.
  //
  // `voltSemisDown` is the CLASSIC VOLT setting in semitones down, i.e. the value the runtime
  // derives from the *_volt knob via classicDroneVoltSemisDownFromNorm (machine_runtime.h:2791:
  // 60*n, so 0.5 -> 30 and 0.0 -> 0). It is a parameter rather than a constant because the HIGH
  // ROLE CELL MUST LAND IN ITS OWN BAND. This is the one place where "the same recipe at a
  // different setting" would be wrong: with the LOW lane's 30 semis down, drone_4's gen 3 lands at
  // 97-224 Hz, which duplicates the LOW band and makes the HIGH cell HIGH in name only. The probe's
  // first 12 HIGH rows did exactly that, and the honest reading of them is "a second LOW cell with
  // a different base frequency", not a HIGH-role cell (@Kimi e6e645d2 ruling 1, approving the
  // fix). At 0 semis down the same generators land at ~551/835/1266 Hz -- inside the 420-1800 Hz
  // kHigh band the role is DEFINED by. So `volt=0` is the volt setting that makes the HIGH lane an
  // actual HIGH lane; the 30-semis-down HIGH rows are kept (they are a valid LOW-adjacent probe of
  // the same accessor and cost nothing to keep) but they are NOT what discharges ruling ③.
  //
  // Consequence for the gate, stated here because it is easy to get backwards: the two HIGH volt
  // lanes are NOT interchangeable rows and their pathName prefix carries the distinction
  // (`drone4_classic` vs `drone4_classic_v0`). A cell id must say which volt it was rendered at,
  // or the baseline row and the acceptance row can be paired across lanes and every number will
  // still look plausible.
  auto classicCells = [&](const std::string& pathName, const std::string& tap, int group,
                          std::size_t gen, const ParameterId (&mute)[5], ParameterId tuneId,
                          ParameterId holdId, ParameterId voltId, double voltNorm,
                          double voltSemisDown) {
    for (double sr : kSrs) {
      for (double tuneNorm : {0.20, 0.50, 0.80}) {
        DeviceStateV1 st = make_default_device_state(kProbeSeed);
        for (int k = 0; k < 5; ++k)
          slot(st, mute[k]) = (static_cast<std::size_t>(k) == gen) ? 0.0 : 1.0;
        slot(st, holdId) = 1.0;
        slot(st, tuneId) = tuneNorm;
        // The VOLT knob must be set on the STATE as well, not only on the self-built bank: the
        // runtime capture below is what the fork detector compares against, and a state left at the
        // default 0.5 would render volt=30 while the self-built bank renders voltSemisDown. That
        // mismatch would fail the detector for the right reason but with the wrong diagnosis (looks
        // like a fork, is actually a recipe divergence), so the two are threaded from ONE argument.
        slot(st, voltId) = voltNorm;
        const std::string cid = pathName + "_" + std::to_string((int)sr) + "_t" +
                                std::to_string(std::lround(tuneNorm * 100));

        Cap c_rt = capture(st, sr, kWarm + kWin, tap, 0.0, "volt");
        ClassicSelf on = captureClassicSelf(group, gen, sr, tuneNorm, voltSemisDown,
                                            /*driftEnabled=*/true);
        ClassicSelf off = captureClassicSelf(group, gen, sr, tuneNorm, voltSemisDown,
                                             /*driftEnabled=*/false);

        // (a) fork detector: the runtime raw and the self-built drift-ON raw must be identical.
        double d_on = -1.0;
        bool same = c_rt.ok && on.cap.ok && c_rt.x.size() == on.cap.x.size();
        if (same) {
          double m = 0.0;
          for (std::size_t i = 0; i < c_rt.x.size(); ++i) {
            const double d = std::fabs(c_rt.x[i] - on.cap.x[i]);
            if (d > m) m = d;
          }
          d_on = m;
          same = (m == 0.0);
        }
        // (b) non-vacuity: flipping ONLY the drift flag must change the render.
        double d_off = -1.0;
        bool differs = false;
        if (on.cap.ok && off.cap.ok && on.cap.x.size() == off.cap.x.size()) {
          double m = 0.0;
          for (std::size_t i = 0; i < on.cap.x.size(); ++i) {
            const double d = std::fabs(on.cap.x[i] - off.cap.x[i]);
            if (d > m) m = d;
          }
          d_off = m;
          differs = (m > 0.0);
        }
        if (!same || !differs) {
          std::fprintf(stderr,
                       "FATAL %s: probe fidelity failed (bit_identical=%s, drift_off_differs=%s)\n",
                       cid.c_str(), same ? "YES" : "NO", differs ? "YES" : "NO");
          gCode |= 16;
        }
        frows.push_back(cid + "\t" + std::to_string(off.cap.x.size()) + "\t" + fmtNum(d_on, 6) + "\t" +
                        (same ? "YES" : "NO") + "\t" + fmtNum(d_off, 6) + "\t" +
                        (differs ? "YES" : "NO"));

        const std::string f0_model = (off.f0_model > 0.0) ? fmtNum(off.f0_model, 12) : std::string("-");
        emit(cid, pathName, sr, off.cap.f0, off.cap, tap, "sawcubic", f0_model);
      }
    }
  };

  // LOW role: drone_1 gen 1 (group 0, gen 0), volt knob at its runtime default 0.5 => 30 semis down.
  classicCells("drone1_classic", "drone1", 0, 0,
               {ParameterId::drone_1_mute_1, ParameterId::drone_1_mute_2, ParameterId::drone_1_mute_3,
                ParameterId::drone_1_mute_4, ParameterId::drone_1_mute_5},
               ParameterId::drone_1_tune_1, ParameterId::drone_1_gate_hold,
               ParameterId::drone_1_volt, /*voltNorm=*/0.5, /*voltSemisDown=*/30.0);
  // HIGH role at 30 semis down: drone_4 gen 4 (group 2, gen 3). Kept -- but this lane's generators
  // land at 97-224 Hz, inside the LOW band, so it does NOT discharge ruling ③ (see the block above).
  classicCells("drone4_classic", "drone4", 2, 3,
               {ParameterId::drone_4_mute_1, ParameterId::drone_4_mute_2, ParameterId::drone_4_mute_3,
                ParameterId::drone_4_mute_4, ParameterId::drone_4_mute_5},
               ParameterId::drone_4_tune_4, ParameterId::drone_4_gate_hold,
               ParameterId::drone_4_volt, /*voltNorm=*/0.5, /*voltSemisDown=*/30.0);
  // HIGH role, THE LANE THAT DISCHARGES RULING ③: same drone_4 gen 4, volt knob at 0.0 => 0 semis
  // down, so the excited generators land at ~551/835/1266 Hz inside the 420-1800 Hz kHigh band.
  // Rendered on the UNFIXED tree like every other baseline row -- that is what makes it a baseline.
  classicCells("drone4_classic_v0", "drone4", 2, 3,
               {ParameterId::drone_4_mute_1, ParameterId::drone_4_mute_2, ParameterId::drone_4_mute_3,
                ParameterId::drone_4_mute_4, ParameterId::drone_4_mute_5},
               ParameterId::drone_4_tune_4, ParameterId::drone_4_gate_hold,
               ParameterId::drone_4_volt, /*voltNorm=*/0.0, /*voltSemisDown=*/0.0);

  // ---- New-drone drone-3 Schmitt: clean periodic tone (pitch>0, fm/am/noise=0), 2 pitch levels. ----
  for (double sr : kSrs) {
    for (double pitchNorm : {0.20, 0.60}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      slot(st, ParameterId::drone_3_pitch) = pitchNorm;
      slot(st, ParameterId::drone_3_noise) = 0.0;
      slot(st, ParameterId::drone_3_fm) = 0.0;
      slot(st, ParameterId::drone_3_am) = 0.0;
      Cap c = capture(st, sr, kWarm + kWin, "drone3", 0.0, "volt");
      emit("drone3_schmitt_" + std::to_string((int)sr) + "_p" + std::to_string(std::lround(pitchNorm * 100)),
           "drone3_schmitt", sr, c.f0, c, "drone3", "schmitt");
    }
  }

  // ---- New-drone drone-6 Schmitt: clean periodic tone, single pitch level (p30). ----
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::drone_6_pitch) = 0.30;
    slot(st, ParameterId::drone_6_noise) = 0.0;
    slot(st, ParameterId::drone_6_fm) = 0.0;
    slot(st, ParameterId::drone_6_am) = 0.0;
    Cap c = capture(st, sr, kWarm + kWin, "drone6", 0.0, "volt");
    emit("drone6_schmitt_" + std::to_string((int)sr) + "_p30",
         "drone6_schmitt", sr, c.f0, c, "drone6", "schmitt");
  }

  // ---- Preamp tanh input stage on the WET bus, driven by an AC sine on ch1 (item ④). ----
  for (double sr : kSrs) {
    for (int lvl : {0, 1}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      for (int c = 1; c <= 10; ++c) {
        const int base = static_cast<int>(ParameterId::mixer_ch1_vol);
        auto pid = static_cast<ParameterId>(base + (c - 1) * 2);  // ch(n)_vol stride = 2
        slot(st, pid) = 0.0;
      }
      // preamp channel index 6 -> mixer_ch7_vol (registry mixer_ch(n+1)_vol). Scale DOWN (0.5) so
      // the saturated tanh output stays inside the device-domain [1e-4, 0.55] scale guard.
      slot(st, ParameterId::mixer_ch7_vol) = 0.5;
      slot(st, ParameterId::preamp_gain) = (lvl == 0) ? 0.3 : 1.0;
      double amp = (lvl == 0) ? 0.2 : 0.6;   // device-normalized sine amplitude on ch1
      const double acHz = 440.0;
      Cap c = captureAc(st, sr, kWarm + kWin, amp, acHz, "wet_l");
      emit("preamp_ac_" + std::to_string((int)sr) + "_l" + std::to_string(lvl),
           "preamp_ac", sr, acHz, c, "wet_l", "tanh");
    }
  }

  // ---- Composite WET chain (default kVcfPath) — COMPOSITE cell, never a module attribution. ----
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    Cap c = capture(st, sr, kWarm + kWin, "wet_l", 0.0);
    emit("wet_chain_" + std::to_string((int)sr) + "_" + std::to_string((int)sr),
         "wet_chain", sr, c.f0, c, "wet_l", "composite");
  }

  // ---- Machine evidence: four-output finite/block-consistency, block-partition invariance, prep
  //      time, and a median/variance per-sample callback cost + real-block size. LOCAL evidence
  //      only — the mandate's CPU bound is "this machine, not a cross-machine budget". Written as a
  //      SEPARATE gh19_cpu.tsv so gh19_scenarios.tsv stays signal-only (the analyzer parses the 3rd
  //      field of a scenario row as "signal"; a cpu meta row there would be miscounted).
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

    const double sr = kSrs[1];                    // 48000
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::vco_a_oct_sel) = 1.0;   // 440 Hz clean VCO-A triangle
    slot(st, ParameterId::vco_a_tune) = 0.0;

    // (a) Four outputs finite + block-consistent across every sample rate. The harness render() only
    // returns true when EVERY processBlock is Rendered (block-consistency); the four shared block
    // size, so equal lengths + all-finite is the per-record contract. Any failure => exit code.
    long fourFail = 0; std::string fourNote = "clean";
    for (double s2 : kSrs) {
      EngineHarness h;
      if (!h.load(st, s2)) { ++fourFail; fourNote = "load-fail@" + std::to_string((int)s2); continue; }
      const int n = 4096;
      if (!h.render(n, 0.0)) { ++fourFail; fourNote = "render-fail@" + std::to_string((int)s2); continue; }
      if (!allFinite(h.dryA()) || !allFinite(h.dryB()) || !allFinite(h.wetL()) || !allFinite(h.wetR())) {
        ++fourFail; fourNote = "non-finite@" + std::to_string((int)s2); continue;
      }
      if (h.dryA().size() != static_cast<std::size_t>(n) || h.dryB().size() != static_cast<std::size_t>(n) ||
          h.wetL().size() != static_cast<std::size_t>(n) || h.wetR().size() != static_cast<std::size_t>(n)) {
        ++fourFail; fourNote = "len@" + std::to_string((int)s2); continue;
      }
    }
    if (fourFail) gCode |= 4;

    // (b) BLOCK-PARTITION invariance (BLOCK item ⑤): same state + same input, compare per-frame
    //      render() against a single renderBlock() (one processBlock call) across all four outputs.
    //      Any divergence is a real finding (a block-size-dependent DSP path). Requires frames <= the
    //      blockFrames the engine was loaded with, so both loads use blockFrames = frames = 4096.
    double bpMax = 0.0; std::string bpNote = "clean";
    {
      const int n = 4096;
      EngineHarness hf, hb;
      if (!hf.load(st, sr, n)) bpNote = "frame-load-fail";
      else if (!hb.load(st, sr, n)) bpNote = "block-load-fail";
      else if (!hf.render(n, 0.0)) bpNote = "frame-render-fail";
      else if (!hb.renderBlock(n, [](std::size_t, double& in0, double& in1) { in0 = 0.0; in1 = 0.0; }))
        bpNote = "block-render-fail";
      else {
        for (int ch = 0; ch < 4; ++ch) {
          const auto& a = hf.out(ch);
          const auto& b = hb.out(ch);
          if (a.size() != b.size()) { bpNote = "len@" + std::to_string(ch); break; }
          for (std::size_t i = 0; i < a.size(); ++i) {
            const double d = std::fabs(a[i] - b[i]);
            if (d > bpMax) { bpMax = d; }
          }
        }
        if (bpMax > 1e-9) { bpNote = "mismatch"; }
      }
      if (bpMax > 1e-9) gCode |= 8;
    }

    using Clock = std::chrono::steady_clock;
    // (c) preparation time: encode -> decode -> applyDeviceState (the serialization roundtrip).
    const int prepReps = 20;
    std::vector<double> prepMs; prepMs.reserve(prepReps);
    for (int i = 0; i < prepReps; ++i) {
      EngineHarness h;
      auto t0 = Clock::now();
      bool pok = h.load(st, sr);
      auto t1 = Clock::now();
      if (!pok) continue;
      prepMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    double prepMedian = medianOf(prepMs);

    // (d) single-thread per-frame HARNESS render-loop cost over a large render (per-frame
    //      processBlock(1) + the harness's input-feed lambda + per-frame output push + the sampled
    //      hook). This is the WHOLE harness render path per sample, NOT a pure processBlock cost —
    //      that is (e). Multiple reps => median + variance; buffer prep is OUTSIDE the timed region
    //      (reserve() + an un-timed warmup), so no realloc contaminates the measurement.
    const std::size_t N = 1ULL << 20;             // 1,048,576 frames
    const int cbReps = 7;
    std::vector<double> cbNs; cbNs.reserve(cbReps);
    for (int i = 0; i < cbReps; ++i) {
      EngineHarness h;
      if (!h.load(st, sr)) continue;
      h.reserve(N + (1ULL << 16));                // capacity for warmup + full window: no realloc
      if (!h.render(4096, 0.0)) continue;          // warmup (un-timed, primes cache)
      auto t0 = Clock::now();
      bool r = h.render(static_cast<int>(N), 0.0);
      auto t1 = Clock::now();
      if (!r) continue;
      cbNs.push_back(std::chrono::duration<double>(t1 - t0).count() * 1e9 / static_cast<double>(N));
    }
    double cbMed = medianOf(cbNs);
    double cbStd = stdevOf(cbNs, cbMed);
    double cbMsg = (cbMed > 0) ? (1e9 / cbMed) / 1e6 : -1.0;   // Msamples/s

    // (e) real-block CPU cost: time ONLY engine.processBlock on a NORMAL block size, repeated, with
    //     every allocation, the input fill and the output insert OUTSIDE the timed region (BLOCK item
    //     ③ — the old single renderBlock(1M) wrapped six vector allocs + input generation + output
    //     insert in the timed loop, so it was NOT a pure processBlock cost). Buffers are caller-owned
    //     (processPure does not allocate / feed / capture), reused across calls; the input is a
    //     constant zero pre-filled once. Median + stdev over `blkLanes` independent lanes.
    double cbBlockNs = -1.0; double cbBlockStd = -1.0; long blockSamples = 0;
    {
      const int block = 512;                       // normal audio block
      const int nRep = 4000;                       // processBlock calls per lane
      std::vector<double> cin0(block, 0.0), cin1(block, 0.0);     // input pre-filled OUTSIDE timing
      std::vector<double> cout0(block), cout1(block), cout2(block), cout3(block);
      const double* in[kInCh] = {cin0.data(), cin1.data()};
      double* devOut[kOutCh] = {cout0.data(), cout1.data(), cout2.data(), cout3.data()};
      const int blkLanes = 7;
      std::vector<double> lanes; lanes.reserve(blkLanes);
      for (int i = 0; i < blkLanes; ++i) {
        EngineHarness hB;
        if (!hB.load(st, sr, block, kInCh, kOutCh)) continue;
        if (hB.processPure(in, devOut, block) != StandaloneAudioEngine::Status::Rendered) continue;  // warmup
        auto t0 = Clock::now();
        bool ok = true;
        for (int r = 0; r < nRep; ++r) {
          if (hB.processPure(in, devOut, block) != StandaloneAudioEngine::Status::Rendered) { ok = false; break; }
        }
        auto t1 = Clock::now();
        if (ok) {
          const double sec = std::chrono::duration<double>(t1 - t0).count();
          lanes.push_back(sec * 1e9 / static_cast<double>(nRep * block));
        }
      }
      if (!lanes.empty()) {
        cbBlockNs = medianOf(lanes);
        cbBlockStd = stdevOf(lanes, cbBlockNs);
        blockSamples = static_cast<long>(block);
      }
    }

    std::vector<std::string> crows;
    crows.push_back("id\tmachine\tos\tcompiler\tvalue\tunit\tsr_hz");
    crows.push_back("cpu_four_output\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(fourFail) + "\tfinite_block_failcount\t" + std::to_string(sr));
    crows.push_back("cpu_four_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(fourNote) + "\tnote\t" + std::to_string(sr));
    crows.push_back("cpu_blockpartition\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(bpMax) + "\tblock_partition_maxdiff\t" + std::to_string(sr));
    crows.push_back("cpu_blockpartition_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(bpNote) + "\tnote\t" + std::to_string(sr));
    crows.push_back("cpu_prep\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(prepMedian) + "\tms\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbMed) + "\tns_per_sample_harness_render\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop_var\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbStd) + "\tns_per_sample_stdev\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop_msps\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbMsg) + "\tMsamples_per_s\t" + std::to_string(sr));
    crows.push_back("cpu_block_ns\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbBlockNs) + "\tns_per_sample_pure_processblock\t" + std::to_string(sr));
    crows.push_back("cpu_block_var\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbBlockStd) + "\tns_per_sample_stdev\t" + std::to_string(sr));
    crows.push_back("cpu_block_size\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(blockSamples) + "\tsamples_per_block\t" + std::to_string(sr));
    std::string cpuTsv = out + "/gh19_cpu.tsv";
    if (!writeTsv(cpuTsv, crows)) {
      std::fprintf(stderr, "FATAL: could not write %s\n", cpuTsv.c_str());
      gCode |= 2;
    }
    std::fprintf(stderr,
        "gh19_alias_probe cpu: four_output_fail=%ld (%s), blockpart_max=%.2e (%s), "
        "prep_median_ms=%.3f, callback_median=%.2f ns/s (stdev %.2f), "
        "callback_block=%.2f ns/s @ %ld samples, machine=%s/%s, compiler=%s\n",
        fourFail, fourNote.c_str(), bpMax, bpNote.c_str(), prepMedian, cbMed, cbStd,
        cbBlockNs, blockSamples, machine.c_str(), sysrel.c_str(), compiler.c_str());
  }

  std::string tsv = out + "/gh19_scenarios.tsv";
  if (!writeTsv(tsv, rows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str());
    gCode |= 2;
  }

  // Classic-cell provenance evidence (ruling B), in its own file so gh19_scenarios.tsv stays a
  // signal-cell record. The analyzer's probe-contract gate fails closed if this file is absent.
  std::string fidTsv = out + "/gh19_fidelity.tsv";
  if (!writeTsv(fidTsv, frows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", fidTsv.c_str());
    gCode |= 2;
  }

  std::size_t produced = 0, blocked = 0;
  for (const auto& r : rows) {
    if (r.rfind("id\t", 0) == 0) continue;
    auto c1 = r.find('\t'); auto c2 = r.find('\t', c1 + 1);
    auto c3 = r.find('\t', c2 + 1);
    if (c2 != std::string::npos && c3 != std::string::npos) {
      std::string sig = r.substr(c2 + 1, c3 - c2 - 1);
      if (sig.empty() || sig == "-") ++produced; else ++blocked;
    }
  }
  std::fprintf(stderr, "gh19_alias_probe: %zu cells produced, %zu blocked, exit_code=%d.\n",
               produced, blocked, gCode);
  return gCode;   // non-zero if any required cell not produced / raw-write / four-output / block mismatch.
}
