// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ENGINE-LEVEL CONTROLLED FIXTURE (task#65 7C2 GH#11, @Codex A′ msg 96361090).
//
// This is NOT the canonical Lunar24 machine/product oracle. It is a controlled-fixture
// regression suite for the low-level SynthRuntime / GraphCompiler / executor mechanics:
// GH#13 feedback capacity (18 self-loops), error/refusal statuses, controlled-topology
// patching, boundary and allocator guards — scenarios that need synthetic descriptor
// tables or topologies the canonical 21-module product machine does not (and must not)
// pretend to support.
//
// The CANONICAL product acceptance lives in tests/core/test_machine_definition.cpp,
// which drives the ONE product machine (core::MachineRuntimeDefinition) and carries the
// product conclusions: the full chain / four outputs / plan order + repatch effect,
// block-partition + reproducibility, render-path zero allocation, the fixed+pluggable
// preamp/env cycle exact break + partition invariance, EventTimebase -> drone_3/6 sample
// timing, classic gate/ENV OUT/CV MOD, and GH#6 VCF identity through the canonical
// vcf_path. Nothing in THIS file is counted toward product exit.
//
// Hard rule (@Claude, msg 43da88a5): each criterion here is asserted against the
// PRODUCT runtime surface (SynthRuntime) — the executor the host's realtime callback
// drives — NEVER a test-internal second executor. The runtime under test is
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
// Synthetic descriptor tables drive the engine exactly as the registry would in
// production; the runtime under test is the same SynthRuntime surface, but the
// TOPOLOGY and the conclusions here are engine-level (controlled fixture), not the
// canonical 21-module product machine. Where a comparison needs "the same VCO phase",
// each side uses a FRESH runtime so the two start from the identical deterministic
// initial state — never a reused runtime, whose phase has already advanced.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <lunar24/core/enums.h>
#include <lunar24/core/machine_runtime.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

// Allocator-count probe (#38 criterion ⑤, rule 5): the replaceable operator new/delete
// pair and its counter now live in tests/core/test_machine_runtime_allocator.cpp (this
// target only). Isolated to that TU so GCC's -Wmismatched-new-delete does not misjudge
// the malloc/free implementation as a new/delete mismatch at the ::operator new / ::operator
// delete call sites below; the ::operator new(8) call here resolves to the replaced
// operator at link time, so the deliberate-allocation negative test still fires and the
// render loop must leave the count at zero.
extern std::size_t g_allocCount;

namespace core = lunar24::core;
using core::JackId;
using core::ModuleId;

namespace {

// The large-object (SynthRuntime, which embeds the DroneBank + 2 PapaVoice + filters)
// acceptance tests create a runtime BY VALUE on the stack. Under ASan the usable stack is
// sharply reduced, and -O1 will happily inline a small test function into main, inflating
// main's frame past the guard. Keep such tests out of main's frame so each runs (and
// releases) its own (still large) frame. This is a CI-safety latch, not product semantics.
#if defined(__GNUC__) || defined(__clang__)
#define IJU_TEST_NOINLINE __attribute__((noinline))
#else
#define IJU_TEST_NOINLINE
#endif

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
constexpr std::size_t kCvModFrames = 4096;  // CV MOD trace length (registry_drone_cv_mod)
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
  for (std::size_t i = 0; i < n; ++i) last = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, driveGraph);
  return last;
}

bool sameOutput(const core::RuntimeOutput& a, const core::RuntimeOutput& b) {
  return a.wetL == b.wetL && a.wetR == b.wetR && a.dryA == b.dryA && a.dryB == b.dryB;
}

// Compare two frame-for-frame blocks of runtime output (the #46 buffer-invariance
// check over a full block run). Bit-exact: the per-frame DSP is deterministic and
// runs the same op sequence whether frames are grouped 64/128/256, so equal means
// the event acted at the SAME sample.
bool sameSeq(const core::RuntimeOutput* a, const core::RuntimeOutput* b, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i)
    if (!sameOutput(a[i], b[i])) return false;
  return true;
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

// ----------------------------------------------------------------------------
// GH#13: feedback capacity. The generated machine registry exposes EXACTLY 18
// legal, mutually non-occupying module-local self-loops (an output jack and a
// DIFFERENT input jack of the same module; each jack used once — verified count:
// vco_a 2 + vco_b 3 + keyboard 2 + envelope_a 1 + envelope_b 1 + sequencer 1 +
// drone_1/2/4/5 1 each + drone_3 2 + drone_6 2 = 18). kMaxFeedback=16, so a
// compiled plan carrying all 18 must be deterministically REJECTED in rebuild() —
// never published as "true + 16/18" by silent truncation — while a ≤16 plan must
// be accepted with feedbackCount == compiled plan count.
// ----------------------------------------------------------------------------
void registry_self_loop_feedback_capacity() {
  namespace reg = lunar24::registry;
  struct Loop {
    core::JackId src;
    core::JackId snk;
  };
  static const Loop loops[] = {
      {core::JackId::vco_a_dry_out, core::JackId::vco_a_cv_in},
      {core::JackId::vco_a_wave_out, core::JackId::vco_a_v_oct_in},
      {core::JackId::vco_b_vco_out, core::JackId::vco_b_cv_in},
      {core::JackId::vco_b_dry_out, core::JackId::vco_b_v_oct_in},
      {core::JackId::vco_b_wave_out, core::JackId::vco_b_pwm_in},
      {core::JackId::keyboard_v_oct_out, core::JackId::keyboard_clock_in},
      {core::JackId::keyboard_gate_left_main_out, core::JackId::keyboard_reset_in},
      {core::JackId::envelope_a_env_out, core::JackId::envelope_a_gate_in},
      {core::JackId::envelope_b_env_out, core::JackId::envelope_b_gate_in},
      {core::JackId::sequencer_clock_out, core::JackId::sequencer_ext_clock_in},
      {core::JackId::drone_1_env_out, core::JackId::drone_1_cv_mod_in},
      {core::JackId::drone_2_env_out, core::JackId::drone_2_cv_mod_in},
      {core::JackId::drone_4_env_out, core::JackId::drone_4_cv_mod_in},
      {core::JackId::drone_5_env_out, core::JackId::drone_5_cv_mod_in},
      {core::JackId::drone_3_cv_out, core::JackId::drone_3_gate_in},
      {core::JackId::drone_3_env_out, core::JackId::drone_3_clock_in},
      {core::JackId::drone_6_cv_out, core::JackId::drone_6_gate_in},
      {core::JackId::drone_6_env_out, core::JackId::drone_6_clock_in},
  };
  static constexpr std::uint32_t kLoopCount =
      static_cast<std::uint32_t>(sizeof(loops) / sizeof(loops[0]));
  static_assert(kLoopCount == 18, "registry self-loop count must be 18");

  // Registry-backed runtime over the real generated tables; every module gets a
  // cycle-safe (empty, valid) contract so the self-loops are admitted, and NO fixed
  // edges / routes, so the compiled plan is purely the patch's self-loops.
  core::ModuleExecutionContract cyc[core::kModuleCount];
  core::GraphModule mods[core::kModuleCount];
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    cyc[i] = core::ModuleExecutionContract{};  // GCC-visible value-init (MSVC/AppleClang accept `= {}`; GCC rejects it)
    cyc[i].sampleRate = kSr;
    cyc[i].allowedInCyclicSCC = true;  // every module is cycle-safe in this test
    mods[i].id = reg::kModules[i].id;
    mods[i].contract = &cyc[i];
  }

  // 18 > 16: all connect legally, but rebuild() must reject the over-capacity plan.
  {
    core::SynthRuntime rt(reg::kJacks, core::kJackCount, nullptr, 0, mods,
                          core::kModuleCount, kSeed, kSr, nullptr, 0);
    bool allConnect = true;
    for (const Loop& lp : loops) allConnect = allConnect && rt.connect(lp.src, lp.snk);
    check(allConnect, "all 18 registry module-local self-loops connect legally");
    check(!rt.rebuild(), "rebuild() rejects an 18-edge feedback plan (> kMaxFeedback)");
    check(!rt.graphValid(), "graphValid() is false after an over-capacity rebuild");
    check(rt.lastRebuildStatus() ==
              core::SynthRuntime::RebuildStatus::feedback_capacity_exceeded,
          "inspectable status is feedback_capacity_exceeded (not a bare bool)");
    check(rt.feedbackCount() == 0, "no partial feedback set is left behind");
    check(rt.feedbackCount() != 16, "never reports a silent true + 16/18");
  }

  // 16 == kMaxFeedback boundary: within capacity, accepted, feedbackCount equals the
  // compiled plan count.
  {
    core::SynthRuntime rt(reg::kJacks, core::kJackCount, nullptr, 0, mods,
                          core::kModuleCount, kSeed, kSr, nullptr, 0);
    bool boundaryConnect = true;
    for (std::uint32_t i = 0; i < 16; ++i)
      boundaryConnect = boundaryConnect && rt.connect(loops[i].src, loops[i].snk);
    check(boundaryConnect, "16 boundary self-loops connect legally");
    check(rt.rebuild(), "rebuild() accepts a 16-edge feedback plan (== kMaxFeedback)");
    check(rt.graphValid(), "graphValid() is true at the capacity boundary");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::ok,
          "inspectable status is ok at the boundary");
    std::uint32_t planCount = 0;
    for (const auto& r : rt.graph().regions)
      if (r.kind == core::RegionKind::cyclic)
        planCount += static_cast<std::uint32_t>(r.feedback.size());
    check(rt.feedbackCount() == planCount, "feedbackCount equals compiled plan count");
    check(planCount == 16 && rt.feedbackCount() == 16,
          "boundary accepts exactly the 16 compiled feedback lines");
  }
}

// ----------------------------------------------------------------------------
// Batch 4A (GH#5): the CLASSIC drone gate/ATT/RLS/HOLD envelope + CV MOD + ENV OUT are
// consumed by the REAL runtime, and specifically the ENV OUT write is descriptor-driven
// against the GENERATED registry's four real env_out jacks (@Codex 方案2b, msg e0f3ed09).
// The runtime under test is the same Registry-backing executor; a bare `reg::kModules`
// contract wrapper mirrors the GH#13 setup so the compiled plan is exactly the classic
// drone -> mixer fixed leg. The order 0..3 == drone 1/2/4/5; the single kDrone role owns
// the collapsed bank that produces all four classic channels.
// ----------------------------------------------------------------------------

// The single fixed classic-drone leg (drone_1 -> mixer). SynthRuntime stores `fixedEdges`
// by pointer and keeps it for the runtime's whole lifetime, so this must have static storage
// duration — it is returned by value out of makeRegistryDroneBase(), and a stack-local copy
// would dangle on the next rebuild(). (The product host passes persistent registry-backed
// arrays; this is the test-side equivalent.)
// @Codex 7C2 no-dedup: FOUR classic drone ModuleIds -> FOUR distinct kDroneBank slots (one
// per compiled ModuleId), each stepping exactly ITS OWN group. One edge per drone -> mixer so
// the mixer grouping (all four feed the same sink) mirrors the product contract. The single
// drone_1->mixer edge + whole-bank tick is GONE — there is no "one tick whole bank" anymore.
const core::FixedEdge kClassicDroneEdge[] = {
    {core::ModuleId::drone_1, core::ModuleId::mixer, "drone_1_to_mixer"},
    {core::ModuleId::drone_2, core::ModuleId::mixer, "drone_2_to_mixer"},
    {core::ModuleId::drone_4, core::ModuleId::mixer, "drone_4_to_mixer"},
    {core::ModuleId::drone_5, core::ModuleId::mixer, "drone_5_to_mixer"},
};

// Registry-backed runtime with ALL FOUR classic drone ModuleIds bound to the kDrone role and
// four fixed edges (drone_1/2/4/5 -> mixer) so they all land in the compiled plan. @Codex 7C2
// no-dedup: each classic drone is ONE kDroneBank slot stepping exactly ITS OWN group. The mixer
// holds no role, so the slots are just the four drone groups — enough to observe droneChannel()
// (the pre-VCA channel data each group emits) and the batch-4A ENV OUT writes. This BASE does
// NOT bind env_out / cv_mod_in, so its groups are unbound (the fail-closed tests use it).
// Look up a registered JackDescriptor by id (linear over the small registry). The binding
// validation AND the descriptor-driven ENV OUT oracle below read nominalMin/Max from these
// REAL generated descriptors — this test never hard-codes a ±9.9/±10 voltage.
const core::JackDescriptor* registryJack(lunar24::registry::JackId id) {
  namespace reg = lunar24::registry;
  for (std::uint32_t i = 0; i < reg::kJackCount; ++i)
    if (reg::kJacks[i].id == static_cast<core::JackId>(id)) return &reg::kJacks[i];
  return nullptr;
}

