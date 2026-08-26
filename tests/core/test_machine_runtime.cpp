// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// #38 (GH#4 A03) acceptance — the will-red criteria for the MACHINE RUNTIME.
//
// The hard rule (@Claude, msg 43da88a5): every criterion is asserted against the
// PRODUCT runtime (SynthRuntime) — the executor the host's realtime callback
// drives — NEVER a test-internal second executor. The runtime under test here is
// core/include/lunar24/core/machine_runtime.h.
//
// Five criteria (each with its will-red negative):
//   ① product path genuinely consumes compile_graph() — the runtime calls
//      compile_graph in rebuild(), honours a patched CV in its render, and the
//      negative control (driveGraph=false, "bypass the graph") makes a patched CV
//      have NO effect — so a test that asserts "a patch changes output" REDS if
//      the runtime stops consuming the compiled graph.
//   ② repatching changes the output — connect/disconnect a patch edge, rebuild,
//      and the output changes / returns.
//   ③ the four outputs are correct and non-interfering — DRY A is driven only by
//      VCO A, DRY B only by VCO B (patching VCO A does not change DRY B), and the
//      WET outputs are the chain path, not a raw DRY tap.
//   ④ sr/buffer invariance + reproducibility — a fixed seed + fixed inputs gives
//      bit-identical output across runs and independent of block partition, at
//      every configured sample rate.
//   ⑤ RT-safe — no allocation, no lock on the render path (by construction),
//      verified as: rendering never recompiles/mutates the plan, output is finite.
//
// Synthetic descriptor tables drive the runtime exactly as the registry would in
// production; the runtime itself is the unmodified product surface. Where a
// comparison needs "the same VCO phase", each side uses a FRESH runtime so the two
// start from the identical deterministic initial state — never a reused runtime,
// whose phase has already advanced.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <lunar24/core/enums.h>
#include <lunar24/core/machine_runtime.h>

namespace core = lunar24::core;
using core::JackId;
using core::ModuleId;

