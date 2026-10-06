// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Offline 64-frame engine timing; this is not a real-time audio-device dropout test.
// Build against either checkout with the same compiler/options:
//   c++ -std=c++17 -O2 -DNDEBUG -Icore/include -Ihost/include -Igenerated
//       tools/benchmark_effector_switch.cpp -o benchmark_effector_switch (one command)
// Usage: benchmark_effector_switch SAMPLE_RATE SECONDS switch|steady [OUTPUT.raw]
// Optional raw output: block-major, then WET L/R and DRY A/B, 64 native doubles each.
// Keep disk I/O disabled for timing; compare raw files in separate runs on the same machine.

#include <host/standalone_audio_engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kBlock = 64;

void report(const char* label, std::vector<double>& times, double budget) {
  if (times.empty()) return;
  double total = 0.0;
  std::size_t over = 0;
  for (double ms : times) { total += ms; over += ms > budget; }
  std::sort(times.begin(), times.end());
  std::printf("%s n=%zu mean=%.6f median=%.6f p99.9=%.6f max=%.6f over_budget=%zu\n",
              label, times.size(), total / static_cast<double>(times.size()), times[times.size() / 2],
              times[static_cast<std::size_t>(0.999 * static_cast<double>(times.size() - 1))],
              times.back(), over);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    std::fprintf(stderr, "usage: %s SAMPLE_RATE SECONDS switch|steady [OUTPUT.raw]\n", argv[0]);
    return 2;
  }
  const double sr = std::atof(argv[1]), seconds = std::atof(argv[2]);
  const bool switching = std::strcmp(argv[3], "switch") == 0;
  if (!std::isfinite(sr) || sr < 8000 || sr > 384000 ||
      !std::isfinite(seconds) || seconds < 1 || seconds > 600 ||
      (!switching && std::strcmp(argv[3], "steady") != 0)) return 2;
  FILE* raw = argc == 5 ? std::fopen(argv[4], "wb") : nullptr;
  if (argc == 5 && raw == nullptr) return 2;

  namespace core = lunar24::core;
  namespace host = lunar24::host;
  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  if (!engine->prepare(host::kLunarStartupSeed, sr, kBlock, 0, 4) ||
      !engine->postDroneKey(0, true) || !engine->postDroneKey(3, true) ||
      !engine->postParameter(core::ParameterId::effector_blend, 0.7)) return 1;
  double samples[4][kBlock]{};
  double* out[4] = {samples[0], samples[1], samples[2], samples[3]};
  auto render = [&] {
    return engine->processBlock(nullptr, out, 0, 4, kBlock) ==
           host::StandaloneAudioEngine::Status::Rendered;
  };
  // One second of untimed warmup; allocation, preparation and UI posts are not timed.
  for (int b = 0; b < static_cast<int>(sr / kBlock); ++b)
    if (!render()) return 1;
  const int blocks = static_cast<int>(seconds * sr / kBlock);
  const int interval = std::max(1, static_cast<int>(0.25 * sr / kBlock));
  // Reset happens after the 15 ms fade-out. Include its block and surrounding fade blocks.
  const int transitionBlocks = static_cast<int>(std::ceil(0.020 * sr / kBlock)) + 1;
  std::vector<double> all, transitions;
  all.reserve(blocks);
  transitions.reserve(blocks);
  int switches = 0;
  double energy = 0.0;
  for (int b = 0; b < blocks; ++b) {
    if (switching && b % interval == 0) {
      // Both sides change together, cycling all 39 programs with different L/R programs.
      const int program = (switches + 1) % 39;
      if (!engine->postEffectorProgram(0, static_cast<core::ProgramId>(program)) ||
          !engine->postEffectorProgram(1, static_cast<core::ProgramId>((program + 7) % 39))) return 1;
      ++switches;
    }
    const auto start = Clock::now();
    const bool ok = render();
    const auto end = Clock::now();
    if (!ok) return 1;
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    all.push_back(ms);
    if (switching && b % interval < transitionBlocks) transitions.push_back(ms);
    for (const auto& channel : samples) {
      for (double v : channel) {
        if (!std::isfinite(v)) return 1;
        energy += v * v;
      }
    }
    if (raw && std::fwrite(samples, sizeof(samples), 1, raw) != 1) return 1;
  }
  if (raw && std::fclose(raw) != 0) return 1;
  const double budget = 1000.0 * kBlock / sr;
  std::printf("sr=%.0f frames=%d seconds=%.3f switches=%d budget_ms=%.6f energy=%.17g raw=%s\n",
              sr, kBlock, seconds, switches, budget, energy, raw ? "yes" : "no");
  report("all_ms", all, budget);
  report("transition_ms", transitions, budget);
  return energy > 0.0 ? 0 : 1;
}