// A descriptor an oracle REQUIRES must be present. Faithful to @Codex 7a42d10a point 4: a
// missing generated descriptor is a TEST defect, NOT a silent skip — count a fault (the
// binary returns non-zero) and let the caller bail before dereferencing. Every call site
// still null-checks before use; this only makes the skip audible instead of quiet.
const core::JackDescriptor* reqRegistryJack(lunar24::registry::JackId id) {
  const core::JackDescriptor* d = registryJack(id);
  check(d != nullptr, "generated registry holds the required jack descriptor");
  return d;
}

// A jack id in [0, kMaxEdges) that is NOT a registered registry jack. The registry is sparse
// (64 registered ids < 128 capacity slots, so a hole always exists). Used for the "missing
// id" fail-closed negative, where findJackDescriptor_ returns nullptr while the id is still
// within the cvOut_ indexable range — the two must be distinguished.
core::JackId unusedRegistryId() {
  namespace reg = lunar24::registry;
  bool used[core::SynthRuntime::kMaxEdges] = {false};
  for (std::uint32_t i = 0; i < reg::kJackCount; ++i) {
    const std::uint32_t j = static_cast<std::uint32_t>(reg::kJacks[i].id);
    if (j < core::SynthRuntime::kMaxEdges) used[j] = true;
  }
  for (std::uint32_t n = 0; n < core::SynthRuntime::kMaxEdges; ++n)
    if (!used[n]) return static_cast<core::JackId>(n);
  return core::JackId{0};  // unreachable: 64 registered ids leave holes below 128
}

// Classic-drone runtime over an ARBITRARY jacks table. The real registry uses
// makeRegistryDroneBase(); the NaN/inverted-range negative passes a jacks table where the
// drone_1_env_out nominal range is pathological, so the descriptor-driven admission must
// refuse it (validRange_). The module table is shared + static (see the storage comment).
core::SynthRuntime makeRegistryDroneJacks(const core::JackDescriptor* jacks, std::uint32_t jack_n) {
  namespace reg = lunar24::registry;
  // SynthRuntime stores BOTH modules_ and each mods[i].contract as caller-owned pointers
  // (machine_runtime.h:848/850) and keeps them for its whole lifetime. This helper returns
  // the runtime by value, so these arrays MUST have static storage duration — a stack-local
  // array would dangle on the next rebuild(). The reassignment below is idempotent (the same
  // registry-derived values every call), so re-running it on static storage is harmless.
  //
  // NB: static storage is zero-initialized at load and GraphModule::contract is const (the
  // compiler/runtime only ever reads the contract), so an element is never re-value-init'ed
  // here. A `cyc[i] = ModuleExecutionContract{}` aggregate copy (the contract embeds a
  // pathDelays[16] array) ICEs GCC's gimplifier (gimple_add_tmp_var) when the destination has
  // static storage duration; a plain per-field set is portable across every toolchain.
  static core::ModuleExecutionContract cyc[core::kModuleCount];
  static core::GraphModule mods[core::kModuleCount];
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    cyc[i].sampleRate = kSr;
    cyc[i].allowedInCyclicSCC = true;  // every module is cycle-safe in this test
    mods[i].id = reg::kModules[i].id;
    mods[i].contract = &cyc[i];
  }
  core::SynthRuntime rt(jacks, jack_n, nullptr, 0, mods, core::kModuleCount, kSeed, kSr,
                        kClassicDroneEdge, 4);
  rt.bindFixedRole(core::ModuleId::drone_1, core::FixedChainRole::kDrone);
  rt.bindFixedRole(core::ModuleId::drone_2, core::FixedChainRole::kDrone);
  rt.bindFixedRole(core::ModuleId::drone_4, core::FixedChainRole::kDrone);
  rt.bindFixedRole(core::ModuleId::drone_5, core::FixedChainRole::kDrone);
  static_cast<void>(rt.rebuild());
  return rt;
}

// The unbound BASE (jacks = the real registry; env_out / cv_mod_in NOT bound). The
// fail-closed tests use this to prove no-binding == unbound and negatives reject cohorts.
core::SynthRuntime makeRegistryDroneBase() {
  namespace reg = lunar24::registry;
  return makeRegistryDroneJacks(reg::kJacks, reg::kJackCount);
}

// The product-wired CLASSIC runtime: base + the four REAL generated-registry env_out and
// cv_mod_in jacks, in the order 0..3 == drone 1/2/4/5. The two atomic setters MUST return
// true (point 1: the product's four real jacks pass admission) — discarding the bool would
// hide a silent admission failure.
core::SynthRuntime makeRegistryDroneRuntime() {
  namespace reg = lunar24::registry;
  core::SynthRuntime rt = makeRegistryDroneBase();
  check(rt.setDroneEnvOutBindings(reg::JackId::drone_1_env_out, reg::JackId::drone_2_env_out,
                                  reg::JackId::drone_4_env_out, reg::JackId::drone_5_env_out),
        "all four real registry ENV OUT jacks pass atomic admission (setter returns true)");
  check(rt.setDroneCvModInBindings(reg::JackId::drone_1_cv_mod_in, reg::JackId::drone_2_cv_mod_in,
                                   reg::JackId::drone_4_cv_mod_in, reg::JackId::drone_5_cv_mod_in),
        "all four real registry CV MOD jacks pass atomic admission (setter returns true)");
  return rt;
}

// Gate the classic group OFF, let it release, then verify the product path: near-silent
// channel, ENV OUT dropped to the row's own nominalMin, and a neighbor group left untouched.
// Then gate ONLY group 0 back ON and verify it recovers with an attack while group 1 stays
// silent, with ENV OUT tracking the envelope up to the row's own nominalMax.
//
// @Codex 52d3c620 point-3 oracle: remove every ±9.9/±10 hard-code. The oracle reads
// nominalMin/Max from the four REAL generated JackDescriptor (registryJack). drone_1/2 range
// + polarity are CONFIRMED (Polarity::bipolar, range candidate confirmed) but their TRANSFER
// is provisional; drone_4/5 range + polarity + transfer are all UNVERIFIED (Polarity::unknown,
// range candidate unverified). So below the numeric oracle is computed from each row's OWN
// descriptor via min + level*(max-min); the confirmed rows pin the exact endpoint, and no
// signed-magnitude hardware claim is made for the unverified rows — only identity + within-range.
void registry_drone_gate_envout() {
  namespace reg = lunar24::registry;
  static const core::JackId envJacks[4] = {reg::JackId::drone_1_env_out,
                                           reg::JackId::drone_2_env_out,
                                           reg::JackId::drone_4_env_out,
                                           reg::JackId::drone_5_env_out};
  core::SynthRuntime rt = makeRegistryDroneRuntime();
  for (int g = 0; g < 4; ++g) {
    rt.setDroneGroupAtt(g, 0.0);  // fast attack (neutral)
    rt.setDroneGroupRls(g, 0.0);  // fast release (neutral)
    if (!reqRegistryJack(envJacks[g])) return;  // fault-counting: missing descriptor = test defect
  }
  check(rt.droneEnvOutBound(0) && rt.droneEnvOutBound(1) && rt.droneEnvOutBound(2) &&
            rt.droneEnvOutBound(3),
        "all 4 classic groups' real registry ENV OUT descriptors are valid (fail-closed passes)");

  // Pass 1: every gate closed -> release completes -> near-silence + ENV OUT at each row min.
  for (int g = 0; g < 4; ++g) rt.setDroneGroupGate(g, false);
  constexpr std::size_t kRelFr = 12000;
  const std::size_t kRelSettle = 3000;  // skip the (fast) release transient before peaking
  double peak = 0.0;
  for (std::size_t i = 0; i < kRelFr; ++i) {
    rt.processFrame(core::RuntimeInputs{0.0, 0.0});
    if (i >= kRelSettle) {
      const double v = std::fabs(rt.droneChannel(0));
      if (v > peak) peak = v;
    }
  }
  check(peak < 1e-3, "gate-off + release-done => droneChannel(0) is near-silent");
  // Per-group descriptor oracle. level==0 after full release, so envOut == min + 0 = nominalMin.
  for (int g = 0; g < 4; ++g) {
    const core::JackDescriptor* d = reqRegistryJack(envJacks[g]);
    if (!d) return;
    const double v = rt.droneEnvOutVolts(g);
    check(v == d->nominalMin,
          "group released ENV OUT volts == OWN descriptor nominalMin (min + level*(max-min), level=0)");
    check(v >= d->nominalMin && v <= d->nominalMax,
          "group released ENV OUT volts is within OWN descriptor nominal range");
  }
  check(rt.controlVoltageAt(reg::JackId::drone_1_env_out) == rt.droneEnvOutVolts(0),
        "droneEnvOutVolts reads the SAME CV source bank entry controlVoltageAt reads");

  // Pass 2: only group 0 re-gated ON -> recovers with an attack; neighbor group 1 stays
  // silent (isolation). ENV OUT rises to nominalMax as the level climbs to exactly 1.0.
  rt.setDroneGroupGate(0, true);
  constexpr std::size_t kAtkFr = 12000;
  double after = 0.0, nbr = 0.0;
  for (std::size_t i = 0; i < kAtkFr; ++i) {
    rt.processFrame(core::RuntimeInputs{0.0, 0.0});
    const double v0 = std::fabs(rt.droneChannel(0));
    if (v0 > after) after = v0;
    const double v1 = std::fabs(rt.droneChannel(1));
    if (v1 > nbr) nbr = v1;
  }
  const core::JackDescriptor* d0 = reqRegistryJack(envJacks[0]);
  if (!d0) return;
  const double envHi = rt.droneEnvOutVolts(0);
  check(after > 0.05, "gate-on recovers the group (attack brings droneChannel back)");
  check(nbr < 1e-3, "neighbor group 1 stays silent when only group 0 is re-gated on");
  check(envHi == d0->nominalMax,
        "open ENV OUT volts == OWN descriptor nominalMax (min + level*(max-min) with level=1)");
  check(envHi >= d0->nominalMin && envHi <= d0->nominalMax,
        "open ENV OUT volts is within OWN descriptor nominal range");
  check(envHi > d0->nominalMin,
        "ENV OUT volts rise with the gate (level tracks the envelope monotonically)");
}

// The shared CV MOD is consumed through the CONTROL layer: patching a CV source into a
// classic cv_mod_in jack detunes the MOD-on group (the product path resolved the joined
// control), while a MOD-off group is inert to the same CV (design/07 §7).
void registry_drone_cv_mod() {
  namespace reg = lunar24::registry;
  // The lambda must not capture any function-local variable (MSVC C3493: a constexpr local
  // still needs a default capture mode). kCvModFrames is a file-scope constant, so `[]` stays.
  const auto modTrace = [](double cv, double depth) {
    core::SynthRuntime rt = makeRegistryDroneRuntime();
    for (int g = 0; g < 5; ++g) rt.setDroneMod(0, g, depth);
    static_cast<void>(rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_1_cv_mod_in));
    static_cast<void>(rt.rebuild());
    rt.setControlVoltage(reg::JackId::lfo_a_cv_out, cv);
    std::vector<double> tr(kCvModFrames);
    for (std::size_t i = 0; i < kCvModFrames; ++i) {
      rt.processFrame(core::RuntimeInputs{0.0, 0.0});
      tr[i] = rt.droneChannel(0);
    }
    return tr;
  };
  const std::vector<double> modOn0 = modTrace(0.0, 1.0);
  const std::vector<double> modOn4 = modTrace(4.0, 1.0);
  check(modOn0 != modOn4,
        "shared CV MOD detunes the MOD-on group (the runtime consumed the joined CV)");

  const std::vector<double> modOff0 = modTrace(0.0, 0.0);
  const std::vector<double> modOff4 = modTrace(4.0, 0.0);
  check(modOff0 == modOff4,
        "MOD-off group is inert to shared CV (CV does not move a MOD-off generator)");
}

// Same-seed reproducibility: two FRESH runtimes over the same seed + an identical gate
// schedule give a bit-identical drone channel AND ENV OUT volts stream (no hidden
// per-instance randomness; the small noise is a pure hash of seed/absolute-sample).
void registry_drone_reproducible() {
  const auto trace = [](core::SynthRuntime rt) {
    struct Tr {
      std::vector<double> ch, env;
    };
    Tr t;
    constexpr std::size_t N = 12000;
    t.ch.reserve(N);
    t.env.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
      rt.setDroneGroupGate(0, i < 2000);  // open 0..2000, released after
      rt.processFrame(core::RuntimeInputs{0.0, 0.0});
      t.ch.push_back(rt.droneChannel(0));
      t.env.push_back(rt.droneEnvOutVolts(0));
    }
    return t;
  };
  const auto a = trace(makeRegistryDroneRuntime());
  const auto b = trace(makeRegistryDroneRuntime());
  check(a.ch == b.ch, "same-seed classic drone channel is bit-identical across fresh runtimes");
  check(a.env == b.env, "same-seed classic drone ENV OUT volts is bit-identical across fresh runtimes");
}

