// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit identity profile — GH#6: the central, versioned micro-difference source of
// truth for "this same-built machine's" VCF / distortion path. Design/07 §7:
//
//   "首次运行生成并持久保存 UnitIdentitySeed，确定 20 个 classic drone
//    oscillator、左右 VCF/distortion 与 gain calibration 的固定微差。不得依赖
//    未规定算法的 std::random；保存 identityModelVersion 和派生 calibration
//    profile，使软件升级不会悄悄把同一台机器'重抽一次'。"
//   "VCF→post-filter distortion→gain staging 作为整条 level-dependent path
//    校准；左右拓扑可共用代码，但 calibration/nonlinear state 独立。"
//
// This header is the small, unique, fixed/no-heap truth source. It consumes ONLY
// the existing UnitIdentitySeed + identityModelVersion primitive values (the
// DeviceStateV1 triple is consumed by the SynthRuntime config entry, which calls
// into this header after validating the calibration fields). It never persists a
// second copy of the derived profile and never touches the DeviceState schema.
//
// WHAT IT DERIVES (per physical L/R domain — the domains are never shared):
//
//   * vcfDrive   — VCF input-stage saturation strength (the input-level driven
//                  nonlinearity of design/07 §7). Small-signal effect is a unit
//                  slope; high input folds toward a lower normalised gain, so the
//                  LEFT and RIGHT channels saturate independently/by a micro.
//   * distDrive  — post-filter Distortion folding-drive micro-difference (L/R).
//   * pathGain   — near-unity path-gain micro-difference, applied ONCE at the
//                  VCF→distortion staging point (before the post-filter fold).
//
// PROVISIONAL DISPOSITION (design/07 §7, and @Codex's GH#6 boundary): every range,
// rail, drive and trim value below is UN-EVIDENCED (no manual / measured figure
// exists). These are deliberately provisional modeling constants that pin trends
// only — a small L/R micro-difference, a level-dependent input fold, a near-unity
// staging gain — NOT a claimed real-machine calibration. Each is named and
// centralised here so the product path and every test reference one definition.
//
// DETERMINISM: the derivation is a pure function of (seed, version, side). It uses
// the project's deterministic SeededRandom (splitmix64), NEVER std::random, so the
// same seed/version yields a bit-identical profile on every toolchain here, and the
// L/R domains draw from domain-salted (non-shared) streams. The per-side range map
// goes through an explicit single-rounded std::fma (detail::mapRange), so the
// derived doubles do NOT depend on whether an optimizing compiler contracts
// `lo + unit*(hi-lo)` into an FMA (fast contraction) or keeps the multiply+add
// separate (two roundings): one explicit rounding is the unique result on every
// toolchain in this project's CI. NOTE: this is only a claim about this code forcing
// a single rounding choice — not that all conceivable floating-point environments
// round every expression identically (that is not true in general).
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

// The single identity-model version this build understands. Design/07 §7 keeps the
// version so a software upgrade cannot silently "re-roll" a machine's identity;
// only v1 is implemented. Unknown versions must be rejected by the caller (the
// SynthRuntime config entry) — this header never fabricates a half profile for one.
inline constexpr std::uint32_t kIdentityModelVersionSupported = 1u;

// L/R physical domain discriminator. The two domains are INDEPENDENT: each draws
// from its own domain-salted stream and the derived profile is NOT shared (the
// calibration/nonlinear-state-independence requirement of design/07 §7).
enum class IdentitySide : std::uint8_t {
  kLeft = 0,
  kRight = 1,
};

// Fixed per-side profile. All values are PROVISIONAL (see file header).
struct SideIdentityProfile {
  // VCF input-stage saturation strength (the "a" in a). >0 enables the fold;
  // a larger value folds the L/R input earlier/stronger.
  double vcfDrive = 0.0;
  // Extra post-filter Distortion folding drive, expressed as a fraction added to
  // 1.0 (driveOffset) — the per-side micro-difference in fold strength.
  double distDrive = 0.0;
  // Near-unity path-gain micro-difference, applied once at VCF→distortion staging.
  double pathGain = 1.0;
};

