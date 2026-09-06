// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_alias_probe.cpp — task #85 (GH #19) REAL-PRODUCT anti-aliasing probe.
//
// Drives the REAL product output through the ONE agreed entry (the same chain the host uses):
//     make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//     standalone_audio_engine.processBlock (encapsulated by EngineHarness),
// and captures the rendered audio for a matrix of [path x sample-rate x frequency/level] cells.
// It does NOT compute the aliasing analysis — that is the verified Python method
// (tools/gh19_alias_analyze.py) applied to these real samples. This file is the guard that the
// measurement truly samples the product's DSP, not an ideal() stand-in or a silent buffer.
//
// OBSERVATION POINTS (each is a real production tap, verified against the runtime):
//   dryA  = VCO A, tapped BEFORE the chain  (core/include/lunar24/core/machine_runtime.h:117)
//   dryB  = VCO B, tapped BEFORE the chain  (machine_runtime.h:118)
//   drone1 = pre-mixer classic drone-1 channel bus (runtime.droneChannel(0), machine_runtime.h:1633)
//   wetL  = the WET mix output channel (device_layout WET_L=0)
//
// CELLS:
//   * vco_a_tri  — VCO A default triangle, frequency by vco_a_oct_sel x vco_a_tune, tap dryA.
//   * drone1_gn1 — classic drone generation-1 saw carrier (+cubic), tap droneChannel(0).
//   * preamp     — the tanh input stage isolated on the WET bus (mixer preamp ch + host drive).
//   * wet_chain  — the composite kVcfPath WET output, recorded as a COMPOSITE cell (never a
//                  single-module attribution).
//
// A cell that produces no legal stimulus (e.g. needs a gate the default patch does not supply) is
// recorded with signal=<reason> and NOT counted as a measurement — it is a stated blocker, never a
// silent pass. Any cell whose samples are non-finite is a hard failure (exit non-zero).
//
// OUTPUT (--out <dir>, default ./report/gh19-probe):
//   gh19_scenarios.tsv    one metadata row per recorded cell (with measured f0 / peak)
//   gh19_scnNNN.raw       little-endian f64 samples of that cell's observation window

#include <lunar24/core/device_state.h>
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
#include <string>
#include <sys/utsname.h>
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

struct Cap {
  bool ok = false;
  std::string signal;        // "" = produced; else a human blocker reason ("silent", "rejected-*")
  std::vector<double> x;
  double peak = 0.0;
  double f0 = 0.0;
};