// Fail-closed admission (@Codex 52d3c620 points 1+2). Two halves:
//   (a) NEVER-bound: every group is unbound + reads 0 (the JackId{0} sentinel would otherwise
//       collide with the real vco_a.cv_in jack id 0 — the explicit bound-state flag fixes that).
//   (b) ATOMIC cohort rejection: after a VALID cohort is admitted, ANY bad member (missing id,
//       out-of-capacity id, wrong owning module, wrong direction, pathological range for ENV)
//       makes the setter reject the WHOLE cohort and return false, clearing EVERY group — no
//       partial binding, no stale readback. The CV-MOD "no stale read" is proven behaviourally:
//       after a rejected cohort, patching CV into the group's cv_mod_in jack leaves it inert.
void registry_drone_envout_fail_closed() {
  namespace reg = lunar24::registry;
  core::SynthRuntime base = makeRegistryDroneBase();
  check(!base.droneEnvOutBound(0) && !base.droneEnvOutBound(3),
        "unbound env_out group fails closed (bound=false for the JackId{0} sentinel)");
  check(base.droneEnvOutVolts(0) == 0.0 && base.droneEnvOutVolts(3) == 0.0,
        "unbound env_out group reads 0 volts, never a stale jack");
  check(!base.droneEnvOutBound(4) && base.droneEnvOutVolts(4) == 0.0,
        "out-of-range group index fails closed (bound=false, volts=0)");

  static const core::JackId validEnv[4] = {reg::JackId::drone_1_env_out,
                                           reg::JackId::drone_2_env_out,
                                           reg::JackId::drone_4_env_out,
                                           reg::JackId::drone_5_env_out};
  static const core::JackId validCvMod[4] = {reg::JackId::drone_1_cv_mod_in,
                                             reg::JackId::drone_2_cv_mod_in,
                                             reg::JackId::drone_4_cv_mod_in,
                                             reg::JackId::drone_5_cv_mod_in};

  enum class Kind { EnvOut, CvMod };
  struct Neg {
    Kind kind;
    const char* name;
    core::JackId ids[4];
  };
  const core::JackId missing = unusedRegistryId();
  const core::JackId overCap = static_cast<core::JackId>(999);  // >= kMaxEdges (128)
  const Neg negs[] = {
      {Kind::EnvOut, "missing registry id",
       {missing, missing, missing, missing}},
      {Kind::EnvOut, "out-of-capacity id",
       {overCap, overCap, overCap, overCap}},
      {Kind::EnvOut, "wrong owning module",
       {reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out,
        reg::JackId::lfo_a_cv_out}},
      {Kind::EnvOut, "wrong direction (input jack for ENV OUT)",
       {reg::JackId::drone_1_cv_mod_in, reg::JackId::drone_1_cv_mod_in,
        reg::JackId::drone_1_cv_mod_in, reg::JackId::drone_1_cv_mod_in}},
      {Kind::CvMod, "wrong owning module",
       {reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out,
        reg::JackId::lfo_a_cv_out}},
      {Kind::CvMod, "wrong direction (output jack for CV MOD)",
       {reg::JackId::drone_1_env_out, reg::JackId::drone_1_env_out, reg::JackId::drone_1_env_out,
        reg::JackId::drone_1_env_out}},
  };
  for (const Neg& n : negs) {
    core::SynthRuntime rt = makeRegistryDroneBase();
    const bool okPre = (n.kind == Kind::EnvOut)
                           ? rt.setDroneEnvOutBindings(validEnv[0], validEnv[1], validEnv[2], validEnv[3])
                           : rt.setDroneCvModInBindings(validCvMod[0], validCvMod[1], validCvMod[2], validCvMod[3]);
    char pre[192];
    std::snprintf(pre, sizeof pre, "'%s': a valid cohort is admitted first (so refusal must clear it too)", n.name);
    check(okPre, pre);
    if (n.kind == Kind::EnvOut) {
      // @Codex 7a42d10a #1 (source-bank criterion): ONE frame AFTER the valid cohort is
      // admitted, the kDrone step writes each group's ENV OUT volts into cvOut_[envOutJack].
      // Capture that per group NOW (before the bad cohort is offered), then reject. A released
      // cohort must zero the ORIGINAL valid source slots — the old releaseEnvOut_ only cleared
      // id/flag and left cvOut_ alive, so the stale ENV OUT voltage stayed consumable.
      rt.processFrame(core::RuntimeInputs{0.0, 0.0});
      double beforeVal[4];
      for (int g = 0; g < 4; ++g) beforeVal[g] = rt.controlVoltageAt(validEnv[g]);
      for (int g = 0; g < 4; ++g) {
        char mw[192];
        std::snprintf(mw, sizeof mw,
                      "'%s': a valid cohort actually wrote non-zero ENV OUT volts into cvOut_ "
                      "(so release is meaningful, not vacuous)",
                      n.name);
        check(beforeVal[g] != 0.0, mw);
      }
      const bool ok = rt.setDroneEnvOutBindings(n.ids[0], n.ids[1], n.ids[2], n.ids[3]);
      char refused[192];
      std::snprintf(refused, sizeof refused, "'%s': the bad cohort is rejected (setter returns false)", n.name);
      check(!ok, refused);
      for (int g = 0; g < 4; ++g) {
        char m1[192];
        std::snprintf(m1, sizeof m1, "'%s': group %d unbound (NO partial binding after refusal)", n.name, g);
        char m2[192];
        std::snprintf(m2, sizeof m2, "'%s': group %d reads 0 volts (NO stale readback after refusal)", n.name, g);
        char m3[192];
        std::snprintf(m3, sizeof m3,
                      "'%s': ORIGINAL valid source-bank slot %d cleared to 0 (releaseEnvOut_ "
                      "zeroes cvOut_, no stale ENV OUT voltage survives)",
                      n.name, g);
        check(!rt.droneEnvOutBound(g), m1);
        check(rt.droneEnvOutVolts(g) == 0.0, m2);
        check(rt.controlVoltageAt(validEnv[g]) == 0.0, m3);
      }
    } else {
      const bool ok = rt.setDroneCvModInBindings(n.ids[0], n.ids[1], n.ids[2], n.ids[3]);
      char refused[192];
      std::snprintf(refused, sizeof refused, "'%s': the bad cohort is rejected (setter returns false)", n.name);
      check(!ok, refused);
    }
  }

  // ENV OUT pathological-range negatives (validRange_): a NaN or inverted nominal range must
  // be refused, so a garbage voltage is never written into the CV source bank. The jacks table
  // is a static COPY of the real registry with drone_1_env_out corrupted (SynthRuntime keeps
  // the pointer, so the table must outlive the runtime).
  struct RangeCase {
    const char* name;
    double min;
    double max;
  };
  const RangeCase ranges[] = {
      {"NaN nominalMin", std::numeric_limits<double>::quiet_NaN(), 10.0},
      {"inverted range (max < min)", 10.0, -10.0},
  };
  for (const RangeCase& rc : ranges) {
    static core::JackDescriptor bad[core::SynthRuntime::kMaxEdges];
    for (std::uint32_t i = 0; i < reg::kJackCount; ++i) bad[i] = reg::kJacks[i];
    for (std::uint32_t i = 0; i < reg::kJackCount; ++i) {
      if (bad[i].id == static_cast<core::JackId>(reg::JackId::drone_1_env_out)) {
        bad[i].nominalMin = rc.min;
        bad[i].nominalMax = rc.max;
        break;
      }
    }
    {
      core::SynthRuntime rt = makeRegistryDroneJacks(bad, reg::kJackCount);
      const bool ok = rt.setDroneEnvOutBindings(validEnv[0], validEnv[1], validEnv[2], validEnv[3]);
      char refused[192];
      std::snprintf(refused, sizeof refused, "ENV range '%s' is rejected by admission (validRange_)", rc.name);
      check(!ok, refused);
      char unbound[192];
      std::snprintf(unbound, sizeof unbound, "ENV range '%s': every group unbound + reads 0 (no NaN/inverted write)", rc.name);
      check(!rt.droneEnvOutBound(0) && !rt.droneEnvOutBound(1) && rt.droneEnvOutVolts(0) == 0.0 &&
                rt.droneEnvOutVolts(1) == 0.0,
            unbound);
    }
  }

  // CV MOD "no stale read" (@Codex 7a42d10a #2): a rejected cohort must UN-APPLY the shared
  // mod CV it had been consuming. The OLD test rejected BEFORE any frame, so the 4V never
  // entered the group (modCvG_ stayed 0 either way) — it was FAKE-GREEN. Here we first CONSUME
  // the joined CV (one processFrame drives modCvG_[0] via applyControlCv_), THEN reject, and
  // require the group's ACTUAL shared modCv to return to 0. Old releaseCvModIn_ only cleared
  // id/flag and left modCvG_ at 4 => the probe stays 4 => red.
  //
  // Why droneGroupModCv(0) (direct inspector) and NOT a full droneChannel(0) vector compare:
  // consuming a CV detunes the oscillator, and after release the phase has already advanced one
  // frame, so even a correct release leaves the post-reject vector phase-diverged from the never-
  // consumed baseline — a false signal that exactly the two traces differ.
  const auto cvmodProbe = [](double cv) {
    core::SynthRuntime rt = makeRegistryDroneBase();
    for (int g = 0; g < 5; ++g) rt.setDroneMod(0, g, 1.0);  // MOD-on => shared CV is audible
    check(rt.setDroneCvModInBindings(validCvMod[0], validCvMod[1], validCvMod[2], validCvMod[3]),
          "CV MOD stale-read: valid cohort pre-admitted");
    static_cast<void>(rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_1_cv_mod_in));
    static_cast<void>(rt.rebuild());
    rt.setControlVoltage(reg::JackId::lfo_a_cv_out, cv);
    rt.processFrame(core::RuntimeInputs{0.0, 0.0});  // CONSUME: applyControlCv_ drives modCvG_[0] to `cv` for real.
    const double beforeReject = rt.droneGroupModCv(0);  // group 0's executed shared modCv
    check(!rt.setDroneCvModInBindings(reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out,
                                      reg::JackId::lfo_a_cv_out, reg::JackId::lfo_a_cv_out),
          "CV MOD stale-read: the bad cohort is rejected");
    const double afterReject = rt.droneGroupModCv(0);
    return std::make_pair(beforeReject, afterReject);
  };
  const auto p4 = cvmodProbe(4.0);
  const auto p0 = cvmodProbe(0.0);
  check(p4.first == 4.0,
        "CV MOD stale-read: a consumed 4V actually ENTERED the group while bound (modCvG_=4 — "
        "proves the CV was applied, not the vacuous 0-before case that was previously fake-green)");
  check(p4.second == 0.0,
        "CV MOD stale-read: rejecting the cohort zeroes the retained modCv (releaseCvModIn_ "
        "drives setGroupModCv(0) — no stale modulation lingers)");
  check(p0.first == 0.0, "CV MOD stale-read: 0V consume leaves modCv at 0");
  check(p0.second == 0.0, "CV MOD stale-read: 0V consume + reject still leaves modCv at 0");
}

// @Codex 7C2 req. 1 (NO execution-kind dedup): the four classic drone ModuleIds must yield
// FOUR distinct kDroneBank ExecutionSlots — NEVER one merged "whole bank" slot. Each slot
// carries its own ModuleId; step(slot.id, kind) runs exactly that drone's group. This is what
// makes env_follower -> drone_2.cv_mod (drone_2 running before its source) a real #64-compliant
// per-module order instead of one collapsed bank step.
IJU_TEST_NOINLINE void registry_drone_no_dedup_slots() {
  core::SynthRuntime rt = makeRegistryDroneRuntime();
  check(rt.execSlotCount() == 4,
        "four classic drone ModuleIds -> four kDroneBank slots (execSlotCount==4, no kind dedup)");
  bool saw1 = false, saw2 = false, saw4 = false, saw5 = false;
  for (std::uint32_t i = 0; i < rt.execSlotCount(); ++i) {
    const core::ExecutionSlot& s = rt.execSlotAt(i);
    check(s.kind == core::ExecutionKind::kDroneBank,
          "every classic drone slot is a kDroneBank ExecutionKind");
    switch (s.id) {
      case core::ModuleId::drone_1: saw1 = true; break;
      case core::ModuleId::drone_2: saw2 = true; break;
      case core::ModuleId::drone_4: saw4 = true; break;
      case core::ModuleId::drone_5: saw5 = true; break;
      default:
        check(false, "classic drone slots carry ONLY the four registry drone_N ModuleIds");
        break;
    }
  }
  check(saw1 && saw2 && saw4 && saw5,
        "drone_1/2/4/5 are all present as DISTINCT slots (none collapsed into one bank)");
}