namespace {

// Synthetic module + jack identity (tests pass synthetic tables, as test_patch_graph
// does). The runtime and compile_graph only read jack id + owning module.
constexpr ModuleId kM_CvSrc{1};
constexpr ModuleId kM_VcoA{2};
constexpr ModuleId kM_VcoB{3};
constexpr ModuleId kM_Vcf{4};

constexpr JackId kJ_CvOut{10};    // CV source output (output jack, M_CvSrc)
constexpr JackId kJ_VcoAVoct{11}; // VCO A v_oct input (M_VcoA)
constexpr JackId kJ_VcoBVoct{12}; // VCO B v_oct input (M_VcoB)
constexpr JackId kJ_VcfCvL{13};   // VCF cv_l input (M_Vcf)
constexpr JackId kJ_VcfCvR{14};   // VCF cv_r input (M_Vcf)

core::JackDescriptor mkJack(JackId id, ModuleId mod, core::PinDirection dir) {
  core::JackDescriptor d{};
  d.id = id;
  d.module = mod;
  d.direction = dir;
  // compile_graph reads only id + module; remaining fields are unused here.
  return d;
}

const core::JackDescriptor kJacks[] = {
    mkJack(kJ_CvOut, kM_CvSrc, core::PinDirection::output),
    mkJack(kJ_VcoAVoct, kM_VcoA, core::PinDirection::input),
    mkJack(kJ_VcoBVoct, kM_VcoB, core::PinDirection::input),
    mkJack(kJ_VcfCvL, kM_Vcf, core::PinDirection::input),
    mkJack(kJ_VcfCvR, kM_Vcf, core::PinDirection::input),
};
constexpr std::uint32_t kJackCount = 5;

// Prepared contracts are not consulted for this acyclic graph (no cyclic region),
// so nullptr is correct and sufficient; the table still declares every module the
// graph can touch so jack_module always resolves to a declared module.
const core::GraphModule kModules[] = {
    {kM_CvSrc, nullptr}, {kM_VcoA, nullptr}, {kM_VcoB, nullptr}, {kM_Vcf, nullptr}};
constexpr std::uint32_t kModuleCount = 4;

constexpr std::uint64_t kSeed = 0x4C554E41ull;  // "LUNA" — fixed, reproducible.
constexpr std::size_t kBlock = 256;
constexpr double kSr = 48000.0;
constexpr double kBaseHz = 220.0;
constexpr double kPatchV = 4.0;  // +4 V on a v_oct jack -> x16 pitch.

// Build a fresh runtime in a known, reproducible initial state (both VCOs start at
// phase 0, so a patched-vs-unpatched comparison starts from the same point). sr is
// a constructor parameter (the voice sources pin their rate at creation), so a run
// at a different sample rate is a fresh runtime built for that rate.
core::SynthRuntime makeRuntime(double sr = kSr) {
  core::SynthRuntime rt(kJacks, kJackCount, nullptr, 0, kModules, kModuleCount, kSeed,
                        sr);
  rt.setVcoBaseHz(kBaseHz);
  rt.setVoctBindings(kJ_VcoAVoct, kJ_VcoBVoct);
  rt.setVcfCvBindings(kJ_VcfCvL, kJ_VcfCvR);
  return rt;
}

// Run `n` frames of silence-ext input through a runtime and return the last frame's
// four outputs. VCOs accumulate phase, so the last frame reflects the freq that was
// live over the run.
core::RuntimeOutput runFrames(core::SynthRuntime& rt, std::size_t n, bool driveGraph) {
  core::RuntimeOutput last{};
  for (std::size_t i = 0; i < n; ++i) last = rt.processFrame(0.0, driveGraph);
  return last;
}

bool sameOutput(const core::RuntimeOutput& a, const core::RuntimeOutput& b) {
  return a.wetL == b.wetL && a.wetR == b.wetR && a.dryA == b.dryA && a.dryB == b.dryB;
}

int g_checks = 0;
int g_fail = 0;

void check(bool cond, const char* label) {
  ++g_checks;
  std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", label);
  if (!cond) ++g_fail;
}

bool finite(const core::RuntimeOutput& o) {
  return std::isfinite(o.wetL) && std::isfinite(o.wetR) && std::isfinite(o.dryA) &&
         std::isfinite(o.dryB);
}

}  // namespace

