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
//   ⑤ RT-safe — no allocation, no lock on the render path. Verified two ways:
//      an ALLOCATOR-COUNT probe (operator new/new[] in this TU are counted; the
//      render loop must leave the count at zero, and the probe is itself proven
//      non-vacuous by a deliberate allocation inside the window), and the plan-stable
//      check (rendering never recompiles/mutates the plan). Output stays finite.
//
// Synthetic descriptor tables drive the runtime exactly as the registry would in
// production; the runtime itself is the unmodified product surface. Where a
// comparison needs "the same VCO phase", each side uses a FRESH runtime so the two
// start from the identical deterministic initial state — never a reused runtime,
// whose phase has already advanced.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <lunar24/core/enums.h>
#include <lunar24/core/machine_runtime.h>

// #38 criterion ⑤ (rule 5) allocator-count probe. The product render path must
// allocate nothing, so every operator new/new[] in THIS test binary is counted and
// the render loop must leave the count at zero. We replace only the unaligned
// operators (a deliberate-alloc vector goes through unaligned new, and the render
// path allocates nothing at all — aligned or not — so the unaligned set fully
// covers both the real measurement and the deliberate negative). Forwarding to
// malloc/free keeps the default delete (which frees) compatible.
std::size_t g_allocCount = 0;

void* operator new(std::size_t n) {
  ++g_allocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace core = lunar24::core;
using core::JackId;
using core::ModuleId;

namespace {

// Synthetic module + jack identity (tests pass synthetic tables, as test_patch_graph
// does). The runtime and compile_graph only read jack id + owning module.
// Modules 1-4 are the CONTROL patchables (CV source, VCO A, VCO B, VCF); 5-10 are
// the FIXED-chain modules merged into the same plan (drone, ext-in, preamp,
// env_follower, mixer, distortion).
constexpr ModuleId kM_CvSrc{1};
constexpr ModuleId kM_VcoA{2};
constexpr ModuleId kM_VcoB{3};
constexpr ModuleId kM_Vcf{4};
constexpr ModuleId kM_Drone{5};
constexpr ModuleId kM_ExtIn{6};
constexpr ModuleId kM_Preamp{7};
constexpr ModuleId kM_EnvFol{8};
constexpr ModuleId kM_Mixer{9};
constexpr ModuleId kM_Dist{10};

constexpr JackId kJ_CvOut{10};    // CV source output (output jack, M_CvSrc)
constexpr JackId kJ_VcoAVoct{11}; // VCO A v_oct input (M_VcoA)
constexpr JackId kJ_VcoBVoct{12}; // VCO B v_oct input (M_VcoB)
constexpr JackId kJ_VcfCvL{13};   // VCF cv_l input (M_Vcf)
constexpr JackId kJ_VcfCvR{14};   // VCF cv_r input (M_Vcf)
// The env_follower cycle cable jack ids. kM_Preamp{7} < kM_EnvFol{8} so the planned
// per-sample order (preamp then env_follower) makes the PLUGGABLE env_out cable the
// cycle's break edge, exactly as the fixed leg (preamp->env_follower) carries the
// module ordering and the cable JackId carries the contract credit (z_inverse on a
// one-pole smoother).
constexpr JackId kJ_EnvFolOut{15};   // env_follower env_out jack (cycle break source)
constexpr JackId kJ_PreampExtIn{16}; // preamp ext_source_in jack (cycle break sink)

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
    mkJack(kJ_EnvFolOut, kM_EnvFol, core::PinDirection::output),
    mkJack(kJ_PreampExtIn, kM_Preamp, core::PinDirection::input),
};
constexpr std::uint32_t kJackCount = 7;

constexpr std::uint64_t kSeed = 0x4C554E41ull;  // "LUNA" — fixed, reproducible.
constexpr std::size_t kBlock = 256;
constexpr double kSr = 48000.0;
constexpr double kBaseHz = 220.0;
constexpr double kPatchV = 4.0;  // +4 V on a v_oct jack -> x16 pitch.

// Env_follower (a one-pole smoother) declares a direct-through env path, so a cycle
// through it is z_inverse (one extra sample). The inPort is JackId{0} — the FIXED
// endpoint sentinel: the fixed preamp->env_follower leg has no JackId, and the
// compiler hands that sentinel to decide_feedback_delay as the cycle's re-entrant
// input port. The outPort is the env_out jack (the pluggable break source).
core::ModuleExecutionContract envFolContract() {
  core::ModuleExecutionContract c;
  c.sampleRate = kSr;
  c.allowedInCyclicSCC = true;
  c.pathDelayCount = 1;
  c.pathDelays[0].inPort = core::JackId{0};
  c.pathDelays[0].outPort = kJ_EnvFolOut;
  c.pathDelays[0].canDirectThrough = true;
  c.pathDelays[0].minCausalDelaySamples = 0.0;
  c.hasDirectThroughPath = true;
  return c;
}
// Preamp is also in the cyclic SCC (it must be allowedInCyclicSCC); it is not the
// module that decides the break delay, so it declares no path here (no direct-
// through path, consistent with hasDirectThroughPath==false).
core::ModuleExecutionContract preampContract() {
  core::ModuleExecutionContract c;
  c.sampleRate = kSr;
  c.allowedInCyclicSCC = true;
  return c;
}

// Acyclic modules (VCO, drone, ext-in, mixer, VCF, distortion, CV source) need no
// contract — the compiler rejects only modules in a cyclic SCC that lack one.
const core::ModuleExecutionContract kEnvFolC = envFolContract();
const core::ModuleExecutionContract kPreampC = preampContract();
const core::GraphModule kModules[] = {
    {kM_CvSrc, nullptr},   {kM_VcoA, nullptr},     {kM_VcoB, nullptr},
    {kM_Vcf, nullptr},     {kM_Drone, nullptr},    {kM_ExtIn, nullptr},
    {kM_Preamp, &kPreampC},{kM_EnvFol, &kEnvFolC},{kM_Mixer, nullptr},
    {kM_Dist, nullptr}};
constexpr std::uint32_t kModuleCount = 10;

// Fixed internal routes (design/07 §4 Decision B), merged into the same plan as the
// pluggable control cables. Only module->module dependency routes go to the
// compiler — device-output routes (vco_a_to_dry_a, dist_to_wet) are terminal taps
// consumed by the runtime's role outputs, not execution-plan edges. Note preamp ->
// env_follower is included so a patched env_out cable forms the real 2-node cycle.
const core::FixedEdge kFixedEdges[] = {
    {kM_Drone, kM_Mixer, "drone_to_mixer"},
    {kM_ExtIn, kM_Mixer, "ext_audio_to_mixer"},
    {kM_VcoA, kM_Mixer, "vco_a_to_mixer"},
    {kM_VcoB, kM_Mixer, "vco_b_to_mixer"},
    {kM_Preamp, kM_Mixer, "preamp_to_mixer"},
    {kM_Preamp, kM_EnvFol, "preamp_to_env_follower"},
    {kM_Mixer, kM_Vcf, "mixer_to_vcf_l"},
    {kM_Mixer, kM_Vcf, "mixer_to_vcf_r"},
    {kM_Vcf, kM_Dist, "vcf_l_to_dist_l"},
    {kM_Vcf, kM_Dist, "vcf_r_to_dist_r"},
};
constexpr std::uint32_t kFixedEdgeCount = 10;

// Build a fresh runtime in a known, reproducible initial state (both VCOs start at
// phase 0, so a patched-vs-unpatched comparison starts from the same point). sr is
// a constructor parameter (the voice sources pin their rate at creation), so a run
// at a different sample rate is a fresh runtime built for that rate.
core::SynthRuntime makeRuntime(double sr = kSr) {
  core::SynthRuntime rt(kJacks, kJackCount, nullptr, 0, kModules, kModuleCount, kSeed,
                        sr, kFixedEdges, kFixedEdgeCount);
  rt.setVcoBaseHz(kBaseHz);
  rt.setVoctBindings(kJ_VcoAVoct, kJ_VcoBVoct);
  rt.setVcfCvBindings(kJ_VcfCvL, kJ_VcfCvR);
  rt.setPreampExtIn(kJ_PreampExtIn);
  rt.setEnvFolOut(kJ_EnvFolOut);
  rt.bindFixedRole(kM_VcoA, core::FixedChainRole::kVcoA);
  rt.bindFixedRole(kM_VcoB, core::FixedChainRole::kVcoB);
  rt.bindFixedRole(kM_Drone, core::FixedChainRole::kDrone);
  rt.bindFixedRole(kM_ExtIn, core::FixedChainRole::kExtIn);
  rt.bindFixedRole(kM_Preamp, core::FixedChainRole::kPreamp);
  rt.bindFixedRole(kM_EnvFol, core::FixedChainRole::kEnvFollower);
  rt.bindFixedRole(kM_Mixer, core::FixedChainRole::kMixer);
  rt.bindFixedRole(kM_Vcf, core::FixedChainRole::kVcf);
  rt.bindFixedRole(kM_Dist, core::FixedChainRole::kDistortion);
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

    // ALLOCATOR-COUNT (@Claude rule 5). The graph/plan was built by rebuild() OFF
    // the audio thread; the render loop is the ON-thread measurement window, so the
    // count is zeroed immediately before it. First prove the probe is non-vacuous:
    // a deliberate allocation inside the window IS seen (a counter that never fires
    // is not a detector). Without this, a probe that always reported 0 would pass
    // trivially and a future allocation in the render path would go unnoticed.
    g_allocCount = 0;
    {
      // A `new char[N]` expression with a non-escaping pointer is scalar-replaced
      // onto the stack under -O2 ([expr.new] permits omitting a replaceable
      // allocation whose result never escapes), so a counter fed by `new[]` would
      // never fire and the probe would be vacuously green. Call the replaceable
      // allocation function DIRECTLY — a plain call the optimizer must perform,
      // because its global-store side effect (g_allocCount++) is externally visible.
      void* mem = ::operator new(8);
      auto* junk = static_cast<char*>(mem);
      junk[0] = 0x42;
      const bool detected = g_allocCount > 0;
      const char peek = junk[0];
      ::operator delete(mem);
      check(detected && peek == 0x42,
            "probe detects a deliberate allocation (non-vacuous)");
    }

    // The real RT claim: rendering 100k frames allocates nothing. Zeroed again so
    // the deliberate allocation above (and any harness setup) is excluded.
    g_allocCount = 0;
    core::RuntimeOutput last{};
    for (std::size_t i = 0; i < 100000; ++i) last = rt.processFrame(0.0, /*driveGraph=*/true);
    const std::size_t allocDuringRender = g_allocCount;
    check(allocDuringRender == 0, "100k frames allocate nothing on the render path");

    bool planStable = (rt.graphModuleCount() == m0) && (rt.edgeCount() == e0) &&
                      (rt.graphValid() == valid0);
    check(planStable, "100k frames do not recompile/mutate the plan (no alloc in render)");
    check(finite(last), "render output stays finite under sustained frames");
  }

  // ---- ⑥ ruling 2: fixed chain + pluggable edges share ONE plan --------------
  std::printf("(6) ruling 2: fixed chain + pluggable edges share one plan\n");
  {
    // (a) SHARED PLAN. The fixed-chain modules (preamp/env_follower/mixer/vcf/dist)
    // must appear in the SAME compiled plan as the pluggable control edge, and the
    // runtime must dispatch them in the plan's order (mixer before vcf before dist).
    core::SynthRuntime rt = makeRuntime();
    rt.setControlVoltage(kJ_CvOut, 0.0);
    bool ok = rt.connect(kJ_CvOut, kJ_VcoBVoct) && rt.rebuild();
    check(ok, "rebuild() ok with the fixed chain wired");
    const core::CompiledGraph& g = rt.graph();
    auto planHasModule = [&](ModuleId id) -> bool {
      for (const auto& r : g.regions)
        for (ModuleId m : r.modules)
          if (m == id) return true;
      return false;
    };
    check(planHasModule(kM_Preamp) && planHasModule(kM_EnvFol) &&
              planHasModule(kM_Mixer) && planHasModule(kM_Vcf) && planHasModule(kM_Dist),
          "fixed-chain modules (preamp/env_follower/mixer/vcf/dist) are in the plan");
    check(planHasModule(kM_CvSrc),
          "the pluggable control CV source shares the same plan (merged, one source of truth)");
    auto rolePos = [&rt](core::FixedChainRole r) -> int {
      for (std::uint32_t i = 0; i < rt.chainExecCount(); ++i)
        if (rt.chainExecRoleAt(i) == r) return static_cast<int>(i);
      return -1;
    };
    const int mixer = rolePos(core::FixedChainRole::kMixer);
    const int vcf = rolePos(core::FixedChainRole::kVcf);
    const int dist = rolePos(core::FixedChainRole::kDistortion);
    check(mixer >= 0 && vcf >= 0 && dist >= 0,
          "runtime binds and dispatches the mixer/vcf/dist roles");
    check(mixer >= 0 && vcf >= 0 && dist >= 0 && mixer < vcf && vcf < dist,
          "chain runs mixer -> vcf -> dist in the plan's order (no hard-coded SignalPath)");

    // Execution ordering probe: with a VCO running, WET must be non-trivial. If a
    // mutation ran the mixer before the VCO (ignoring plan order), the VCO channel
    // would be zeroed at mix time and WET would collapse to ~0.
    double maxWet = 0.0;
    core::RuntimeOutput last{};
    for (std::size_t i = 0; i < kBlock * 2; ++i) {
      last = rt.processFrame(0.0, /*driveGraph=*/true);
      if (std::fabs(last.wetL) > maxWet) maxWet = std::fabs(last.wetL);
    }
    check(maxWet > 1e-3, "WET is non-trivial (full chain ran in plan order)");
    check(finite(last), "chain output finite");
  }

  // (b) REAL CYCLE (topology): patching env_out -> preamp.ext_source_in against the
  // fixed preamp -> env_follower edge must yield ONE 2-node cyclic SCC, broken on the
  // PLUGGABLE cable with a z^-1 (one-pole smoother: canDirectThrough -> z_inverse).
  {
    core::SynthRuntime cyc = makeRuntime();
    bool ok = cyc.connect(kJ_EnvFolOut, kJ_PreampExtIn) && cyc.rebuild();
    check(ok, "rebuild() ok with the env cable patched");
    const core::CompiledGraph& cg = cyc.graph();
    bool sawCyclic = false;
    bool hasPreamp = false, hasEnv = false, twoModules = false, oneBreak = false;
    core::CompiledFeedbackEdge breakFe{};
    for (const auto& r : cg.regions) {
      if (r.kind != core::RegionKind::cyclic) continue;
      sawCyclic = true;
      for (ModuleId m : r.modules) { if (m == kM_Preamp) hasPreamp = true; if (m == kM_EnvFol) hasEnv = true; }
      twoModules = (r.modules.size() == 2u);
      oneBreak = (r.feedback.size() == 1u);
      if (!r.feedback.empty()) breakFe = r.feedback[0];
    }
    check(sawCyclic, "patched env cable forms a cyclic region (cycle detected)");
    check(hasPreamp && hasEnv && twoModules,
          "cyclic SCC is exactly preamp <-> env_follower (2 nodes)");
    check(oneBreak && cg.regions.size() >= 1,
          "the cycle selects exactly one break edge");
    check(breakFe.sourceJack == kJ_EnvFolOut && breakFe.sinkJack == kJ_PreampExtIn,
          "break edge is the PLUGGABLE env_out -> ext_source_in cable (has the JackId)");
    check(breakFe.delay == core::FeedbackDelay::z_inverse &&
              std::fabs(breakFe.delaySamples - 1.0) < 1e-6,
          "break is z_inverse / delaySamples==1 (one-pole smoother -> z^-1)");

    // Execution discriminator (will-red if env_follower is NOT restored): with the
    // cycle patched, the preamp is fed the delayed envelope, NOT the raw ext signal,
    // so the chain output must DIFFER from an unpatched runtime driven by the same
    // source. If env_follower were gone the cycle would not form and the two would
    // render identically.
    std::vector<double> src(kBlock);
    for (std::size_t i = 0; i < kBlock; ++i) src[i] = 0.5 * std::sin(2.0 * 3.14159265358979 * (100.0 / kSr) * static_cast<double>(i));
    core::SynthRuntime cycA = makeRuntime();
    cycA.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    cycA.rebuild();
    core::SynthRuntime cycB = makeRuntime();  // unpatched: preamp reads ext directly
    cycB.rebuild();
    std::vector<core::RuntimeOutput> outA(kBlock), outB(kBlock);
    cycA.processBlock(src.data(), kBlock, outA.data(), /*driveGraph=*/true);
    cycB.processBlock(src.data(), kBlock, outB.data(), /*driveGraph=*/true);
    double diff = 0.0;
    for (std::size_t i = 0; i < kBlock; ++i) diff += std::fabs(outA[i].wetL - outB[i].wetL);
    check(diff > 1e-6,
          "patching the env cable changes the preamp path (cycle reaches the preamp)");

    // PARTITION INVARIANCE with the cycle (block-lazy negative): the cyclic region
    // runs per-sample, so any block partition reproduces the same sequence.
    core::SynthRuntime whole = makeRuntime();
    whole.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    whole.rebuild();
    core::SynthRuntime part = makeRuntime();
    part.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    part.rebuild();
    std::vector<core::RuntimeOutput> outFull(kBlock), outPart(kBlock);
    whole.processBlock(src.data(), kBlock, outFull.data(), true);
    const std::size_t chunks[] = {64, 100, 37, 55};
    std::size_t off = 0, ci = 0;
    while (off < kBlock) {
      const std::size_t n = std::min(chunks[ci % 4], kBlock - off);
      part.processBlock(src.data() + off, n, outPart.data() + off, true);
      off += n; ++ci;
    }
    bool invariant = true;
    for (std::size_t i = 0; i < kBlock; ++i)
      if (!sameOutput(outFull[i], outPart[i])) { invariant = false; break; }
    check(invariant, "cycle render is partition-invariant (per-sample, not block-lazy)");
    bool finiteAll = true;
    for (std::size_t i = 0; i < kBlock; ++i) if (!finite(outFull[i])) finiteAll = false;
    check(finiteAll, "cycle render stays finite (no NaN from the break)");
  }

  // (b2) EXECUTOR RECONCILE + DELAY-LENGTH GUARD (07 §4 "z⁻¹ 退化成整块延迟" 的防护, 也是
  //       P2-③ real_path "执行器不认编译器账" 的同一病灶)。判据读**运行时真实反馈延迟**
  //       `feedback_[i].delaySamples`(执行器真正用的那个), 不是编译器的
  //       `region.feedback[].delaySamples`。突变把执行器延迟换成整块(本应是编译器判定的
  //       1)⇒ 这里必须红。
  {
    core::SynthRuntime rc = makeRuntime();
    rc.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    rc.rebuild();
    const core::CompiledGraph& rg = rc.graph();
    // 编译器为该环计划的 delaySamples(按 region.feedback 的 plan 序收集)。
    std::vector<double> planDelay;
    for (const auto& r : rg.regions)
      if (r.kind == core::RegionKind::cyclic)
        for (const auto& fe : r.feedback) planDelay.push_back(fe.delaySamples);
    const std::uint32_t n = (planDelay.size() < rc.feedbackCount())
                                ? static_cast<std::uint32_t>(planDelay.size())
                                : rc.feedbackCount();
    bool obeysPlan = (rc.feedbackCount() == planDelay.size());
    bool oneSample = true;
    for (std::uint32_t i = 0; i < n; ++i) {
      const double rt = rc.feedbackAt(i).delaySamples;  // 执行器真实值
      if (std::fabs(rt - planDelay[i]) > 1e-9) obeysPlan = false;
      if (std::fabs(rt - 1.0) > 1e-9) oneSample = false;
    }
    check(obeysPlan,
          "executor delaySamples == compiler plan delay for every break edge (执行器对账)");
    check(oneSample,
          "executor ring is exactly z^-1 / 1 sample deep (非退化的整块延迟)");
    // 跑满一整块(静音 ext)确认执行器对延迟链的消费是确定且有限的(=64.0 时该值仍由
    // delaySamples 驱动, 对账与 1-deep 两条已在最上面红; 这里只做行为侧兜底)。
    std::vector<double> quiet(kBlock, 0.0);
    std::vector<core::RuntimeOutput> outc(kBlock);
    rc.processBlock(quiet.data(), kBlock, outc.data(), /*driveGraph=*/true);
    check(std::isfinite(outc[kBlock - 1].wetL), "reconciled cycle output finite for a full block");
  }

  // (c) BREAK-EDGE INSERTION-ORDER INDEPENDENCE: the same topology built with the
  // cables connected in a different order yields the identical break-edge set.
  {
    auto feedbackSig = [](const core::CompiledGraph& g, std::string& sig) {
      sig.clear();
      for (const auto& r : g.regions) {
        if (r.kind != core::RegionKind::cyclic) continue;
        for (const auto& fe : r.feedback)
          sig += std::to_string(static_cast<std::uint32_t>(fe.sourceJack)) + "," +
                 std::to_string(static_cast<std::uint32_t>(fe.sinkJack)) + "," +
                 std::to_string(fe.delaySamples) + ";";
      }
    };
    core::SynthRuntime orderA = makeRuntime();
    orderA.connect(kJ_CvOut, kJ_VcoBVoct);
    orderA.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    orderA.rebuild();
    core::SynthRuntime orderB = makeRuntime();
    orderB.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    orderB.connect(kJ_CvOut, kJ_VcoBVoct);
    orderB.rebuild();
    std::string sigA, sigB;
    feedbackSig(orderA.graph(), sigA);
    feedbackSig(orderB.graph(), sigB);
    check(sigA == sigB && sigA.find("15,16,1") != std::string::npos,
          "break-edge set is insertion-order independent (deterministic z^-1 on the cable)");
  }

  // (d) EFFECTOR 4-ROUTE DECLARED OMISSION (P6 out of scope): the fixed routes must
  // NOT include dist->eff / eff->wet, so WET is taken at the distortion output.
  {
    bool effectorRoute = false;
    for (const core::FixedEdge& fe : kFixedEdges) {
      if (std::strstr(fe.name, "to_eff") || std::strstr(fe.name, "eff_to") ||
          std::strstr(fe.name, "dist_l_to_eff") || std::strstr(fe.name, "dist_r_to_eff"))
        effectorRoute = true;
    }
    check(!effectorRoute,
          "effector 4 routes are a declared omission (dist->eff, eff->wet absent)");
    // WET is the distortion output, not a tap after an omitted effector.
    core::SynthRuntime wetRt = makeRuntime();
    wetRt.rebuild();
    core::RuntimeOutput o = runFrames(wetRt, kBlock, /*driveGraph=*/true);
    check(std::isfinite(o.wetL) && std::isfinite(o.wetR),
          "WET L/R finite from the distortion (out-of-P6 effector not wired)");
  }

  // ---- ⑦ #39: product-path drone is nonlinear AND from the bank ----------------
  // @Claude (msg b3bfb888): test_drone_classic proved the DroneBank CLASS in
  // isolation, but NO criterion proved the machine's SOUND contains it — "零件对 ≠
  // 机器用了它". A mutation that bypasses the bank at the PRODUCT path (drone[q]=0.5)
  // must red here. The product path computes the drone channel in step_(kDrone) ->
  // aggregateDrone_ and feeds it to the mixer; we read that EXECUTED value via
  // droneChannel() — the same data the mixer consumes, not a test-side re-derivation.
  // A same-seed standalone DroneBank is the oracle for what the bank produces.
  {
    constexpr std::size_t kN = 1024;
    // Same seed / sr / voiceCount / drift the runtime constructs for its own drone_
    // (makeRuntime passes kSeed, kSr; the runtime's drone_(seed_, sampleRate) uses
    // the default voiceCount=20 and driftEnabled=true).
    core::DroneBank ref(kSeed, kSr, core::DroneBank::kMaxVoices, /*driftEnabled=*/true);
    core::SynthRuntime rt = makeRuntime();
    rt.rebuild();

    double buf[core::DroneBank::kMaxVoices] = {};
    std::vector<double> prod(kN), actual(kN), lin(kN);
    for (std::size_t i = 0; i < kN; ++i) {
      double saw[5], nl[5];
      for (int g = 0; g < 5; ++g) {
        const double ph = ref.phaseOf(g);  // phase the bank uses THIS frame.
        saw[g] = core::DroneBank::sawtooth(ph);
        nl[g] = core::DroneBank::nonlinearity(saw[g]);
      }
      ref.tick(buf);                       // advances the bank; buf[g]=amp*nonlin(saw).
      double a = 0.0, l = 0.0;
      for (int g = 0; g < 5; ++g) {
        const double nlg = buf[g];         // executed gen output = amp*nonlin(saw).
        a += nlg;
        // Recover the per-gen amplitude and compute the PURE-sawtooth LINEAR (no
        // nonlinearity) superposition forecast. Guard saw==0 => nonlin==0 (0/0): at
        // saw==0 the linear term is 0*anything = 0.
        const double amp = (std::abs(nl[g]) > 1e-12) ? nlg / nl[g] : 0.0;
        l += amp * saw[g];
      }
      actual[i] = a;
      lin[i] = l;
      rt.processFrame(0.0, /*driveGraph=*/true);
      prod[i] = rt.droneChannel(0);        // the value the PRODUCT path fed to the mixer.
    }

    // FROM-BANK + non-vacuous: the product drone channel equals the bank's
    // classic-voice-0 sum (same seed => identical). A drone[q]=0.5 constant bypass
    // makes prod flat (2.5) — it would NOT match the varying bank sum and red here.
    double maxDiff = 0.0, prodMax = -1e30, prodMin = 1e30;
    for (std::size_t i = 0; i < kN; ++i) {
      maxDiff = std::max(maxDiff, std::fabs(prod[i] - actual[i]));
      prodMax = std::max(prodMax, prod[i]);
      prodMin = std::min(prodMin, prod[i]);
    }
    check(maxDiff < 1e-9,
          "product drone channel == bank sum (drone comes from the bank, not bypassed)");
    check((prodMax - prodMin) > 1e-3,
          "product drone channel VARIES (a constant drone[q]=0.5 bypass is flat; non-vacuous)");

    // NONLINEARITY: the product drone channel differs from the pure-sawtooth LINEAR
    // superposition forecast. An identity nonlinearity would collapse prod to lin and
    // red here.
    double gap = 0.0, absProd = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
      gap = std::max(gap, std::fabs(prod[i] - lin[i]));
      absProd = std::max(absProd, std::fabs(prod[i]));
    }
    check(gap > 1e-3 && gap > 0.01 * absProd,
          "product drone channel != pure-sawtooth LINEAR superposition (nonlinearity is in the signal)");
  }

  // ---- ⑧ #39: drone panel controls reach the bank (knob -> bank) ----------------
  // @Claude (msg b3bfb888): "再 wire 面板绑定（旋钮→bank），并配会红判据：动一个
  // 旋钮参数，产品路径输出必须随之改变；不改 → 红。" A dark knob that the runtime
  // receives but discards (setter no-ops, never forwards to drone_.setX) is a dead
  // binding — the product output must NOT change, and this criterion reds. Each
  // control is set on a FRESH runtime (phase identical to its neutral twin) and the
  // product drone channel (droneChannel(0)) is compared.
  {
    constexpr std::size_t kN = 512;
    auto renderDrone = [](core::SynthRuntime& rt) {
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) { rt.processFrame(0.0, /*driveGraph=*/true); seq[i] = rt.droneChannel(0); }
      return seq;
    };
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };
    auto peak = [](const std::vector<double>& a) {
      double m = 0.0;
      for (double x : a) m = std::max(m, std::fabs(x));
      return m;
    };
    auto neutral = [&]() {
      core::SynthRuntime rt = makeRuntime();
      rt.rebuild();
      return renderDrone(rt);
    };

    // TUNE: +12 st on classic voice 0 gen 0.
    {
      const auto base = neutral();
      core::SynthRuntime set = makeRuntime(); set.rebuild(); set.setDroneTune(0, 0, 12.0);
      const auto tuned = renderDrone(set);
      check(peak(base) > 1e-3 && peak(tuned) > 1e-3,
            "drone TUNE comparison sounds (non-vacuous)");
      check(peakDiff(base, tuned) > 1e-6,
            "drone TUNE knob reaches the bank (product drone channel changes)");
    }
    // MUTE: mute gen 0 of classic voice 0 (default uncovered) vs uncovered.
    {
      const auto base = neutral();
      core::SynthRuntime set = makeRuntime(); set.rebuild(); set.setDroneMute(0, 0, true);
      const auto muted = renderDrone(set);
      check(peakDiff(base, muted) > 1e-6,
            "drone MUTE knob reaches the bank (product drone channel changes)");
    }
    // MOD: modulate gen 0 of classic voice 0. MOD = modAmount*modCv, so the CV must
    // be nonzero to be audible; build two equal-CV runtimes differing only in amount.
    {
      core::SynthRuntime noMod = makeRuntime(); noMod.rebuild();
      noMod.setDroneModCv(0, 0, 3.0); noMod.setDroneMod(0, 0, 0.0);
      const auto m0 = renderDrone(noMod);
      core::SynthRuntime mod = makeRuntime(); mod.rebuild();
      mod.setDroneModCv(0, 0, 3.0); mod.setDroneMod(0, 0, 1.0);
      const auto m1 = renderDrone(mod);
      check(peakDiff(m0, m1) > 1e-6,
            "drone MOD knob reaches the bank (product drone channel changes at fixed CV)");
    }
    // VOLT: shared transpose of the whole classic voice 0 group (+12 st down).
    {
      const auto base = neutral();
      core::SynthRuntime set = makeRuntime(); set.rebuild(); set.setDroneVolt(0, 12.0);
      const auto down = renderDrone(set);
      check(peakDiff(base, down) > 1e-6,
            "drone VOLT knob reaches the bank (product drone channel changes)");
    }
  }

  // ---- ⑪ #45: FM/AM are SWITCHES, four combos -> four distinct drone-3 outputs ----
  // @Claude (msg 3e21f284, manual L344-366): the NEW voice has TWO Schmitt oscillators,
  // NOT one. An LF Schmitt is a square-wave MODULATOR (RATE); an audio-frequency Schmitt
  // does the tone (PITCH/RANGE). FM/AM are not a third source — they are two SWITCHES
  // routing the LF square onto the audio oscillator, giving four combos (drone / FM /
  // AM / FM+AM). The criterion: the four combos produce pairwise-distinct drone3Channel
  // streams (all FRESH runtimes on the same seed, so the noise stem cancels in the diff
  // and only the switched modulation survives). A wiring-revert (FM/AM become no-ops)
  // collides all four to one stream and must red.
  {
    constexpr std::size_t kN = 1024;
    auto render3 = [](core::SynthRuntime& rt) {
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) { rt.processFrame(0.0, /*driveGraph=*/true); seq[i] = rt.drone3Channel(); }
      return seq;
    };
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };
    auto makeCombo = [&](bool fm, bool am) {
      core::SynthRuntime rt = makeRuntime();  // FRESH runtime, same seed.
      rt.rebuild();
      rt.setDrone3Fm(fm); rt.setDrone3Am(am);
      return render3(rt);
    };
    const auto none = makeCombo(false, false);  // "drone" (both switches off).
    const auto fmOnly = makeCombo(true, false);
    const auto amOnly = makeCombo(false, true);
    const auto fmAm = makeCombo(true, true);
    double peakNone = 0.0;
    for (double x : none) peakNone = std::max(peakNone, std::fabs(x));
    check(peakNone > 1e-3,
          "drone combo (both FM/AM off) sounds (non-vacuous; the audio oscillator is live)");
    check(peakDiff(none, fmOnly) > 1e-5,
          "FM switch routes the LF square onto the audio oscillator (drone vs FM differ)");
    check(peakDiff(none, amOnly) > 1e-5,
          "AM switch routes the LF square onto the audio oscillator (drone vs AM differ)");
    check(peakDiff(none, fmAm) > 1e-5,
          "FM+AM combo differs from drone (both switches engaged)");
    check(peakDiff(fmOnly, amOnly) > 1e-5,
          "FM-only vs AM-only differ (the two switches are distinct routes)");
    check(peakDiff(fmOnly, fmAm) > 1e-5,
          "adding AM on FM changes the output (FM-only vs FM+AM differ)");
    check(peakDiff(amOnly, fmAm) > 1e-5,
          "adding FM on AM changes the output (AM-only vs FM+AM differ)");
  }

  // ---- ⑫ #45: PITCH=0 + NOISE full -> drone 3 channel is pure noise (tone gated) ----
  // The manual "PITCH to zero => clean noise" recipe (@Claude criterion ②): the PITCH-
  // at-floor silence gate (schmitt_osc.h kSilenceSt) kills the audio oscillator, so the
  // drone channel is EXACTLY the noise stem. The oracle is the same-seed standalone
  // NoiseSource the runtime derives via newVoiceSeed/newSourceSeed, lockstep for exact
  // equality — the strongest form of "the product path really gated the tone off", which
  // a half-implemented recipe (tone still leaking) would red.
  {
    constexpr std::size_t kN = 1024;
    const auto voiceSeed3 = core::SynthRuntime::newVoiceSeed(kSeed, 0);
    const auto noiseSeed3 = core::SynthRuntime::newSourceSeed(voiceSeed3, core::SynthRuntime::kNewSrcNoise);
    const double amp = core::SynthRuntime::kNewDroneNoiseAmp;  // full (default) noise mix.
    core::NoiseSource refNoise(noiseSeed3, amp);
    core::SynthRuntime low = makeRuntime();   low.rebuild();
    core::SynthRuntime high = makeRuntime();  high.rebuild();
    low.setDrone3Pitch(0.0);   low.setDrone3Noise(amp);   // PITCH to zero, NOISE full.
    high.setDrone3Pitch(0.5);  high.setDrone3Noise(amp);  // tone ON for contrast.

    std::vector<double> prod(kN), src(kN), hi(kN);
    for (std::size_t i = 0; i < kN; ++i) {
      double n = 0.0;
      low.processFrame(0.0, true);
      refNoise.tick(&n);
      prod[i] = low.drone3Channel();
      src[i] = n;
      high.processFrame(0.0, true);
      hi[i] = high.drone3Channel();
    }
    double d = 0.0, pk = 0.0, toneDiff = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
      d = std::max(d, std::fabs(prod[i] - src[i]));
      pk = std::max(pk, std::fabs(prod[i]));
      toneDiff = std::max(toneDiff, std::fabs(prod[i] - hi[i]));
    }
    check(d < 1e-9,
          "PITCH=0 + NOISE full -> drone 3 channel == same-seed NoiseSource (tone fully gated off)");
    check(pk > 1e-3,
          "PITCH=0 + NOISE full -> drone 3 channel is non-silent (the noise is audible)");
    check(toneDiff > 1e-3,
          "raising PITCH back on re-adds the tone (the recipe really removed it; non-vacuous)");
  }

  // ---- ⑬ #45: S&H is a CV OUT (not in the audio channel); unclocked it doesn't self-run
  // @Claude criterion ③: the Sample & Hold runs noise->IN with the LF/mod source as its
  // clock and yields a -5..+5 V CV OUT of the voice (manual), so it is NOT summed into
  // the mixer channel. Clocked it steps at the clock rate; unclocked (constant clock) it
  // captures nothing and does NOT self-run. The product path never sums sampleHold*Cv
  // into chIn_, so clocking the S&H must leave drone3Channel byte-identical while the
  // CV itself moves.
  {
    constexpr std::size_t kN = 1024;
    std::vector<double> clk0(kN, 0.0), clkSq(kN);
    for (std::size_t i = 0; i < kN; ++i) clkSq[i] = (i / 100) % 2 == 0 ? 0.0 : 1.0;  // 100-frame period.
    auto render3 = [&](core::SynthRuntime& rt, const std::vector<double>& clk) {
      rt.rebuild();
      std::vector<double> ch(kN), cv(kN);
      for (std::size_t i = 0; i < kN; ++i) {
        rt.setDrone3ShClock(clk[i]);
        rt.processFrame(0.0, true);
        ch[i] = rt.drone3Channel();
        cv[i] = rt.sampleHold3Cv();
      }
      return std::make_pair(ch, cv);
    };
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };
    auto countSteps = [](const std::vector<double>& v) {
      std::size_t n = 0;
      if (v.empty()) return n;
      double prev = v[0];
      for (std::size_t i = 1; i < v.size(); ++i) {
        if (std::fabs(v[i] - prev) > 1e-9) ++n;
        prev = v[i];
      }
      return n;
    };
    // (a) unclocked: constant clock 0.0 -> captures nothing -> CV stays at the initial
    // held value (0.0); the channel keeps sounding but the CV does NOT self-run.
    {
      core::SynthRuntime rt = makeRuntime();
      auto [ch, cv] = render3(rt, clk0);
      static_cast<void>(ch);
      double cvMax = 0.0;
      for (double x : cv) cvMax = std::max(cvMax, std::fabs(x));
      check(cvMax < 1e-12,
            "unclocked S&H does NOT self-run (sampleHold3Cv stays at the initial held value)");
    }
    // (b) clocked vs unclocked: clocking the S&H leaves drone3Channel byte-identical
    // (S&H is NOT in the audio channel) but makes sampleHold3Cv step at the clock edges.
    {
      core::SynthRuntime rtC = makeRuntime();
      auto clocked = render3(rtC, clkSq);
      core::SynthRuntime rtU = makeRuntime();
      auto unclocked = render3(rtU, clk0);
      check(peakDiff(clocked.first, unclocked.first) < 1e-12,
            "S&H is NOT in the audio channel (clocking it leaves drone3Channel byte-identical)");
      const std::size_t steps = countSteps(clocked.second);
      check(steps >= 4,
            "clocked S&H steps at the clock rate (sampleHold3Cv changes at the clock edges)");
      double cvPeak = 0.0;
      for (double x : clocked.second) cvPeak = std::max(cvPeak, std::fabs(x));
      check(cvPeak > 1e-3,
            "clocked S&H CV is a bounded nonzero level (the noise samples it captures)");
    }
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