// @Codex 7C2 req. 4 (EXPLICIT strict binding policy, never inferred): an owning definition
// enables strictness via setStrictBindings(); only then does rebuild() fail-closed TWO distinct
// ways — a compiled-region module with NO binding (missing_execution_binding) vs one EXPLICITLY
// bound to kUnsupported (unsupported_module). Default OFF keeps the permissive synthetic path.
IJU_TEST_NOINLINE void registry_strict_binding_policy() {
  // (a) default OFF: the permissive path compiles the registry-drone plan; mixer is unbound
  //     but permissive mode keeps it outside the executor's scope — never a bind-status failure.
  {
    core::SynthRuntime rt = makeRegistryDroneRuntime();
    check(rt.rebuild(), "strict OFF: permissive rebuild succeeds");
    const auto st = rt.lastRebuildStatus();
    check(st != core::SynthRuntime::RebuildStatus::missing_execution_binding &&
              st != core::SynthRuntime::RebuildStatus::unsupported_module,
          "strict OFF: permissive rebuild takes no fail-closed binding status");
  }
  // (b) strict ON + a compiled region that contains an UNBOUND module (mixer, which no kind
  //     binding in this fixture) -> REFUSE with missing_execution_binding (distinct from a
  //     kUnsupported module). setStrictBindings dirties the plan so the prior permissive build
  //     is re-preflighted.
  {
    core::SynthRuntime rt = makeRegistryDroneBase();
    rt.setStrictBindings(true);
    check(!rt.rebuild(), "strict ON + unbound region module: rebuild REFUSES");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::missing_execution_binding,
          "strict ON + unbound module -> missing_execution_binding");
  }
  // (c) strict ON + EVERY region module bound but one EXPLICITLY kUnsupported -> REFUSE with
  //     unsupported_module. Binding mixer -> kMixer removes the missing-binding shadow so the
  //     declared-deferred drone_1 is the one that triggers.
  {
    core::SynthRuntime rt = makeRegistryDroneBase();
    rt.bindExecutionKind(core::ModuleId::mixer, core::ExecutionKind::kMixer);
    rt.bindExecutionKind(core::ModuleId::drone_1, core::ExecutionKind::kUnsupported);
    rt.setStrictBindings(true);
    check(!rt.rebuild(), "strict ON + explicit kUnsupported module: rebuild REFUSES");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::unsupported_module,
          "strict ON + explicitly-deferred module -> unsupported_module (NOT missing_binding)");
  }
}

// @Codex 263cb3ca point 1 (ACTIVE-only strict preflight): strictness must judge a module ACTIVE
// by whether it is an endpoint of THIS candidate's effective PatchEdge or FixedEdge — NOT by
// merely existing in the inventory / landing in a compiled region. The owning definition binds
// the six not-yet-integrated control sources to kUnsupported; an ISOLATED one must NOT fail the
// build, but the SAME module patched into a real edge MUST.
IJU_TEST_NOINLINE void registry_strict_active_only() {
  namespace reg = lunar24::registry;
  {
    core::SynthRuntime rt = makeRegistryDroneBase();
    // Every ACTIVE module (drone_1/2/4/5 via the fixed routes + the mixter sink) gets a real kind.
    rt.bindExecutionKind(core::ModuleId::drone_1, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_2, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_4, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_5, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::mixer, core::ExecutionKind::kMixer);
    // lfo_a: declared-deferred control source bound to kUnsupported but NOT patched into any
    // effective edge yet -> NOT active -> the strict build must SUCCEED (never a false fail).
    rt.bindExecutionKind(core::ModuleId::lfo_a, core::ExecutionKind::kUnsupported);
    rt.setStrictBindings(true);
    check(rt.rebuild(), "strict ON + isolated kUnsupported lfo_a: rebuild succeeds");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::ok,
          "isolated kUnsupported (inventory-only) -> status ok, NOT unsupported_module");
    check(rt.execSlotCount() == 5,
          "isolated kUnsupported contributes no execution slot (4 drones + mixer = 5)");

    // Now PATCH lfo_a.cv_out -> vcf.cv_r_in: lfo_a becomes an effective PatchEdge endpoint, so
    // it IS active. Bound to kUnsupported, the strict rebuild must REFUSE with the PRECISE
    // unsupported_module (not graph_unchanged, not missing_binding, never a silent skip).
    rt.bindExecutionKind(core::ModuleId::vcf, core::ExecutionKind::kVcfPath);  // vcf is not "missing"
    check(rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_r_in),
          "lfo_a.cv_out -> vcf.cv_r_in cable connects");
    check(!rt.rebuild(), "lfo_a now ACTIVE as kUnsupported: rebuild REFUSES");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::unsupported_module,
          "ACTIVE kUnsupported lfo_a -> precise unsupported_module (NOT missing_binding)");
  }
}

// @Codex 263cb3ca point 2 (bindExecutionKind must dirty the plan on BOTH add + update): a
// successful strict rebuild followed by a kind change must re-preflight — otherwise the next
// rebuild() short-circuits to graph_unchanged and the OLD preflight/slots survive, violating
// the explicit strict policy. This demonstrates the supported->unsupported flip on the UPDATE
// branch (the add branch is also dirtied: mixer is bound here via a never-bound id).
IJU_TEST_NOINLINE void registry_strict_binding_kind_dirty() {
  {
    core::SynthRuntime rt = makeRegistryDroneBase();
    rt.bindExecutionKind(core::ModuleId::drone_1, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_2, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_4, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::drone_5, core::ExecutionKind::kDroneBank);
    rt.bindExecutionKind(core::ModuleId::mixer, core::ExecutionKind::kMixer);  // ADD branch
    rt.setStrictBindings(true);
    check(rt.rebuild(), "strict ON + all active modules bound: first rebuild succeeds");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::ok,
          "first strict rebuild status ok");
    check(rt.execSlotCount() == 5, "one slot per active module (4 drones + mixer)");

    // Flip drone_1 kDroneBank -> kUnsupported on the UPDATE branch. This must dirty the plan so
    // the second rebuild() does a REAL preflight and REFUSES with unsupported_module — NOT
    // graph_unchanged (which would leave the 5 old slots in place).
    rt.bindExecutionKind(core::ModuleId::drone_1, core::ExecutionKind::kUnsupported);
    check(!rt.rebuild(), "kind flip after a valid rebuild re-preflights and REFUSES");
    check(rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::unsupported_module,
          "supported->unsupported kind flip -> unsupported_module (NOT graph_unchanged)");
  }
}

// @Codex 7a42d10a #3 (sentinel removal): the old code used JackId{0} as an "unbound" sentinel,
// which collides with the REAL vco_a.cv_in jack id 0 and would wrongly treat a group
// LEGITIMATELY bound to jack id 0 as unbound. The fix decides only by the explicit bound-state
// flags. Pin group 0 to a legal jack id 0 via a synthetic cohort (copies of the real drone
// descriptors with drone_1's id re-pointed to 0, so no vco_a id-0 collision remains) and verify
// BOTH directions really work: ENV OUT writes/reads cvOut_[0], CV MOD input id 0 is consumed.
void registry_drone_envout_id0_sentinel() {
  namespace reg = lunar24::registry;
  const core::JackDescriptor* e0 = reqRegistryJack(reg::JackId::drone_1_env_out);
  const core::JackDescriptor* e1 = reqRegistryJack(reg::JackId::drone_2_env_out);
  const core::JackDescriptor* e2 = reqRegistryJack(reg::JackId::drone_4_env_out);
  const core::JackDescriptor* e3 = reqRegistryJack(reg::JackId::drone_5_env_out);
  const core::JackDescriptor* c1 = reqRegistryJack(reg::JackId::drone_1_cv_mod_in);
  const core::JackDescriptor* c2 = reqRegistryJack(reg::JackId::drone_2_cv_mod_in);
  const core::JackDescriptor* c4 = reqRegistryJack(reg::JackId::drone_4_cv_mod_in);
  const core::JackDescriptor* c5 = reqRegistryJack(reg::JackId::drone_5_cv_mod_in);
  const core::JackDescriptor* lfo = reqRegistryJack(reg::JackId::lfo_a_cv_out);
  if (!e0 || !e1 || !e2 || !e3 || !c1 || !c2 || !c4 || !c5 || !lfo) return;  // faults already counted

  // ENV OUT half: group 0's env_out jack is id 0 (a real vco_a collision id). The write must
  // land in cvOut_[0] and droneEnvOutVolts(0) must read it — NOT return 0 from a sentinel guard.
  static core::JackDescriptor id0Env[4];
  id0Env[0] = *e0;
  id0Env[0].id = core::JackId{0};
  id0Env[1] = *e1;
  id0Env[2] = *e2;
  id0Env[3] = *e3;
  {
    core::SynthRuntime rt = makeRegistryDroneJacks(id0Env, 4);
    check(rt.setDroneEnvOutBindings(core::JackId{0}, reg::JackId::drone_2_env_out,
                                    reg::JackId::drone_4_env_out, reg::JackId::drone_5_env_out),
          "ENV OUT: cohort with group-0 jack id 0 is ADMITTED (id 0 is not auto-rejected)");
    check(rt.droneEnvOutBound(0) && rt.droneEnvOutBound(1) && rt.droneEnvOutBound(2) &&
              rt.droneEnvOutBound(3),
          "ENV OUT: all four groups bound after the id-0 cohort (bound-state flag, not sentinel)");
    rt.processFrame(core::RuntimeInputs{0.0, 0.0});
    check(rt.controlVoltageAt(core::JackId{0}) != 0.0,
          "ENV OUT: the id-0 jack's ENV OUT source slot is actually written (cvOut_[0] is live)");
    check(rt.droneEnvOutVolts(0) == rt.controlVoltageAt(core::JackId{0}),
          "ENV OUT: droneEnvOutVolts(0) reads the id-0 slot (old sentinel guard returned 0)");
  }

  // CV MOD half: group 0's cv_mod_in jack is id 0. A patched 4V must be consumed by the group
  // (cvModInGroupOf_ resolves id 0); the old sentinel guard dropped id 0 silently -> the group
  // stayed at modCv 0. @Codex 7a42d10a #3.
  //
  // NOTE on the fixed VOCT/VCF sinks: SynthRuntime's voctA_/voctB_/vcfCvL_/vcfCvR_ default to
  // JackId{0} (= vco_a's cv_in jack). applyControlCv_ checks `snk == voctA_` (etc.) BEFORE the
  // drone-group branch, so in a REGISTRY WHERE id 0 IS the drone group's cv_mod_in (this
  // synthetic table re-points drone_1_cv_mod_in to id 0), the id-0 cable would be routed to
  // VCO A's otave instead of the drone group. That is genuine product wiring, not the #3
  // bug, so here we must NOT let vco_a shadow the drone group: point the fixed sinks off id 0.
  // A catch-22 of the sentinel bug is that "group 0 bound to a real id-0 jack" in a standard
  // registry IS vco_a.cv_in — only a registry whose id-0 jack is the drone cv_mod_in exercises
  // the drone branch for id 0, which is exactly the synthetic cohort the directive calls for.
  static core::JackDescriptor id0Cv[5];
  id0Cv[0] = *lfo;  // source jack must be present for patch_.connect to resolve it
  id0Cv[1] = *c1;
  id0Cv[1].id = core::JackId{0};
  id0Cv[2] = *c2;
  id0Cv[3] = *c4;
  id0Cv[4] = *c5;
  const auto id0CvTrace = [](double cv) {
    core::SynthRuntime st = makeRegistryDroneJacks(id0Cv, 5);
    st.setVoctBindings(core::JackId{900}, core::JackId{901});  // off id 0: don't shadow the drone
    st.setVcfCvBindings(core::JackId{902}, core::JackId{903}); // off id 0: don't shadow the drone
    check(st.setDroneCvModInBindings(core::JackId{0}, reg::JackId::drone_2_cv_mod_in,
                                     reg::JackId::drone_4_cv_mod_in, reg::JackId::drone_5_cv_mod_in),
          "CV MOD: id-0 cohort admitted (group-0 cv_mod_in is jack id 0)");
    static_cast<void>(st.connect(reg::JackId::lfo_a_cv_out, core::JackId{0}));
    static_cast<void>(st.rebuild());
    st.setControlVoltage(reg::JackId::lfo_a_cv_out, cv);
    st.processFrame(core::RuntimeInputs{0.0, 0.0});  // consume: applyControlCv_ drives group 0's shared modCv
    return st.droneGroupModCv(0);  // the group's executed shared modCv (0 if the CV was dropped)
  };
  check(id0CvTrace(0.0) == 0.0, "id-0 CV MOD: 0V leaves the group's shared modCv at 0");
  check(id0CvTrace(4.0) == 4.0,
        "id-0 CV MOD: 4V is CONSUMED by group 0 (cvModInGroupOf_ resolves a real jack id 0, "
        "not -1 — the old sentinel guard silently dropped it)");
}