// The whole per-side profile. NOTE: this carries NO validity flag — the caller
// ensures the version is supported before deriving, and only then applies the
// COMPLETE profile atomically. A bad version is rejected up front, so this struct
// never represents a half-derived state.
struct VcfIdentityProfile {
  SideIdentityProfile left;
  SideIdentityProfile right;
};

// ---------------------------------------------------------------------------
// PROVISIONAL profile ranges (no manual number exists; see file header).
// ---------------------------------------------------------------------------

// VCF input-stage drive range. Chosen so small-signal slope stays 1 and a high
// input (normalised ~3 V) folds the fundamental well below unity, so the
// amplitude-sweep oracle sees the L/R level difference.
inline constexpr double kProfileVcfDriveMin = 0.6;
inline constexpr double kProfileVcfDriveMax = 1.0;

// Distortion folding-drive offset (added to kDriveFold): the L/R micro-difference.
inline constexpr double kProfileDistDriveMin = 0.0;
inline constexpr double kProfileDistDriveMax = 0.6;

// Near-unity path-gain micro difference (±2%). Applied once at staging.
inline constexpr double kProfilePathGainSpread = 0.02;

// How strongly a side's distDrive raises its Distortion rail. PROVISIONAL.
inline constexpr double kDistRailStiffness = 0.5;

// Supports only the known v1 identity model. Unknown versions are rejected by the
// caller (fail-closed), never derived here.
inline bool isSupportedIdentityVersion(std::uint32_t version) {
  return version == kIdentityModelVersionSupported;
}

namespace detail {

// Deterministic range-map of a unit value (in [0,1)) into [lo, hi). It forces a
// single explicit rounding via std::fma, so the result is the unique value
// regardless of whether a compiler fuses `lo + unit*(hi-lo)` into one FMA (fast
// contraction) or keeps two roundings — the two-step form is NOT integer/bit
// stable across toolchains, so we never depend on the compiler's choice. This is
// the single place all three profile fields are mapped into their ranges.
inline double mapRange(double unit, double lo, double hi) {
  return std::fma(unit, hi - lo, lo);
}

// Deterministic bit-mix of (seed, version, side): different version / side inject
// a different stream, so the L/R domains never collide and version really
// participates (not just the gate below).
inline std::uint64_t mixIdentityInput(std::uint64_t seed, std::uint32_t version,
                                      IdentitySide side) {
  std::uint64_t x = seed ^ (0x9E3779B97F4A7C15ULL * (version + 1u));
  x ^= (side == IdentitySide::kLeft) ? 0x243F6A8885A308D3ULL : 0x13198A2E03707344ULL;
  // splitmix64 finaliser (same family SeededRandom uses).
  x = (x ^ (x >> 33)) * 0xFF51AFD7ED558CCDULL;
  x = (x ^ (x >> 33)) * 0xC4CEB9FE1A85EC53ULL;
  x ^= (x >> 33);
  return x;
}

inline SideIdentityProfile deriveSideProfile(std::uint64_t seed, std::uint32_t version,
                                             IdentitySide side) {
  SeededRandom rng(mixIdentityInput(seed, version, side));
  SideIdentityProfile p;
  // vcfDrive: input-stage saturation strength.
  p.vcfDrive = mapRange(rng.nextUnit(), kProfileVcfDriveMin, kProfileVcfDriveMax);
  // distDrive: extra post-filter distortion fold offset.
  p.distDrive = mapRange(rng.nextUnit(), kProfileDistDriveMin, kProfileDistDriveMax);
  // pathGain: near-unity, asymmetric around 1.0.
  p.pathGain = 1.0 + mapRange(rng.nextUnit(), -kProfilePathGainSpread, kProfilePathGainSpread);
  return p;
}

}  // namespace detail

// Derive the complete per-L/R profile for (seed, version). Callers MUST have
// verified isSupportedIdentityVersion(version) first; this function always
// returns a consistent, deterministic profile (v1 supported path only).
inline VcfIdentityProfile deriveVcfIdentityProfile(std::uint64_t seed,
                                                   std::uint32_t version) {
  VcfIdentityProfile p;
  p.left = detail::deriveSideProfile(seed, version, IdentitySide::kLeft);
  p.right = detail::deriveSideProfile(seed, version, IdentitySide::kRight);
  return p;
}

}  // namespace lunar24::core