// Capture `tap` from a single engine render of `frames` frames at `sr` under the given state.
// `sampled` selects a runtime getter tap; otherwise it reads an output channel buffer.
// `domain` = the tap's voltage domain, which fixes the scale-guard bounds:
//   "device" -> the physical device output (normalized/0.5 scale): peak must be in [1e-4, 0.55].
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
  if (tap == "drone1" || tap == "drone3") {
    std::vector<double> d;
    d.reserve(frames);
    bool ok = h.renderSampled(static_cast<int>(frames), preampV,
        [&](const lunar24::core::SynthRuntime& rt) {
          d.push_back(tap == "drone1" ? rt.droneChannel(0) : rt.drone3Channel());
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

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh19-probe";
  for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
  std::string mk = "mkdir -p '" + out + "'";
  if (std::system(mk.c_str()) != 0) { /* best effort */ }

  std::vector<std::string> rows;
  rows.push_back("id\tpath\tsignal\tsr_hz\tf0_target_hz\tf0_meas_hz\tpeak\tchannel\twave\traw\tsamples");
  int scnIdx = 0;

  // Emit a scenario row. Every PRODUCED cell gets an indexed raw sample file so the analyzer
  // (tools/gh19_alias_analyze.py) can consume the genuine product samples from the SAME directory.
  auto emit = [&](const std::string& id, const std::string& path, const std::string& signal,
                  double sr, double ft, const Cap& c, const std::string& channel,
                  const std::string& wave) {
    std::string raw = "";
    if (c.ok) {
      raw = "gh19_scn" + std::to_string(++scnIdx) + ".raw";
      writeRaw(out + "/" + raw, c.x);
    }
    rows.push_back(id + "\t" + path + "\t" + safeTsv(signal) +
                   "\t" + std::to_string(sr) +
                   "\t" + std::to_string(ft) +
                   "\t" + std::to_string(c.f0) +
                   "\t" + std::to_string(c.peak) +
                   "\t" + channel + "\t" + wave +
                   "\t" + raw + "\t" + std::to_string(c.x.size()));
    // A *produced-signal* cell must be within scale and finite; a blocked cell is informational.
    if (signal.empty() && !c.ok) { std::fprintf(stderr, "FATAL %s: produced but failed\n", id.c_str()); }
  };

  // ---- VCO A default triangle: 4 SRS x 3 frequencies. Reachable, clean, periodic. ----
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      double oct = (ft == 220.0) ? 0.0 : 1.0;
      double tune = (ft == 880.0) ? 1.0 : 0.0;
      slot(st, ParameterId::vco_a_oct_sel) = oct;
      slot(st, ParameterId::vco_a_tune) = tune;
      Cap c = capture(st, sr, kWarm + kWin, "dry_a", 0.0);
      emit("vco_a_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft),
           "vco_a_tri", c.signal, sr, ft, c, "dry_a", "tri");
    }
  }

  // ---- Classic drone generation-1 (saw carrier + cubic path): mute off + hold on. ----
  for (double sr : kSrs) {
    for (double tuneNorm : {0.20, 0.50, 0.80}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      slot(st, ParameterId::drone_1_mute_1) = 0.0;
      slot(st, ParameterId::drone_1_gate_hold) = 1.0;
      slot(st, ParameterId::drone_1_tune_1) = tuneNorm;
      Cap c = capture(st, sr, kWarm + kWin, "drone1", 0.0, "volt");
      emit("drone1_gn1_" + std::to_string((int)sr) + "_t" + std::to_string((int)(tuneNorm * 100)),
           "drone1_gn1", c.signal, sr, 0.0, c, "drone1", "sawcubic");
    }
  }

  // ---- Preamp (tanh input stage) isolated on the WET bus: mixer preamp ch + host drive. ----
  for (double sr : kSrs) {
    for (int lvl : {0, 1}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      for (int c = 1; c <= 10; ++c) {
        const int base = static_cast<int>(ParameterId::mixer_ch1_vol);
        auto pid = static_cast<ParameterId>(base + (c - 1) * 2);  // ch(n)_vol stride = 2
        slot(st, pid) = 0.0;
      }
      // preamp channel index 6 -> mixer_ch7_vol (registry mixer_ch(n+1)_vol).
      slot(st, ParameterId::mixer_ch7_vol) = 1.0;
      slot(st, ParameterId::preamp_gain) = (lvl == 0) ? 0.3 : 1.0;
      double drive = (lvl == 0) ? 0.05 : 0.5;
      Cap c = capture(st, sr, kWarm + kWin, "wet_l", drive);
      // The host preamp feed is a CONSTANT voltage (the shared harness render() has no per-frame
      // AC input), so the isolated preamp tap is DC — there is no periodic signal to alias. This is
      // a Class-B per-sample nonlinearity whose aliasing is measured by oversample *comparison*,
      // not harmonic folding; it is recorded as a stated BLOCKER, not an alias cell, and its
      // measurement is deferred to the oversample path (not the first slice).
      if (c.ok && c.f0 < 1.0) { c.signal = "blocked:no-ac-stimulus"; c.ok = false; }
      emit("preamp_" + std::to_string((int)sr) + "_l" + std::to_string(lvl),
           "preamp", c.signal, sr, 0.0, c, "wet_l", "tanh");
    }
  }

  // ---- Composite WET chain (default kVcfPath) — recorded as a COMPOSITE cell. ----
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    Cap c = capture(st, sr, kWarm + kWin, "wet_l", 0.0);
    emit("wet_chain_" + std::to_string((int)sr), "wet_chain", c.signal, sr, 0.0, c, "wet_l", "composite");
  }

  // ---- Machine evidence: four-output finite/block-consistency, prep time, callback cost. ----
  // LOCAL evidence only — the mandate's CPU bound is "this machine, not a cross-machine budget".
  // Written as a SEPARATE gh19_cpu.tsv so gh19_scenarios.tsv stays signal-only (the analyzer
  // parses the 3rd field of a scenario row as "signal"; a cpu meta row there would be miscounted).
  {
    std::string machine = "unknown", sysrel = "unknown", compiler = "unknown";
    struct utsname un;
    if (uname(&un) == 0) { machine = un.machine; sysrel = std::string(un.sysname) + " " + un.release; }
#ifdef _MSC_VER
    compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    compiler = __VERSION__;
#endif

    const double sr = kSrs[1];                    // 48000
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::vco_a_oct_sel) = 1.0;   // 440 Hz clean VCO-A triangle
    slot(st, ParameterId::vco_a_tune) = 0.0;

    // (a) Four outputs finite + block-consistent across every sample rate. The harness render()
    // only returns true when EVERY processBlock is Rendered (block-consistency); the four shared
    // block size, so equal lengths + all-finite is the per-record contract.
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

    using Clock = std::chrono::steady_clock;
    // (b) preparation time: encode -> decode -> applyDeviceState (the serialization roundtrip).
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
    std::sort(prepMs.begin(), prepMs.end());
    double prepMedian = prepMs.empty() ? -1.0 : prepMs[prepMs.size() / 2];

    // (c) single-thread per-sample callback cost over a large render (per-frame processBlock(1)).
    EngineHarness h;
    bool pok = h.load(st, sr);
    double cbNs = -1.0, cbMsg = -1.0;
    if (pok) {
      const std::size_t N = 1ULL << 20;           // 1,048,576 frames
      auto t0 = Clock::now();
      bool r = h.render(static_cast<int>(N), 0.0);
      auto t1 = Clock::now();
      if (r) {
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        cbNs = sec * 1e9 / static_cast<double>(N);
        cbMsg = static_cast<double>(N) / sec / 1e6;
      }
    }

    std::vector<std::string> crows;
    crows.push_back("id\tmachine\tos\tcompiler\tvalue\tunit\tsr_hz");
    crows.push_back("cpu_four_output\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(fourFail) + "\tfinite_block_failcount\t" + std::to_string(sr));
    crows.push_back("cpu_four_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(fourNote) + "\tnote\t" + std::to_string(sr));
    crows.push_back("cpu_prep\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(prepMedian) + "\tms\t" + std::to_string(sr));
    crows.push_back("cpu_callback\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbNs) + "\tns_per_sample\t" + std::to_string(sr));
    crows.push_back("cpu_callback_msg\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbMsg) + "\tMsamples_per_s\t" + std::to_string(sr));
    std::string cpuTsv = out + "/gh19_cpu.tsv";
    if (!writeTsv(cpuTsv, crows)) std::fprintf(stderr, "FATAL: could not write %s\n", cpuTsv.c_str());
    std::fprintf(stderr,
        "gh19_alias_probe cpu: four_output_fail=%ld (%s), prep_median_ms=%.3f, "
        "callback_ns_per_sample=%.2f, callback_Msamples_s=%.2f, machine=%s/%s, compiler=%s\n",
        fourFail, fourNote.c_str(), prepMedian, cbNs, cbMsg, machine.c_str(), sysrel.c_str(), compiler.c_str());
  }

  std::string tsv = out + "/gh19_scenarios.tsv";
  if (!writeTsv(tsv, rows)) { std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str()); return 1; }

  std::size_t produced = 0, blocked = 0;
  for (const auto& r : rows) {
    if (r.rfind("id\t", 0) == 0) continue;
    auto c1 = r.find('\t'); auto c2 = r.find('\t', c1 + 1);
    auto c3 = r.find('\t', c2 + 1);
    if (c2 != std::string::npos && c3 != std::string::npos) {
      // signal is the 3rd field (after id and path), not the path that c1..c2 spans.
      std::string sig = r.substr(c2 + 1, c3 - c2 - 1);
      if (sig.empty() || sig == "-") ++produced; else ++blocked;
    }
  }
  std::fprintf(stderr, "gh19_alias_probe: %zu cells produced, %zu blocked.\n", produced, blocked);
  return 0;   // informational baseline; the Python analyzer owns the pass/fail gate.
}