// Block-partition invariance of the batch-4A ENV OUT path: with a HELD gate the envelope
// level is a deterministic per-frame function, so the ENV OUT volts every classic group
// writes into the CV source bank last frame is identical under 64/128/256 partitions.
// The gate is set EXPLICITLY open (point 6: the behavior test never leans on the provisional
// default gate state as hardware evidence) and the terminal value is compared to the real
// descriptor's nominalMax, not a hard-coded ±9.9.
void registry_drone_envout_partition() {
  namespace reg = lunar24::registry;
  const core::JackDescriptor* d0 = reqRegistryJack(reg::JackId::drone_1_env_out);
  if (!d0) return;
  constexpr std::size_t kBlocks[3] = {64, 128, 256};
  constexpr std::size_t kTot = 256;
  static const core::RuntimeInputs kSilence[kTot] = {core::RuntimeInputs{0.0, 0.0}};
  double finalEnv[3] = {}, finalCh[3] = {};
  core::RuntimeOutput dummy[kTot];
  for (int bi = 0; bi < 3; ++bi) {
    core::SynthRuntime rt = makeRegistryDroneRuntime();
    rt.setDroneGroupAtt(0, 0.0);
    rt.setDroneGroupRls(0, 0.0);
    rt.setDroneGroupGate(0, true);  // explicit held gate (not the provisional default)
    for (std::size_t b = 0; b < kTot; b += kBlocks[bi])
      rt.processBlock(kSilence + b, kBlocks[bi], dummy + b);
    finalEnv[bi] = rt.droneEnvOutVolts(0);
    finalCh[bi] = rt.droneChannel(0);
  }
  check(finalEnv[0] == finalEnv[1] && finalEnv[1] == finalEnv[2],
        "classic drone ENV OUT volts is block-partition invariant (64/128/256)");
  check(finalEnv[0] == d0->nominalMax,
        "held-gate open ENV OUT volts == OWN descriptor nominalMax (level==1, min+level*(max-min))");
  check(finalEnv[0] >= d0->nominalMin && finalEnv[0] <= d0->nominalMax,
        "held-gate open ENV OUT volts within OWN descriptor nominal range");
  check(finalCh[0] == finalCh[1] && finalCh[1] == finalCh[2],
        "classic drone channel is block-partition invariant (64/128/256)");
}

// ----------------------------------------------------------------------------
// GH#6 — VCF identity / calibration config entry on the product runtime.
// Design/07 §7: the whole VCF→distortion→gain level-dependent path is calibrated by a
// versioned (seed, version, calibration) profile; L/R calibration/nonlinear state are
// independent. The oracle below asserts the EXECUTED state (via the no-alloc
// inspectors), the WET-path distinguishability, the trim->distortion level coupling,
// L/R isolation, fail-closed admission, and bit-identical reproducibility — all on the
// real SynthRuntime (makeRuntime), never a test shadow.
// ----------------------------------------------------------------------------

bool vecBitIdentical(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}
double vecPeakDiff(const std::vector<double>& a, const std::vector<double>& b) {
  std::size_t n = a.size() < b.size() ? a.size() : b.size();
  double d = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = std::fabs(a[i] - b[i]);
    if (v > d) d = v;
  }
  return d;
}

// Render the WET path of a fresh, product-wired runtime configured with the identity
// triple. The runtime drives itself from the bound VCOs (silence ext), so the chain
// (mixer -> vcf -> dist) produces a real WET signal; the identity config mutates the
// executed VCF input drive / distortion drive-rail / staging gain inside that path.
struct IdentityWet {
  std::vector<double> wetL, wetR;
};
IdentityWet renderIdentityWet(std::uint64_t seed, const core::CalibrationState& calib,
                              double dist = 0.0, double gain = 0.0, bool configured = true) {
  constexpr std::size_t kN = 1024;
  core::SynthRuntime rt = makeRuntime();
  rt.rebuild();
  if (configured) rt.configureVcfIdentity(1u, seed, calib);
  rt.setDistortion(dist, gain);
  IdentityWet w;
  w.wetL.resize(kN);
  w.wetR.resize(kN);
  core::RuntimeOutput o{};
  for (std::size_t i = 0; i < kN; ++i) {
    o = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
    w.wetL[i] = o.wetL;
    w.wetR[i] = o.wetR;
  }
  return w;
}

// Requirement 4 oracle — the config entry is real and atomic, the executed profile is
// observable, the WET path is distinguishable pre/post config, and with dist/gain OPEN
// a trim change genuinely moves the level the distortion folds.
void gh6_config_entry() {
  core::CalibrationState calib{};
  calib.vcfLeftTrim = 1.0f;
  calib.vcfRightTrim = 1.0f;
  // The entry is consumed from a REAL DeviceStateV1 triple — identityModelVersion,
  // identitySeed.seed, calibration — never bare constants wearing a state label.
  core::DeviceStateV1 state{};
  state.identityModelVersion = 1u;
  state.identitySeed.seed = kSeed;
  state.calibration = calib;
  core::SynthRuntime rt = makeRuntime();
  rt.rebuild();
  check(rt.configureVcfIdentity(state.identityModelVersion, state.identitySeed.seed,
                                state.calibration),
        "config entry accepts v1 identityModelVersion + finite positive trims (DeviceStateV1 triple)");
  check(rt.vcfIdentityConfigured(), "vcfIdentityConfigured() is true after a valid config");

  // Inspectors report the EXECUTED state (re-derived profile, not a test shadow).
  const auto prof = core::deriveVcfIdentityProfile(kSeed, 1u);
  check(rt.vcfInputDrive(0) == prof.left.vcfDrive, "executed VCF L input drive == derived L vcfDrive");
  check(rt.vcfInputDrive(1) == prof.right.vcfDrive, "executed VCF R input drive == derived R vcfDrive");
  check(rt.distortionDrive(0) == core::Distortion::kDriveFold * (1.0 + prof.left.distDrive),
        "executed Distortion L drive == kDriveFold*(1 + L distDrive)");
  check(rt.distortionRail(0) == core::Distortion::kSaturationVoltage *
                                    (1.0 + core::kDistRailStiffness * prof.left.distDrive),
        "executed Distortion L rail == kSaturationVoltage*(1 + k*L distDrive)");
  check(rt.distortionDrive(1) == core::Distortion::kDriveFold * (1.0 + prof.right.distDrive),
        "executed Distortion R drive == kDriveFold*(1 + R distDrive)");
  check(rt.distortionRail(1) == core::Distortion::kSaturationVoltage *
                                    (1.0 + core::kDistRailStiffness * prof.right.distDrive),
        "executed Distortion R rail == kSaturationVoltage*(1 + k*R distDrive)");
  check(rt.vcfPathStagingGain(0) == prof.left.pathGain, "executed staging gain L == derived L pathGain");
  check(rt.vcfPathStagingGain(1) == prof.right.pathGain, "executed staging gain R == derived R pathGain");

  // WET path distinguishable: configured vs unconfigured fresh runtimes differ (the
  // staging gain + VCF fold are genuinely exercised, not a no-op).
  const auto wetC = renderIdentityWet(kSeed, calib);
  const auto wetU = renderIdentityWet(kSeed, calib, 0.0, 0.0, /*configured=*/false);
  check(vecPeakDiff(wetC.wetL, wetU.wetL) > 1e-4,
        "config ON vs OFF: WET path is distinguishable (staging + VCF fold applied)");

  // dist/gain OPEN + trim change: the level the distortion actually folds really moves.
  core::CalibrationState trimA{};
  trimA.vcfLeftTrim = 1.0f;
  trimA.vcfRightTrim = 1.0f;
  core::CalibrationState trimB{};
  trimB.vcfLeftTrim = 0.4f;
  trimB.vcfRightTrim = 1.0f;
  const auto a = renderIdentityWet(kSeed, trimA, /*dist=*/1.0, /*gain=*/1.0);
  const auto b = renderIdentityWet(kSeed, trimB, /*dist=*/1.0, /*gain=*/1.0);
  check(vecPeakDiff(a.wetL, b.wetL) > 1e-4,
        "with dist/gain open, changing the L trim changes the distortion-seen level (WET differs)");
}

// Requirement 4 fail-closed — a bad (version / NaN / Inf / <=0 trim) config is rejected
// with NO change under the fixed "keep old complete profile" policy.
void gh6_fail_closed() {
  core::CalibrationState ok{};
  ok.vcfLeftTrim = 1.0f;
  ok.vcfRightTrim = 1.0f;

  // Apply a valid config first so the old complete profile is observable pre-reject.
  core::SynthRuntime rt = makeRuntime();
  rt.rebuild();
  check(rt.configureVcfIdentity(1u, kSeed, ok), "baseline valid config applied");
  const double dL0 = rt.distortionDrive(0), rL0 = rt.distortionRail(0), stL0 = rt.vcfPathStagingGain(0);
  const double dR0 = rt.distortionDrive(1), rR0 = rt.distortionRail(1), stR0 = rt.vcfPathStagingGain(1);
  const double vL0 = rt.vcfInputDrive(0), vR0 = rt.vcfInputDrive(1);

  core::CalibrationState badTrim{};
  badTrim.vcfLeftTrim = 1.0f;
  badTrim.vcfRightTrim = 1.0f;
  check(!rt.configureVcfIdentity(2u, kSeed, badTrim), "unknown version (v2) is rejected");
  core::CalibrationState nan{};
  nan.vcfLeftTrim = std::numeric_limits<float>::quiet_NaN();
  nan.vcfRightTrim = 1.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, nan), "NaN left trim is rejected");
  core::CalibrationState infc{};
  infc.vcfLeftTrim = std::numeric_limits<float>::infinity();
  infc.vcfRightTrim = 1.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, infc), "Inf left trim is rejected");
  core::CalibrationState zeroc{};
  zeroc.vcfLeftTrim = 0.0f;
  zeroc.vcfRightTrim = 1.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, zeroc), "zero left trim is rejected");
  core::CalibrationState negc{};
  negc.vcfLeftTrim = -1.0f;
  negc.vcfRightTrim = 1.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, negc), "negative left trim is rejected");

  // RIGHT-side admission is independently gated too — the "ignore one-side calib"
  // negative control must be pinned in-repo on BOTH sides, not left alone.
  core::CalibrationState nanR{};
  nanR.vcfLeftTrim = 1.0f;
  nanR.vcfRightTrim = std::numeric_limits<float>::quiet_NaN();
  check(!rt.configureVcfIdentity(1u, kSeed, nanR), "NaN right trim is rejected");
  core::CalibrationState infR{};
  infR.vcfLeftTrim = 1.0f;
  infR.vcfRightTrim = std::numeric_limits<float>::infinity();
  check(!rt.configureVcfIdentity(1u, kSeed, infR), "Inf right trim is rejected");
  core::CalibrationState zeroR{};
  zeroR.vcfLeftTrim = 1.0f;
  zeroR.vcfRightTrim = 0.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, zeroR), "zero right trim is rejected");
  core::CalibrationState negR{};
  negR.vcfLeftTrim = 1.0f;
  negR.vcfRightTrim = -1.0f;
  check(!rt.configureVcfIdentity(1u, kSeed, negR), "negative right trim is rejected");

  // The old complete profile is fully intact after EVERY rejection (no half-profile).
  check(rt.distortionDrive(0) == dL0 && rt.distortionRail(0) == rL0 &&
            rt.vcfPathStagingGain(0) == stL0,
        "rejections leave the L profile fully intact (no partial update)");
  check(rt.distortionDrive(1) == dR0 && rt.distortionRail(1) == rR0 &&
            rt.vcfPathStagingGain(1) == stR0,
        "rejections leave the R profile fully intact (no partial update)");
  check(rt.vcfInputDrive(0) == vL0 && rt.vcfInputDrive(1) == vR0,
        "rejections leave the VCF input drives intact");
  check(rt.vcfIdentityConfigured(), "failure path keeps identity configured (old state preserved)");

  // Behavioral no-residue: two fresh runtimes, one with a valid config, one that fires
  // EVERY rejection after the valid config. The rejections must be no-ops -> WET is
  // bit-identical (a residue would move the rendered path).
  const auto renderRT = [&](bool fireRejects) {
    constexpr std::size_t kN = 1024;
    core::SynthRuntime r = makeRuntime();
    r.rebuild();
    r.configureVcfIdentity(1u, kSeed, ok);
    if (fireRejects) {
      r.configureVcfIdentity(2u, kSeed, badTrim);
      r.configureVcfIdentity(1u, kSeed, nan);
      r.configureVcfIdentity(1u, kSeed, infc);
      r.configureVcfIdentity(1u, kSeed, zeroc);
      r.configureVcfIdentity(1u, kSeed, negc);
      r.configureVcfIdentity(1u, kSeed, nanR);
      r.configureVcfIdentity(1u, kSeed, infR);
      r.configureVcfIdentity(1u, kSeed, zeroR);
      r.configureVcfIdentity(1u, kSeed, negR);
    }
    std::vector<double> wl(kN), wr(kN);
    core::RuntimeOutput o{};
    for (std::size_t i = 0; i < kN; ++i) {
      o = r.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
      wl[i] = o.wetL;
      wr[i] = o.wetR;
    }
    return std::make_pair(wl, wr);
  };
  const auto clean = renderRT(false);
  const auto rejected = renderRT(true);
  check(vecBitIdentical(clean.first, rejected.first),
        "no residue: firing every rejection leaves WET L bit-identical");
  check(vecBitIdentical(clean.second, rejected.second),
        "no residue: firing every rejection leaves WET R bit-identical");
}