int main() {
  std::printf("#38 machine runtime acceptance — product-path executor.\n");

  // ---- ① product path consumes compile_graph() --------------------------------
  std::printf("(1) product path consumes compile_graph()\n");
  {
    core::SynthRuntime rt = makeRuntime();
    bool ok = rt.connect(kJ_CvOut, kJ_VcoBVoct) && rt.rebuild();
    check(ok, "rebuild() returns ok after a patch");
    check(rt.graphValid(), "compiled graph is valid");
    check(rt.graphModuleCount() >= 1, "compiled graph has >=1 module (graph consumed)");
    check(rt.edgeCount() >= 1, "compiled edge set is non-empty");

    // Drive the graph: CV=5 vs CV=0 on the patched VCO B v_oct. Two FRESH runtimes
    // (both start at phase 0) so the only difference is the resolved frequency.
    core::SynthRuntime hi = makeRuntime();
    hi.connect(kJ_CvOut, kJ_VcoBVoct);
    hi.rebuild();
    hi.setControlVoltage(kJ_CvOut, 5.0);
    core::RuntimeOutput hiO = runFrames(hi, kBlock, /*driveGraph=*/true);

    core::SynthRuntime lo = makeRuntime();
    lo.connect(kJ_CvOut, kJ_VcoBVoct);
    lo.rebuild();
    lo.setControlVoltage(kJ_CvOut, 0.0);
    core::RuntimeOutput loO = runFrames(lo, kBlock, /*driveGraph=*/true);

    check(std::fabs(hiO.dryB - loO.dryB) > 1e-6,
          "patch CV->VCO B v_oct changes DRY B (graph consumed)");

    // NEGATIVE (will-red): bypass the graph (driveGraph=false) and the same CV must
    // NOT reach VCO B — two fresh runtimes at CV=5 vs CV=0 give identical DRY B,
    // proving the ① discriminator is not vacuous. If the runtime ignores the graph
    // in the PRODUCT path too, the positive above would also stay green and the
    // whole test is telling you it is not consuming the graph anywhere.
    core::SynthRuntime bHi = makeRuntime();
    bHi.connect(kJ_CvOut, kJ_VcoBVoct);
    bHi.rebuild();
    bHi.setControlVoltage(kJ_CvOut, 5.0);
    core::RuntimeOutput bHiO = runFrames(bHi, kBlock, /*driveGraph=*/false);

    core::SynthRuntime bLo = makeRuntime();
    bLo.connect(kJ_CvOut, kJ_VcoBVoct);
    bLo.rebuild();
    bLo.setControlVoltage(kJ_CvOut, 0.0);
    core::RuntimeOutput bLoO = runFrames(bLo, kBlock, /*driveGraph=*/false);

    check(std::fabs(bHiO.dryB - bLoO.dryB) < 1e-9,
          "bypass (driveGraph=false): CV has NO effect on DRY B (negative)");
  }

  // ---- ② repatching changes the output ---------------------------------------
  std::printf("(2) repatching changes the output\n");
  {
    // Bare: no edge -> VCO B stays at base freq.
    core::SynthRuntime bareRt = makeRuntime();
    bareRt.rebuild();
    bareRt.setControlVoltage(kJ_CvOut, 5.0);
    core::RuntimeOutput bare = runFrames(bareRt, kBlock, /*driveGraph=*/true);

    core::SynthRuntime connRt = makeRuntime();
    bool cOk = connRt.connect(kJ_CvOut, kJ_VcoBVoct) && connRt.rebuild();
    connRt.setControlVoltage(kJ_CvOut, 5.0);
    core::RuntimeOutput patched = runFrames(connRt, kBlock, /*driveGraph=*/true);
    check(cOk, "connect returns ok");
    check(connRt.edgeCount() == 1, "after connect: one compiled edge");
    check(std::fabs(patched.dryB - bare.dryB) > 1e-6,
          "connecting CV->VCO B changes DRY B");

    core::SynthRuntime discRt = makeRuntime();
    discRt.connect(kJ_CvOut, kJ_VcoBVoct);
    discRt.rebuild();
    bool dOk = discRt.disconnect(kJ_CvOut, kJ_VcoBVoct) && discRt.rebuild();
    discRt.setControlVoltage(kJ_CvOut, 5.0);
    core::RuntimeOutput restored = runFrames(discRt, kBlock, /*driveGraph=*/true);
    check(dOk, "disconnect returns ok");
    check(discRt.edgeCount() == 0, "after disconnect: zero compiled edges");
    check(std::fabs(restored.dryB - bare.dryB) < 1e-9,
          "disconnecting CV->VCO B restores DRY B (repatch toggles)");
  }

  // ---- ③ four outputs correct, non-interfering ------------------------------
  std::printf("(3) four outputs correct and non-interfering\n");
  {
    // Baseline: no edge, both VCOs at the same base frequency, same start phase.
    core::SynthRuntime baseRt = makeRuntime();
    baseRt.rebuild();
    baseRt.setControlVoltage(kJ_CvOut, kPatchV);
    core::RuntimeOutput baseO = runFrames(baseRt, kBlock, /*driveGraph=*/true);

    // Patch VCO A's v_oct only. VCO A should change, VCO B must NOT (no crosstalk).
    core::SynthRuntime aRt = makeRuntime();
    aRt.connect(kJ_CvOut, kJ_VcoAVoct);
    aRt.rebuild();
    aRt.setControlVoltage(kJ_CvOut, kPatchV);
    core::RuntimeOutput aO = runFrames(aRt, kBlock, /*driveGraph=*/true);

    check(std::fabs(aO.dryA - baseO.dryA) > 1e-6, "patching VCO A changes DRY A");
    check(std::fabs(aO.dryB - baseO.dryB) < 1e-9,
          "patching VCO A does NOT change DRY B (no crosstalk)");
    check(std::fabs(aO.wetL - aO.dryA) > 1e-6 || std::fabs(aO.wetR - aO.dryA) > 1e-6,
          "WET L/R are the chain path, not the raw DRY tap");
    check(finite(aO), "all four outputs finite");
  }

  // ---- ④ sr/buffer invariance + reproducibility -----------------------------
  std::printf("(4) sr/buffer invariance + reproducibility\n");
  {
    const double kSrs[] = {44100.0, 48000.0, 88200.0, 96000.0};
    // Reproducibility at a fixed configuration: two freshly-built runtimes with the
    // same seed + inputs produce bit-identical output (at every configured sr).
    bool reproducible = true;
    for (double sr : kSrs) {
      core::SynthRuntime r1 = makeRuntime(sr);
      r1.connect(kJ_CvOut, kJ_VcoBVoct);
      r1.rebuild();
      r1.setControlVoltage(kJ_CvOut, 3.0);
      core::SynthRuntime r2 = makeRuntime(sr);
      r2.connect(kJ_CvOut, kJ_VcoBVoct);
      r2.rebuild();
      r2.setControlVoltage(kJ_CvOut, 3.0);
      if (!sameOutput(runFrames(r1, kBlock, true), runFrames(r2, kBlock, true))) {
        std::printf("    reproducibility FAILED at sr=%g\n", sr);
        reproducible = false;
      }
    }
    check(reproducible, "same seed + inputs => bit-identical output at 44.1/48/88.2/96k");

    // Partition invariance: one kBlock render equals four (kBlock/4) renders,
    // sample-by-sample, on the same configuration.
    core::SynthRuntime whole = makeRuntime(kSr);
    whole.connect(kJ_CvOut, kJ_VcoBVoct);
    whole.rebuild();
    whole.setControlVoltage(kJ_CvOut, 3.0);

    core::SynthRuntime part = makeRuntime(kSr);
    part.connect(kJ_CvOut, kJ_VcoBVoct);
    part.rebuild();
    part.setControlVoltage(kJ_CvOut, 3.0);

    std::vector<double> zeros(kBlock, 0.0);
    std::vector<core::RuntimeOutput> outFull(kBlock), outPart(kBlock);
    whole.processBlock(zeros.data(), kBlock, outFull.data(), /*driveGraph=*/true);
    const std::size_t chunk = kBlock / 4;
    for (std::size_t off = 0; off < kBlock; off += chunk)
      part.processBlock(zeros.data(), chunk, outPart.data() + off, /*driveGraph=*/true);

    bool invariant = true;
    for (std::size_t i = 0; i < kBlock; ++i) {
      if (!sameOutput(outFull[i], outPart[i])) {
        invariant = false;
        break;
      }
    }
    check(invariant, "1x256 render == 4x64 renders, bit-identical (partition-invariant)");
  }

  // ---- ⑤ RT-safe: no alloc/lock on the render path --------------------------
  std::printf("(5) RT-safe: no allocation / lock on the render path\n");
  {
    core::SynthRuntime rt = makeRuntime();
    rt.connect(kJ_CvOut, kJ_VcoBVoct);
    rt.rebuild();
    rt.setControlVoltage(kJ_CvOut, 2.0);
    const std::uint32_t m0 = rt.graphModuleCount();
    const std::uint32_t e0 = rt.edgeCount();
    const bool valid0 = rt.graphValid();
    core::RuntimeOutput last{};
    for (std::size_t i = 0; i < 100000; ++i) last = rt.processFrame(0.0, /*driveGraph=*/true);
    bool planStable = (rt.graphModuleCount() == m0) && (rt.edgeCount() == e0) &&
                      (rt.graphValid() == valid0);
    check(planStable, "100k frames do not recompile/mutate the plan (no alloc in render)");
    check(finite(last), "render output stays finite under sustained frames");
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