// Requirement 4 L/R isolation — changing the LEFT calibration trim moves only the L
// path; R is bit-identical. And the L/R profile domains are genuinely distinct (a
// "share/steal L profile to R" mutation is caught by the staging-gain inspectors).
void gh6_lr_isolation() {
  const auto prof = core::deriveVcfIdentityProfile(kSeed, 1u);
  check(prof.left.pathGain != prof.right.pathGain,
        "seed yields distinct L/R pathGain (independent, non-colliding domains)");
  check(prof.left.vcfDrive != prof.right.vcfDrive || prof.left.distDrive != prof.right.distDrive,
        "seed yields distinct L/R (vcfDrive / distDrive)");

  core::CalibrationState calA{};
  calA.vcfLeftTrim = 1.0f;
  calA.vcfRightTrim = 1.0f;
  core::CalibrationState calB{};
  calB.vcfLeftTrim = 0.4f;
  calB.vcfRightTrim = 1.0f;
  const auto a = renderIdentityWet(kSeed, calA);
  const auto b = renderIdentityWet(kSeed, calB);
  check(vecPeakDiff(a.wetL, b.wetL) > 1e-4,
        "changing the LEFT trim moves the L wet path");
  check(vecBitIdentical(a.wetR, b.wetR),
        "changing the LEFT trim leaves R wet path bit-identical (calibration isolation)");

  // RIGHT-side mirror: changing the RIGHT trim moves only R; L stays bit-identical.
  // This is the in-repo oracle that pins "ignore one-side calibration" on the RIGHT side too.
  core::CalibrationState calC{};
  calC.vcfLeftTrim = 1.0f;
  calC.vcfRightTrim = 1.0f;
  core::CalibrationState calD{};
  calD.vcfLeftTrim = 1.0f;
  calD.vcfRightTrim = 0.4f;
  const auto c = renderIdentityWet(kSeed, calC);
  const auto d = renderIdentityWet(kSeed, calD);
  check(vecPeakDiff(c.wetR, d.wetR) > 1e-4,
        "changing the RIGHT trim moves the R wet path");
  check(vecBitIdentical(c.wetL, d.wetL),
        "changing the RIGHT trim leaves L wet path bit-identical (calibration isolation)");

  // The executed R staging gain must come from the R (NOT the L) domain — a steal-L-to-R
  // mutation sets vcfPathStagingGain(1) == prof.left.pathGain and reds here.
  core::SynthRuntime rt = makeRuntime();
  rt.rebuild();
  rt.configureVcfIdentity(1u, kSeed, calA);
  check(rt.vcfPathStagingGain(1) == prof.right.pathGain,
        "R staging gain is the RIGHT domain (steal-L-to-R would red)");
  check(rt.vcfInputDrive(1) == prof.right.vcfDrive,
        "R VCF drive is the RIGHT domain (steal-L-to-R would red)");
}

// Requirement 4 reproducibility + seed participation — same seed/version/calib over two
// fresh instances AND over block partition is bit-identical; a different seed gives a
// fixed WET difference (the version/seed really participates, not just a gate).
void gh6_bit_identical() {
  core::CalibrationState calib{};
  calib.vcfLeftTrim = 0.9f;
  calib.vcfRightTrim = 1.1f;
  const auto a = renderIdentityWet(kSeed, calib);
  const auto b = renderIdentityWet(kSeed, calib);
  check(vecBitIdentical(a.wetL, b.wetL) && vecBitIdentical(a.wetR, b.wetR),
        "same seed/version/calib -> two fresh instances produce bit-identical WET");

  // Block-partition invariance: a 256-frame processBlock render == 256 per-frame renders.
  constexpr std::size_t kTot = 256;
  static const core::RuntimeInputs kSilence[kTot] = {core::RuntimeInputs{0.0, 0.0}};
  core::RuntimeOutput whole[kTot], part[kTot];
  {
    core::SynthRuntime rt = makeRuntime();
    rt.rebuild();
    rt.configureVcfIdentity(1u, kSeed, calib);
    rt.processBlock(kSilence, kTot, whole, /*driveGraph=*/true);
  }
  {
    core::SynthRuntime rt = makeRuntime();
    rt.rebuild();
    rt.configureVcfIdentity(1u, kSeed, calib);
    for (std::size_t i = 0; i < kTot; ++i) part[i] = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, true);
  }
  check(sameSeq(whole, part, kTot),
        "identity render is block-partition invariant (processBlock == per-frame)");

  // Different seed -> a fixed WET difference (the seed genuinely participates).
  const auto c = renderIdentityWet(kSeed + 1, calib);
  check(vecPeakDiff(a.wetL, c.wetL) > 1e-4 || vecPeakDiff(a.wetR, c.wetR) > 1e-4,
        "different seed -> fixed WET difference (seed participates in the profile)");
}

}  // namespace

int main() {
  std::printf("ENGINE-LEVEL controlled fixture (SynthRuntime/GraphCompiler/executor mechanics).\n");

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
    std::vector<core::RuntimeInputs> zerosRi(kBlock, core::RuntimeInputs{0.0, 0.0});
    std::vector<core::RuntimeOutput> outFull(kBlock), outPart(kBlock);
    whole.processBlock(zerosRi.data(), kBlock, outFull.data(), /*driveGraph=*/true);
    const std::size_t chunk = kBlock / 4;
    for (std::size_t off = 0; off < kBlock; off += chunk)
      part.processBlock(zerosRi.data(), chunk, outPart.data() + off, /*driveGraph=*/true);

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
    for (std::size_t i = 0; i < 100000; ++i) last = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
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
      last = rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
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
    std::vector<core::RuntimeInputs> srcRi(kBlock);
    for (std::size_t i = 0; i < kBlock; ++i) srcRi[i] = core::RuntimeInputs{src[i], src[i]};
    core::SynthRuntime cycA = makeRuntime();
    cycA.connect(kJ_EnvFolOut, kJ_PreampExtIn);
    cycA.rebuild();
    core::SynthRuntime cycB = makeRuntime();  // unpatched: preamp reads ext directly
    cycB.rebuild();
    std::vector<core::RuntimeOutput> outA(kBlock), outB(kBlock);
    cycA.processBlock(srcRi.data(), kBlock, outA.data(), /*driveGraph=*/true);
    cycB.processBlock(srcRi.data(), kBlock, outB.data(), /*driveGraph=*/true);
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
    whole.processBlock(srcRi.data(), kBlock, outFull.data(), true);
    const std::size_t chunks[] = {64, 100, 37, 55};
    std::size_t off = 0, ci = 0;
    while (off < kBlock) {
      const std::size_t n = std::min(chunks[ci % 4], kBlock - off);
      part.processBlock(srcRi.data() + off, n, outPart.data() + off, true);
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
    std::vector<core::RuntimeInputs> quietRi(kBlock, core::RuntimeInputs{0.0, 0.0});
    std::vector<core::RuntimeOutput> outc(kBlock);
    rc.processBlock(quietRi.data(), kBlock, outc.data(), /*driveGraph=*/true);
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
      rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
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
    // MSVC C3493 rejects a block-local constexpr used in an empty-capture `[]` lambda
    // (no default capture mode), while Clang's -Wunused-lambda-capture rejects naming it
    // explicitly ([kN], since a constexpr need not be captured). A default capture is the
    // form both accept, exactly as the S&H lambda below uses.
    auto renderDrone = [&](core::SynthRuntime& rt) {
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) { rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true); seq[i] = rt.droneChannel(0); }
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
    // MSVC C3493: a block-local constexpr in an empty-capture `[]` lambda is rejected
    // (no default capture mode); a default capture is the form both MSVC and Clang accept.
    auto render3 = [&](core::SynthRuntime& rt) {
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) { rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true); seq[i] = rt.drone3Channel(); }
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
      low.processFrame(core::RuntimeInputs{0.0, 0.0}, true);
      refNoise.tick(&n);
      prod[i] = low.drone3Channel();
      src[i] = n;
      high.processFrame(core::RuntimeInputs{0.0, 0.0}, true);
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

  // ---- ⑬ #45: S&H is a CV OUT (not in the audio channel); the lane OWNS its clock ----
  // @Claude criterion ③: the Sample & Hold runs noise->IN with the LF/mod source as its
  // clock and yields a -5..+5 V CV OUT of the voice (manual), so it is NOT summed into
  // the mixer channel. GH#15 D3 re-routes this: the driven S&H clock is now the LF square,
  // edge-count divided by the DIVIDER ratio divN_ (a real lane knob, not an injectable
  // clock). @Kimi ruling ④ voided the setDrone3ShClock field-injection seam, so the old
  // arbitrary-clock-waveform / no-clock ("unclocked doesn't self-run") cases are NO LONGER
  // expressible through the product lane — that is reported back (escape hatch) rather than
  // silently dropped. What the lane CAN prove: the S&H is genuinely clocked by the divided
  // LF (its CV is a stepped nonzero sequence, never inert 0.0), and the CV is a CV OUT that
  // is never summed into *out (a divider change leaves drone3Channel byte-identical while
  // sampleHold3Cv moves).
  {
    constexpr std::size_t kN = 8192;
    auto render3 = [](core::SynthRuntime& rt, double divNorm, std::size_t frames) {
      rt.rebuild();
      rt.setDrone3Rate(60.0);   // fast LF (fixture convenience) so captures are dense in-window.
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_divider, divNorm));
      std::vector<double> ch(frames), cv(frames);
      for (std::size_t i = 0; i < frames; ++i) {
        rt.processFrame(core::RuntimeInputs{0.0, 0.0}, true);
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
    // (i) the lane-clock drives the S&H: DIVIDER=0 (N=1, capture every LF rising edge) over
    // a fast-LF window gives a stepped, bounded nonzero CV — never the inert 0.0 of the old
    // no-clock path.
    {
      core::SynthRuntime rt = makeRuntime();
      auto [ch, cv] = render3(rt, 0.0, kN);
      static_cast<void>(ch);
      double cvPeak = 0.0;
      for (double x : cv) cvPeak = std::max(cvPeak, std::fabs(x));
      check(cvPeak > 1e-3,
            "S&H is clocked by the lane (sampleHold3Cv is a bounded nonzero level)");
      const std::size_t steps = countSteps(cv);
      check(steps >= 4,
            "clocked S&H steps at the divided-LF clock edges (sampleHold3Cv changes)");
    }
    // (ii) S&H CV is a CV OUT, never summed into *out: two divider ratios give two distinct
    // CV profiles (different division rates -> different captures) but byte-identical channels.
    {
      core::SynthRuntime rtA = makeRuntime();
      auto a = render3(rtA, 0.0, kN);   // N = 1 -> capture every LF edge.
      core::SynthRuntime rtB = makeRuntime();
      auto b = render3(rtB, 1.0, kN);   // N = 16 -> capture every 16th LF edge.
      check(peakDiff(a.first, b.first) < 1e-12,
            "S&H is NOT in the audio channel (divider change leaves drone3Channel byte-identical)");
      check(peakDiff(a.second, b.second) > 1e-3,
            "divider change alters the S&H CV readback (real lever, non-vacuous)");
    }
  }

  // ---- #46: ControlEvent dispatch consumes EventTimebase (buffer-invariant) ----
  std::printf("(46) ControlEvent dispatch is buffer-invariant + non-vacuous\n");
  {
    constexpr std::size_t kTotFrames = 256;
    constexpr std::size_t kEventSample = 100;   // absolute sample the pitch lands on.
    constexpr std::size_t kBlocks[3] = {64, 128, 256};
    static const core::RuntimeInputs kSilence[kTotFrames] = {core::RuntimeInputs{0.0, 0.0}};  // extSource = 0, as runFrames() uses.

    // A single drone_3.pitch = 0.5 event at absolute sample kEventSample. The runtime's
    // default pitch is silence, so a nonzero pitch turns the tone on THERE.
    auto pitchEvent = [&]() {
      core::ControlEvent ev;
      ev.kind = core::ControlEventKind::parameter;
      ev.parameter = core::ParameterId::drone_3_pitch;
      ev.value = 0.5;
      ev.source = 1;
      ev.producerSequence = 1;
      core::TimedControlEvent te;
      te.event = ev;
      te.sample = kEventSample;
      return te;
    };

    // Render kTotFrames in `block`-frame chunks and write them into `seq`.
    auto blockRender = [&](core::SynthRuntime& rt, std::size_t block, core::RuntimeOutput* seq) {
      for (std::size_t b = 0; b < kTotFrames; b += block) rt.processBlock(kSilence, block, seq + b);
    };

    // Every render must go through rebuild() so the fixed chain actually executes
    // (makeRuntime() binds roles and the chain order is derived on rebuild; without
    // it the outputs stay at 0, which would make any comparison vacuous).
    auto makeRunning = [&]() { core::SynthRuntime rt = makeRuntime(); rt.rebuild(); return rt; };

    // Baseline: no event — must itself be buffer-invariant.
    core::RuntimeOutput base[3][kTotFrames] = {};
    for (int bi = 0; bi < 3; ++bi) {
      core::SynthRuntime rt = makeRunning();
      blockRender(rt, kBlocks[bi], base[bi]);
    }
    check(sameSeq(base[0], base[1], kTotFrames) && sameSeq(base[1], base[2], kTotFrames),
          "no-event baseline output is buffer-invariant");

    // Admission accepts the driven event (once).
    {
      core::SynthRuntime scratch = makeRunning();
      check(scratch.enqueueControlEvent(pitchEvent()),
            "enqueueControlEvent accepts a drone_3.pitch timed control event");
    }

    // Scripted: one pitch event; the block partition (64/128/256) must not move the
    // frame it fires at (buffer-invariance, criterion ④), AND it must change the output
    // vs the empty script (not vacuous — "component present != machine uses it").
    core::RuntimeOutput ev[3][kTotFrames] = {};
    for (int bi = 0; bi < 3; ++bi) {
      core::SynthRuntime rt = makeRunning();
      static_cast<void>(rt.enqueueControlEvent(pitchEvent()));
      blockRender(rt, kBlocks[bi], ev[bi]);
    }
    check(sameSeq(ev[0], ev[1], kTotFrames) && sameSeq(ev[1], ev[2], kTotFrames),
          "scripted output is buffer-invariant across 64/128/256 (event acts at the SAME sample)");
    check(!sameSeq(ev[0], base[0], kTotFrames),
          "event script differs from the empty script (dispatch is exercised, not vacuous)");
  }

  // ---- GH#15 D1: MOD knob (drone_3/6.mod) reaches the audio DSP ------------------
  // The mod knobs were in the 16 no-consumer set; D1 wires them into BOTH dispatch
  // lanes (applyDspParam batch + applyControlEvent_ live) -> the PapaVoice mod_ field,
  // which tick() scales onto the LF square feeding the audio oscillator (the OLD code
  // fed a raw ±1 square, i.e. depth 1.0; the NEW default is the registry 0.5).
  // Acceptance is non-vacuous per @Kimi's default-change contract (35e5328b):
  //   (c) a FRESH runtime reports the registry default 0.5 on both drones;
  //   (a) the batch lane routes a non-default value into the real field (getter);
  //   (a') norm->depth follows the declared linear kModDepthFromNorm;
  //   (b) the live ControlEvent lane routes into the same field;
  //   (d) mod is a REAL audio lever — with FM on, depth 1.0 vs 0.5 change
  //       drone3Channel() (the OLD behavior vs the NEW default).
  // NEGATIVE (source mutations the fixture above discriminates): dropping `* mod_` in
  // PapaVoice::tick collapses (d) 1.0 vs 0.5 onto one stream -> red; zeroing the
  // kModDepthFromNorm scale makes (a)/(a')/b) applied values 0 instead of 0.8/1.0 -> red.
  {
    auto render3 = [](core::SynthRuntime& rt) {
      constexpr std::size_t kN = 1024;
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) {
        rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
        seq[i] = rt.drone3Channel();
      }
      return seq;
    };
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };

    // (c) default. Fresh runtime never touched -> registry default 0.5 on BOTH drones.
    // This pins the POST-wire default (was implicitly depth 1.0 via the raw ±1 square).
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      check(rt.drone3ModApplied() == 0.5, "d3 MOD default depth is 0.5 (registry default)");
      check(rt.drone6ModApplied() == 0.5, "d6 MOD default depth is 0.5 (registry default)");
    }
    // (a) batch lane, non-vacuous. 0.8 != default 0.5, so a missed dispatch case stays
    // 0.5 and reds; a zeroed kModDepthFromNorm applies 0.0 and reds.
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      const auto s3 = rt.applyDspParam(core::ParameterId::drone_3_mod, 0.8);
      check(s3 == core::ParameterApplyStatus::applied, "d3 MOD batch apply is 'applied'");
      check(rt.drone3ModApplied() == 0.8, "d3 MOD batch lane reaches the audio field (0.8)");
      const auto s6 = rt.applyDspParam(core::ParameterId::drone_6_mod, 0.8);
      check(s6 == core::ParameterApplyStatus::applied, "d6 MOD batch apply is 'applied'");
      check(rt.drone6ModApplied() == 0.8, "d6 MOD batch lane reaches the audio field (0.8)");
    }
    // (a') linear mapping: norm 1.0 -> depth kModDepthFromNorm (== 1.0), pinning the
    // declared norm->depth relationship rather than an undocumented scale.
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_mod, 1.0));
      check(rt.drone3ModApplied() == 1.0, "d3 MOD norm=1.0 maps to depth 1.0 (kModDepthFromNorm)");
    }
    // (b) live lane, non-vacuous. A scheduled parameter ControlEvent must reach the SAME
    // field via applyControlEvent_. te.sample=0 fires at frame 0, before the frame ticks.
    {
      constexpr std::size_t kTot = 128;
      static const core::RuntimeInputs kSil[kTot] = {};
      core::RuntimeOutput out[kTot] = {};
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      core::ControlEvent ev;
      ev.kind = core::ControlEventKind::parameter;
      ev.parameter = core::ParameterId::drone_3_mod;
      ev.value = static_cast<core::SignalSample>(0.8);
      ev.source = 1;
      ev.producerSequence = 1;
      core::TimedControlEvent te;
      te.event = ev;
      te.sample = 0;
      check(rt.enqueueControlEvent(te), "d3 MOD live ControlEvent is admitted");
      rt.processBlock(kSil, kTot, out, /*driveGraph=*/true);
      // SignalSample is float, so the panel value 0.8 lands as 0.8000000119 — compare
      // with a tolerance (the batch lane, which passes an exact double 0.8, stays exact).
      check(std::fabs(rt.drone3ModApplied() - 0.8) < 1e-5,
            "d3 MOD live lane reaches the audio field (0.8)");
    }
    // (d) mod is a REAL audio lever. FM is ON (fmDevHz=120 non-inert), so scaling the
    // mod depth 1.0 vs 0.5 changes the FM swing on drone3Channel(). Fresh same-seed
    // runtimes cancel the noise stem, so a peak diff > 0 is the audio difference only.
    {
      const auto modDepth = [&](double depth) {
        core::SynthRuntime rt = makeRuntime(); rt.rebuild();
        rt.setDrone3Fm(true);
        rt.setDrone3Mod(depth);
        return render3(rt);
      };
      const auto d10 = modDepth(1.0);  // OLD behavior: unconditional full depth.
      const auto d05 = modDepth(0.5);  // NEW default: registry-consistent half depth.
      check(peakDiff(d10, d05) > 1e-5,
            "d3 MOD depth 1.0 vs 0.5 change the drone channel (mod is a real audio lever)");
    }
  }

  // ---- GH#15 D2: RANGE / RATE-SWITCH selectors (drone_3/6.hi_low + rate_switch) ----
  // The hi_low (RANGE) and rate_switch (RATE SWITCH) selectors were the last four D2
  // no-consumer params. D2 wires them into BOTH dispatch lanes (applyDspParam batch +
  // applyControlEvent_ live) onto the audio/LF oscillator as a COMPOSE onto the already-
  // wired PITCH+RANGE / RATE, not a replace:
  //   * hi_low (labels [hi,low], idx0=hi): setRangeHiLow shades the audio pitch band by
  //     rangeBaseSt_ (-24 st at low; 0 at hi) ON TOP of the wired PITCH knob, so
  //     pitchHz(low) = pitchHz(hi) * 2^(-24/12) = pitchHz(hi) * 0.25.
  //   * rate_switch (labels [off,on], idx0=off): setRateSwitch multiplies the LF rate by
  //     rateMult_ (2 at on; 1 at off) ON TOP of the wired RATE knob, so rateHz(on) =
  //     rateHz(off) * 2.
  // Both default to position 0 (hi / off), which yields 0 offset / ×1 — the selectors are
  // BIT-IDENTICAL to the pre-D2 oscillator at default, so (unlike D1's mod, whose default
  // depth moved 1.0→0.5) there is NO default-behaviour change and no before/after evidence.
  // Acceptance (per @Kimi D2 clause): each position proves it changes the executed module:
  //   (a) batch lane position 0 vs 1 on the REAL getter — hi_low pitchHz ratio 0.25,
  //       rate_switch rateHz ratio 2.0;
  //   (b) live lane ControlEvent reaches the same getter;
  //   (c) render lever (audio) — hi_low: FM OFF, channel 0 vs 1 differ; rate_switch:
  //       FM ON (the LF only reaches the audio through the mod swing), channel 0 vs 1 differ;
  //   (d) out-of-range lock — 2.0 / 0.5 / -1.0 are invalid_value and leave state unchanged.
  // NEGATIVE (source mutations the fixture discriminates): hard-coding rangeBaseSt_=0 /
  // rateMult_=1 (ignoring the selector) collapses the (a)/(b) two-position ratio onto one
  // stream -> red; dropping the selector->osc feeding collapses (c) -> red.
  {
    auto render3n = [](core::SynthRuntime& rt, std::size_t kN) {
      std::vector<double> seq(kN);
      for (std::size_t i = 0; i < kN; ++i) {
        rt.processFrame(core::RuntimeInputs{0.0, 0.0}, /*driveGraph=*/true);
        seq[i] = rt.drone3Channel();
      }
      return seq;
    };
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };
    constexpr double kRangeRatio = 0.25;   // 2^(-24/12) — the PROVISIONAL low-band shift.
    constexpr double kRateRatio = 2.0;     // kNewDroneRateSwitchMult — the ON multiplier.

    // (a) batch lane, two positions on the REAL getter. Position 0 is the identity default,
    // positioning 1 must differ (and by the exact declared ratio). Both drones.
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      const double p0 = rt.drone3PitchHz();   // hi_low=0 (hi, default): base band.
      check(p0 > 0.0, "d3 hi_low base pitch is audible (>0, real tone)");
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_hi_low, 1.0));
      const double p1 = rt.drone3PitchHz();
      check(std::fabs(p1 / p0 - kRangeRatio) < 1e-3,
            "d3 hi_low low position drops the pitch band by 2 oct (pitchHz ratio 0.25)");
      const double r0 = rt.drone3RateHz();    // rate_switch=0 (off, default): ×1.
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_rate_switch, 1.0));
      const double r1 = rt.drone3RateHz();
      check(std::fabs(r1 / r0 - kRateRatio) < 1e-6,
            "d3 rate_switch on position doubles the LF rate (rateHz ratio 2.0)");

      core::SynthRuntime rt6 = makeRuntime(); rt6.rebuild();
      const double q0 = rt6.drone6PitchHz();
      static_cast<void>(rt6.applyDspParam(core::ParameterId::drone_6_hi_low, 1.0));
      check(std::fabs(rt6.drone6PitchHz() / q0 - kRangeRatio) < 1e-3,
            "d6 hi_low low position drops the pitch band by 2 oct (pitchHz ratio 0.25)");
      const double s0 = rt6.drone6RateHz();
      static_cast<void>(rt6.applyDspParam(core::ParameterId::drone_6_rate_switch, 1.0));
      check(std::fabs(rt6.drone6RateHz() / s0 - kRateRatio) < 1e-6,
            "d6 rate_switch on position doubles the LF rate (rateHz ratio 2.0)");
    }
    // (b) live lane, non-vacuous. A scheduled parameter ControlEvent must reach the SAME
    // getter via applyControlEvent_. te.sample=0 fires at frame 0, before the frame ticks.
    {
      constexpr std::size_t kTot = 128;
      static const core::RuntimeInputs kSil[kTot] = {};
      core::RuntimeOutput out[kTot] = {};
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      const double p0 = rt.drone3PitchHz();
      core::ControlEvent ev;
      ev.kind = core::ControlEventKind::parameter;
      ev.parameter = core::ParameterId::drone_3_hi_low;
      ev.value = static_cast<core::SignalSample>(1.0);
      ev.source = 1;
      ev.producerSequence = 1;
      core::TimedControlEvent te;
      te.event = ev;
      te.sample = 0;
      check(rt.enqueueControlEvent(te), "d3 hi_low live ControlEvent is admitted");
      rt.processBlock(kSil, kTot, out, /*driveGraph=*/true);
      check(std::fabs(rt.drone3PitchHz() / p0 - kRangeRatio) < 1e-3,
            "d3 hi_low live lane reaches the pitch field (2-oct drop)");
    }
    // (c) hi_low is a REAL audio lever. FM OFF (default): the audio tone itself shifts band,
    // so position 0 vs 1 differ on drone3Channel() immediately.
    {
      const auto leverHi = [&](int sel) {
        core::SynthRuntime rt = makeRuntime(); rt.rebuild();
        rt.setDrone3HiLow(sel);   // 0 = hi (default), 1 = low.
        return render3n(rt, 1024);
      };
      check(peakDiff(leverHi(0), leverHi(1)) > 1e-5,
            "d3 hi_low changes the drone channel (RANGE is a real audio lever)");
    }
    // (c') rate_switch is a REAL audio lever. FM ON (the LF only reaches the audio via the
    // mod swing): rate_switch ON doubles the LF rate, so the mod square flips earlier than
    // the OFF (×1) run. A long enough window captures that flipped-mod difference.
    {
      const auto leverRate = [&](int sel) {
        core::SynthRuntime rt = makeRuntime(); rt.rebuild();
        rt.setDrone3Fm(true);
        rt.setDrone3RateSwitch(sel);   // 0 = off (default, ×1), 1 = on (×2).
        return render3n(rt, 8192);     // > 83 ms so the ON run's first flip is in-window.
      };
      check(peakDiff(leverRate(0), leverRate(1)) > 1e-5,
            "d3 rate_switch changes the drone channel (RATE-SWITCH is a real audio lever)");
    }
    // (d) out-of-range lock. The batch lane validates selectors via dspParamValid_ (exact
    // integer in [min,max]); 2.0 / 0.5 / -1.0 are all rejected as invalid_value and the
    // readback stays at the last VALID value (fail-closed, never a silent clamp).
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_hi_low, 1.0));
      const double held = rt.drone3PitchHz();
      check(rt.applyDspParam(core::ParameterId::drone_3_hi_low, 2.0) ==
                core::ParameterApplyStatus::invalid_value,
            "d3 hi_low 2.0 is rejected as invalid_value");
      check(rt.drone3PitchHz() == held, "d3 hi_low out-of-range leaves the state unchanged");
      check(rt.applyDspParam(core::ParameterId::drone_3_hi_low, 0.5) ==
                core::ParameterApplyStatus::invalid_value,
            "d3 hi_low 0.5 is rejected as invalid_value (non-integer)");
      check(rt.drone3PitchHz() == held, "d3 hi_low 0.5 leaves the state unchanged");
      const double heldRate = rt.drone3RateHz();
      check(rt.applyDspParam(core::ParameterId::drone_3_rate_switch, -1.0) ==
                core::ParameterApplyStatus::invalid_value,
            "d3 rate_switch -1.0 is rejected as invalid_value");
      check(rt.drone3RateHz() == heldRate,
            "d3 rate_switch out-of-range leaves the state unchanged");
    }
  }

  // ---- GH#15 D3: DIVIDER knob (drone_3/6.divider) owns the S&H clock division ----
  // The divider was the last D3 no-consumer param beside the S&H clock. @Kimi ruling ④
  // gave the divided-LF lane OWNERSHIP of the S&H clock source (shClock_ is derived in
  // tick() from the LF square, edge-count divided by divN_); the old setShClock injection
  // seam is voided, so no invented dual-source priority rule is introduced. D3 wires
  // drone_3/6_divider into BOTH dispatch lanes (applyDspParam batch + applyControlEvent_
  // live) -> PapaVoice::setDivider, which sets divN_ = 1 + (kNewDroneDivMax-1)*norm
  // (linear; the max is PROVISIONAL — a software model, no manual/DSP evidence).
  // Acceptance (per @Kimi D3 clause): each divider value proves it changes the executed
  // module — the S&H clock — via its CV-out readback, and does NOT touch the audio:
  //   (a) batch lane two norms on the REAL getter (drone3Divider(), closed form 1+15*n);
  //   (b) live lane ControlEvent reaches the same getter;
  //   (c) render lever — changing the divider leaves drone3Channel byte-identical (the
  //       S&H CV is never summed into *out) but moves sampleHold3Cv (a real lever);
  //   (d) out-of-range lock — norm outside [0,1] is invalid_value and leaves state unchanged.
  // NEGATIVE (source mutations the fixture discriminates): summing shCv_ into *out
  // collapses (c) byte-identical onto two differing channels -> red; forcing divN_ = 1
  // (ignoring the norm) collapses (a)/(b) onto the default ratio -> red.
  {
    auto peakDiff = [](const std::vector<double>& a, const std::vector<double>& b) {
      double d = 0.0;
      for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        d = std::max(d, std::fabs(a[i] - b[i]));
      return d;
    };
    constexpr double kDivSpan = 16.0 - 1.0;   // kNewDroneDivMax-1 (PROVISIONAL max).

    // (a) batch lane, two norms on the REAL getter. Closed form divN_ = 1 + 15*norm, so
    // a mapping error (wrong max, or a nonlinear norm) is a red here. Both drones.
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_divider, 0.25));
      const double n25 = rt.drone3Divider();
      check(std::fabs(n25 - (1.0 + kDivSpan * 0.25)) < 1e-9,
            "d3 divider norm=0.25 -> divN=1+(16-1)*0.25");
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_divider, 0.75));
      const double n75 = rt.drone3Divider();
      check(std::fabs(n75 - (1.0 + kDivSpan * 0.75)) < 1e-9,
            "d3 divider norm=0.75 -> divN=1+(16-1)*0.75");
      check(std::fabs(n75 - n25 - kDivSpan * 0.5) < 1e-9,
            "d3 divider is linear in norm (0.25 -> 0.75 gap = 15*0.5)");
      core::SynthRuntime rt6 = makeRuntime(); rt6.rebuild();
      static_cast<void>(rt6.applyDspParam(core::ParameterId::drone_6_divider, 0.75));
      check(std::fabs(rt6.drone6Divider() - (1.0 + kDivSpan * 0.75)) < 1e-9,
            "d6 divider batch lane reaches the field (1+(16-1)*0.75)");
    }
    // (b) live lane, non-vacuous. A scheduled parameter ControlEvent must reach the SAME
    // getter via applyControlEvent_. te.sample=0 fires at frame 0, before the frame ticks.
    {
      constexpr std::size_t kTot = 128;
      static const core::RuntimeInputs kSil[kTot] = {};
      core::RuntimeOutput out[kTot] = {};
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      core::ControlEvent ev;
      ev.kind = core::ControlEventKind::parameter;
      ev.parameter = core::ParameterId::drone_3_divider;
      ev.value = static_cast<core::SignalSample>(0.25);
      ev.source = 1;
      ev.producerSequence = 1;
      core::TimedControlEvent te;
      te.event = ev;
      te.sample = 0;
      check(rt.enqueueControlEvent(te), "d3 divider live ControlEvent is admitted");
      rt.processBlock(kSil, kTot, out, /*driveGraph=*/true);
      check(std::fabs(rt.drone3Divider() - (1.0 + kDivSpan * 0.25)) < 1e-9,
            "d3 divider live lane reaches the field (1+(16-1)*0.25)");
    }
    // (c) render lever. The S&H is a CV OUT, never summed into *out, so a divider change
    // leaves drone3Channel byte-identical while sampleHold3Cv moves (fast-LF window).
    {
      const auto renderDiv = [&](core::SynthRuntime& rt, double norm, std::size_t kN) {
        rt.rebuild();
        rt.setDrone3Rate(60.0);   // fast LF (fixture) so captures are dense in-window.
        static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_divider, norm));
        std::vector<double> ch(kN), cv(kN);
        for (std::size_t i = 0; i < kN; ++i) {
          rt.processFrame(core::RuntimeInputs{0.0, 0.0}, true);
          ch[i] = rt.drone3Channel();
          cv[i] = rt.sampleHold3Cv();
        }
        return std::make_pair(ch, cv);
      };
      constexpr std::size_t kN = 8192;
      core::SynthRuntime rtA = makeRuntime(); auto a = renderDiv(rtA, 0.0, kN);   // N=1.
      core::SynthRuntime rtB = makeRuntime(); auto b = renderDiv(rtB, 1.0, kN);   // N=16.
      check(peakDiff(a.first, b.first) < 1e-12,
            "d3 divider change leaves drone3Channel byte-identical (S&H NOT in audio)");
      check(peakDiff(a.second, b.second) > 1e-3,
            "d3 divider change alters the S&H CV readback (real lever, non-vacuous)");
    }
    // (d) out-of-range lock. The batch lane validates the continuous unit domain via
    // dspParamValid_ (v in [min,max]); 2.0 / -0.5 are outside [0,1] -> invalid_value and
    // the readback stays at the last valid value (fail-closed, never a silent clamp).
    {
      core::SynthRuntime rt = makeRuntime(); rt.rebuild();
      static_cast<void>(rt.applyDspParam(core::ParameterId::drone_3_divider, 0.25));
      const double held = rt.drone3Divider();
      check(rt.applyDspParam(core::ParameterId::drone_3_divider, 2.0) ==
                core::ParameterApplyStatus::invalid_value,
            "d3 divider 2.0 is rejected as invalid_value (norm > 1)");
      check(rt.drone3Divider() == held, "d3 divider 2.0 leaves the state unchanged");
      check(rt.applyDspParam(core::ParameterId::drone_3_divider, -0.5) ==
                core::ParameterApplyStatus::invalid_value,
            "d3 divider -0.5 is rejected as invalid_value (norm < 0)");
      check(rt.drone3Divider() == held, "d3 divider -0.5 leaves the state unchanged");
    }
  }

  std::printf("(11) GH#13 feedback capacity — registry 18 self-loops\n");
  registry_self_loop_feedback_capacity();

  std::printf("(12) GH#5 classic drone group gate/env — product path (batch 4A)\n");
  registry_drone_gate_envout();

  std::printf("(13) GH#5 classic drone shared CV MOD — joined control consumed\n");
  registry_drone_cv_mod();

  std::printf("(14) GH#5 classic drone same-seed reproducibility\n");
  registry_drone_reproducible();

  std::printf("(14b) GH#11 no execution-kind dedup — four classic drone slots\n");
  registry_drone_no_dedup_slots();

  std::printf("(14c) GH#11 explicit strict binding policy — distinct fail-closed statuses\n");
  registry_strict_binding_policy();

  std::printf("(14d) GH#11 strict preflight judges ACTIVE edge-endpoints, not inventory presence\n");
  registry_strict_active_only();

  std::printf("(14e) GH#11 bindExecutionKind dirties the plan on add + update\n");
  registry_strict_binding_kind_dirty();

  std::printf("(15) GH#5 classic drone ENV OUT fail-closed\n");
  registry_drone_envout_fail_closed();

  std::printf("(16) GH#5 classic drone ENV OUT block-partition invariance\n");
  registry_drone_envout_partition();

  std::printf("(17) GH#5 classic drone JackId{0} sentinel removed (legal id 0 cohort)\n");
  registry_drone_envout_id0_sentinel();

  std::printf("(18) GH#6 VCF identity / calibration config entry on the product runtime\n");
  gh6_config_entry();

  std::printf("(19) GH#6 fail-closed version / trim admission (keep old complete profile)\n");
  gh6_fail_closed();

  std::printf("(20) GH#6 L/R calibration + profile domain isolation\n");
  gh6_lr_isolation();

  std::printf("(21) GH#6 reproducibility + seed participation (bit-identical, partition-invariant)\n");
  gh6_bit_identical();

  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
