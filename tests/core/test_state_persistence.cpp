// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-⑤ Half 2 tests for state persistence (design/07 §6, master plan line 93).
// The control thread takes a consistent snapshot, debounces it (coalescing rapid
// changes, never dropping the final state), writes a TEMP file, flushes it, then
// atomically renames it over the live file. The audio thread never participates
// in disk saving.
//
// Four must-tests, each with a red-negative control (@Claude: "用不会暴露错误的
// 输入去测，等于没测"):
//   1. atomic-replace never exposes a half file (a failed temp/rename leaves the
//      live file untouched & loadable); negative = in-place direct write.
//   2. debounce throttles but never loses the last (real writes < changes AND the
//      final state persists); negatives = no throttle, and a tail-dropping debounce.
//   3. round-trip fidelity (encode -> decode field-identical + topology identical);
//      negative = a field omitted in encode breaks the round trip.
//   4. persistence never on the audio thread (OutDir save outside the RT window is
//      clean; inside it is caught by the RtGuard file detector).
//
// Honest boundary: temp->flush->rename ORDER and interruption visibility are what
// these tests prove. True power-off durability (fsync) is NOT — see the header
// and FINDINGS.md; this suite never asserts "crash-safe".

#include "mini_test.h"
#include "rt_guard_test.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <lunar24/core/device_state.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/state_persistence.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;
namespace reg = lunar24::registry;

// ---------------------------------------------------------------- helpers ----

// Distinctive, exact-round-trip value pattern for a whole state. Reserved bytes
// (the keyboard-preset pad and the sequencer block) are filled with a NON-ZERO,
// NON-0xFF pattern so a serializer that silently clears them is caught (@Claude
// Q2: reserved bytes are preserved, never destroyed).
static void fill_state(core::DeviceStateV1& s) {
  s.schemaVersion = 17u;
  s.identityModelVersion = 3u;
  s.identitySeed.seed = 0xA1B2C3D4E5F60718ULL;
  s.calibration.vcfLeftTrim = 0.5f;
  s.calibration.vcfRightTrim = 1.25f;
  for (std::uint32_t i = 0; i < core::kDeviceParamCapacity; ++i)
    s.parameters[i] = static_cast<double>(i) * 0.5;

  // Two real user cables (source = output jack, sink = input jack).
  s.inputCable[1u] = 1u;
  s.cableSource[1u] = core::JackId{31u};  // joystick_x_out -> vco_a_v_oct_in
  s.inputCable[0u] = 1u;
  s.cableSource[0u] = core::JackId{29u};  // lfo_a_cv_out -> vco_a_cv_in

  s.routeOverridden[5u] = 1u;

  for (std::uint32_t k = 0; k < core::kDeviceKeyboardPresetCount; ++k) {
    core::KeyboardPreset& p = s.keyboardPresets[k];
    p.id = 100u + k;
    p.pressureBehaviour = 2u;
    p.pressureOutput = 1u;
    p.reserved[0] = 0x5Au;  // distinctive non-zero, non-0xFF pattern
    p.reserved[1] = 0xA5u;
  }
  s.keyboardSettings.pressureBehaviour = 7u;
  s.keyboardSettings.pressureOutput = 9u;
  s.leftEffector.program = core::ProgramId{0x010203u};
  s.rightEffector.program = core::ProgramId{0x040506u};
  for (std::uint32_t i = 0; i < core::kSequencerPhysicalBytes; ++i)
    s.sequencer.reserved[i] = static_cast<std::uint8_t>(0x80u + i);
}

static std::uint32_t fbits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4u);
  return u;
}
static std::uint64_t dbits(double d) {
  std::uint64_t u;
  std::memcpy(&u, &d, 8u);
  return u;
}

// Bit-exact field compare. Deliberately NOT memcmp (padding) so it compares the
// wire-meaningful fields exactly.
static bool states_identical(const core::DeviceStateV1& a, const core::DeviceStateV1& b) {
  if (a.schemaVersion != b.schemaVersion) return false;
  if (a.identityModelVersion != b.identityModelVersion) return false;
  if (a.identitySeed.seed != b.identitySeed.seed) return false;
  if (fbits(a.calibration.vcfLeftTrim) != fbits(b.calibration.vcfLeftTrim)) return false;
  if (fbits(a.calibration.vcfRightTrim) != fbits(b.calibration.vcfRightTrim)) return false;
  for (std::uint32_t i = 0; i < core::kDeviceParamCapacity; ++i)
    if (dbits(a.parameters[i]) != dbits(b.parameters[i])) return false;
  for (std::uint32_t i = 0; i < core::kDevicePatchCapacity; ++i) {
    if (a.inputCable[i] != b.inputCable[i]) return false;
    if (a.cableSource[i] != b.cableSource[i]) return false;
  }
  for (std::uint32_t i = 0; i < core::kDeviceRouteCapacity; ++i)
    if (a.routeOverridden[i] != b.routeOverridden[i]) return false;
  for (std::uint32_t k = 0; k < core::kDeviceKeyboardPresetCount; ++k) {
    const core::KeyboardPreset& x = a.keyboardPresets[k];
    const core::KeyboardPreset& y = b.keyboardPresets[k];
    if (x.id != y.id) return false;
    if (x.pressureBehaviour != y.pressureBehaviour) return false;
    if (x.pressureOutput != y.pressureOutput) return false;
    if (x.reserved[0] != y.reserved[0]) return false;
    if (x.reserved[1] != y.reserved[1]) return false;
  }
  if (a.keyboardSettings.pressureBehaviour != b.keyboardSettings.pressureBehaviour) return false;
  if (a.keyboardSettings.pressureOutput != b.keyboardSettings.pressureOutput) return false;
  if (a.leftEffector.program != b.leftEffector.program) return false;
  if (a.rightEffector.program != b.rightEffector.program) return false;
  for (std::uint32_t i = 0; i < core::kSequencerPhysicalBytes; ++i)
    if (a.sequencer.reserved[i] != b.sequencer.reserved[i]) return false;
  return true;
}

// Topology compare (reuses P2-② effectiveEdges): rebuild a PatchGraph from a
// DeviceState's stored cable facts (inputCable/cableSource indexed by raw JackId,
// which is gated < 65) and capture the canonical effective-edge set.
static void rebuild_and_connect(const core::DeviceStateV1& s, core::PatchGraph* g) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    const core::JackDescriptor& j = reg::kJacks[i];
    if (j.direction != core::PinDirection::input) continue;
    const std::uint32_t idx = static_cast<std::uint32_t>(j.id);
    if (s.inputCable[idx] != 0u) g->connect(s.cableSource[idx], j.id);
  }
}

static std::vector<core::PatchEdge> capture_edges(const core::PatchGraph& g) {
  std::vector<core::PatchEdge> v(512u);
  const std::uint32_t n = g.effectiveEdges(v.data(), static_cast<std::uint32_t>(v.size()));
  v.resize(n);
  std::sort(v.begin(), v.end(), core::patch_edge_before);
  return v;
}

// C++17: std::vector<...> == works only if the element has operator==; PatchEdge
// is an aggregate without one, so compare element-by-element on the canonical
// (already sorted) order.
static bool edges_equal(const std::vector<core::PatchEdge>& a,
                        const std::vector<core::PatchEdge>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].source != b[i].source) return false;
    if (a[i].sink != b[i].sink) return false;
  }
  return true;
}

// Byte offset of a top-level storage field in the wire record, walked from the
// field table so it can never silently diverge (mirrors the encoder loop).
static std::uint32_t storage_offset_of(const char* name) {
  std::uint32_t off = 0u;
  for (std::uint32_t i = 0; i < core::kDeviceStorageSchema.fieldCount; ++i) {
    const core::StorageField& f = core::kDeviceStorageSchema.fields[i];
    if (std::strcmp(f.name, name) == 0) return off;
    off += core::storage_field_bytes(f);
  }
  return 0u;
}

// --------------------------------------------------- must-test 1: atomic path --

struct MemFs {
  std::vector<std::uint8_t> live;
  std::vector<std::uint8_t> temp;
  const char* tempPath = "t";   // the temp path the save logic is told to use
  const char* livePath = "live";  // the live path the save logic is told to use
  bool failPartway = false;  // write partial data then fail
  bool failReplace = false;  // rename fails (fault between temp & rename)
};

// The mock is PATH-AWARE: it routes to the temp or live buffer by comparing the
// `path` string the production code passed against the configured temp/live path.
// This is what makes the atomic-replace mutation test honest — a real bug that
// writes to `livePath` (no temp isolation) is caught, and a caller passing the
// same path for temp and live really does corrupt the live file. Dropping `path`
// (the old `(void)path;` + an `inPlace` flag) proved only that the mock could
// reroute, never that the production temp->rename order held.
static bool fs_write(void* ctx, const char* path, const std::uint8_t* bytes,
                     std::size_t n) {
  MemFs* m = static_cast<MemFs*>(ctx);
  std::vector<std::uint8_t>& dst =
      (std::strcmp(path, m->livePath) == 0) ? m->live : m->temp;
  if (m->failPartway) {
    dst.assign(bytes, bytes + n / 2u);
    return false;
  }
  dst.assign(bytes, bytes + n);
  return true;
}
static bool fs_flush(void*, const char*) { return true; }
static bool fs_replace(void* ctx, const char*, const char*) {
  MemFs* m = static_cast<MemFs*>(ctx);
  if (m->failReplace) return false;
  m->live = m->temp;
  m->temp.clear();
  return true;
}
static void fs_discard(void* ctx, const char*) {
  MemFs* m = static_cast<MemFs*>(ctx);
  m->temp.clear();
}

// A rename that fails leaves the live file UNTOUCHED (still the prior valid
// state, still loadable) and the temp discarded — a half file is never exposed.
static void atomic_replace_never_exposes_half_file() {
  core::DeviceStateV1 old, next;
  fill_state(old);
  std::vector<std::uint8_t> oldBytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(old, oldBytes.data(), oldBytes.size()));

  fill_state(next);
  next.calibration.vcfLeftTrim = 99.0f;  // make "next" distinct from "old"
  std::vector<std::uint8_t> nextBytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(next, nextBytes.data(), nextBytes.size()));

  MemFs fs;
  fs.live = oldBytes;               // live holds the prior valid state
  fs.failReplace = true;            // fault AFTER temp write, BEFORE rename
  core::FileOps ops{fs_write, fs_flush, fs_replace, fs_discard};

  core::SaveResult r = core::save_state_atomic(nextBytes.data(), nextBytes.size(),
                                               "t", "live", ops, &fs);
  CHECK(r == core::SaveResult::replace_failed);
  CHECK(fs.live == oldBytes);       // live untouched
  CHECK(fs.temp.empty());           // aborted temp discarded

  // The untouched live file still decodes to the prior valid state.
  core::DeviceStateV1 decoded;
  CHECK(core::decode_device_state(fs.live.data(), fs.live.size(), &decoded));
  CHECK(states_identical(old, decoded));
}

// The negative: an in-place direct write (no temp+rename) corrupts the live file
// on a mid-write failure — exactly no crash-safe guarantee, and the reason the
// temp-flush-rename order is mandatory. This is a REAL degradation, not a mock
// reroute: the caller passes the SAME path for temp and live, so there is no
// isolated temp to protect the live file — whatever the mock writes to that path
// is the live state.
static void in_place_write_corrupts_live() {
  core::DeviceStateV1 old, next;
  fill_state(old);
  std::vector<std::uint8_t> oldBytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(old, oldBytes.data(), oldBytes.size()));
  fill_state(next);
  next.calibration.vcfLeftTrim = 99.0f;
  std::vector<std::uint8_t> nextBytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(next, nextBytes.data(), nextBytes.size()));

  MemFs fs;
  fs.live = oldBytes;
  fs.failPartway = true;   // ... and stop halfway
  core::FileOps ops{fs_write, fs_flush, fs_replace, fs_discard};

  // temp == live: no isolation. A mid-write fault exposes a half file as live.
  core::SaveResult r = core::save_state_atomic(nextBytes.data(), nextBytes.size(),
                                               "live", "live", ops, &fs);
  CHECK(r == core::SaveResult::temp_write_failed);
  // live is now truncated/corrupt (a half file). If the product used this path, a
  // crash here would leave the live file unusable -> the atomic order is required.
  CHECK(fs.live.size() != core::kDeviceStorageSchema.totalBytesHint);
  core::DeviceStateV1 decoded;
  CHECK_FALSE(core::decode_device_state(fs.live.data(), fs.live.size(), &decoded));
}

// --------------------------------------------------- must-test 2: debounce ----

// Rapid changes coalesce into fewer REAL writes than changes, but the FINAL state
// is never dropped: flushDirty emits a pending tail even right after a burst.
static void debounce_throttles_but_never_drops() {
  core::StateSaveDebounce deb(100u);  // 100 ms window
  std::uint64_t now = 1000u;
  std::uint64_t pendingValue = 0u;
  std::uint64_t savedValue = 0u;
  int realWrites = 0;
  const int changes = 5;

  for (int i = 1; i <= changes; ++i, now += 10u) {
    pendingValue = static_cast<std::uint64_t>(i);
    if (deb.markChanged(now)) {
      ++realWrites;          // a real disk write (writeFile invocation)
      savedValue = pendingValue;
    }
  }
  CHECK_EQ(realWrites, 1);   // the 5 rapid changes coalesced to 1 write
  CHECK(deb.dirty());        // a trailing change is held, not lost

  now += 200u;               // window elapses -> emit the final state
  CHECK(deb.flushDirty(now));
  ++realWrites;
  savedValue = pendingValue;
  CHECK_FALSE(deb.dirty());
  CHECK_EQ(realWrites, 2);
  CHECK(realWrites < changes);        // throttled
  CHECK_EQ(savedValue, static_cast<std::uint64_t>(changes));  // final state persisted
}

// Negative 1: a zero-window debounce writes every change (no throttle). Its
// realWrites == changeCount, i.e. it does NOT satisfy the `realWrites < changes`
// requirement the positive test enforces. We assert the degenerate really does
// emit every change, so that requirement is non-vacuous.
static void no_debounce_is_caught() {
  core::StateSaveDebounce deb(0u);  // == write-every-change
  std::uint64_t now = 1000u;
  int realWrites = 0;
  for (int i = 0; i < 5; ++i, now += 10u) {
    if (deb.markChanged(now)) ++realWrites;
  }
  CHECK_EQ(realWrites, 5);  // a no-throttle debounce isn't throttled
}

// Negative 2: a tail-dropping debounce clears dirty WITHOUT emitting when the
// window has not elapsed, so the trailing change is silently lost and the final
// persisted state != memory. The forbidden symptom is the tail not being held.
struct DroppingDebounce {
  bool windowElapsed = true;
  bool dirty_ = false;
  bool markChanged(std::uint64_t) {
    dirty_ = true;
    if (windowElapsed) {
      dirty_ = false;
      return true;
    }
    dirty_ = false;  // BUG: clears the pending change without writing it
    return false;
  }
  bool flushDirty(std::uint64_t) {
    if (!dirty_) return false;
    dirty_ = false;
    return true;
  }
  bool dirty() const { return dirty_; }
};

static void tail_dropping_debounce_is_caught() {
  DroppingDebounce deb;
  deb.windowElapsed = false;
  // A change arrives while the window hasn't elapsed.
  deb.markChanged(1000u);
  // FORBIDDEN SYMPTOM: the tail is dropped (nothing pending) and can't be
  // flushed, so the final state never reaches disk. A correct debounce holds it
  // dirty. These assert the degenerate path really loses the tail.
  CHECK_FALSE(deb.dirty());           // tail was dropped, nothing held
  CHECK_FALSE(deb.flushDirty(2000u)); // nothing to flush -> final state lost
}

// ------------------------------------------------- must-test 3: round-trip ----

// encode(height) -> bytes -> decode must give a field-identical state AND the same
// effective topology (after rebuilding a PatchGraph from the stored cable facts).
static void round_trip_fidelity() {
  core::DeviceStateV1 orig;
  fill_state(orig);
  std::vector<std::uint8_t> bytes(core::kDeviceStorageSchema.totalBytesHint);
  std::size_t written = 0u;
  CHECK(core::encode_device_state(orig, bytes.data(), bytes.size(), &written));
  CHECK_EQ(written, static_cast<std::size_t>(core::kDeviceStorageSchema.totalBytesHint));

  core::DeviceStateV1 back;
  CHECK(core::decode_device_state(bytes.data(), bytes.size(), &back));
  CHECK(states_identical(orig, back));  // field fidelity (incl. reserved patterns)

  // Topology identical: rebuild a graph from the loaded facts and compare the
  // canonical edge set against a graph built fresh from the same facts.
  core::PatchGraph gOriginals(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                              core::kRouteCount);
  rebuild_and_connect(orig, &gOriginals);
  core::PatchGraph gLoaded(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                           core::kRouteCount);
  rebuild_and_connect(back, &gLoaded);
  CHECK(edges_equal(capture_edges(gOriginals), capture_edges(gLoaded)));
}

// Negative: a serializer that OMITS cable_source (writes zeros there) in encode.
// The decoded state's cable facts no longer reproduce the original topology -> the
// fidelity check catches it. This is what separates "a real round trip" from "a
// round trip that dropped a field".
static void omitted_field_breaks_round_trip() {
  core::DeviceStateV1 orig;
  fill_state(orig);
  std::vector<std::uint8_t> bytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(orig, bytes.data(), bytes.size()));

  // Zero the cable_source field region -> a serializer that never wrote it.
  const std::uint32_t off = storage_offset_of("cable_source");
  const std::uint32_t n = core::storage_field_bytes(core::kDeviceStorageSchema.fields[7u]);
  std::memset(bytes.data() + off, 0, n);

  core::DeviceStateV1 back;
  CHECK(core::decode_device_state(bytes.data(), bytes.size(), &back));
  CHECK_FALSE(states_identical(orig, back));  // field fidelity broke -> red

  // Topology also diverged (cables now go source->itself, rejected as self-loops).
  core::PatchGraph gOrig(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                         core::kRouteCount);
  core::PatchGraph gBad(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                        core::kRouteCount);
  rebuild_and_connect(orig, &gOrig);
  rebuild_and_connect(back, &gBad);
  CHECK_FALSE(edges_equal(capture_edges(gOrig), capture_edges(gBad)));
}

// ------------------------------------------ must-test 4: never on audio thread --

// A backend that routes its write through the RtGuard file detector: saving off
// the audio thread is clean; saving inside the RT window is caught.
struct RtFs {
  std::vector<std::uint8_t> out;
};
static bool rtfs_write(void* ctx, const char* path, const std::uint8_t* bytes,
                       std::size_t n) {
  rt::rt_check_file_write(path);  // bumps g_file_in_rt if in the RT window
  static_cast<RtFs*>(ctx)->out.assign(bytes, bytes + n);
  return true;
}
static bool rtfs_flush(void*, const char*) { return true; }
static bool rtfs_replace(void*, const char*, const char*) { return true; }
static void rtfs_discard(void* ctx, const char*) {
  static_cast<RtFs*>(ctx)->out.clear();
}

static void persistence_never_on_audio_thread() {
  core::DeviceStateV1 s;
  fill_state(s);
  std::vector<std::uint8_t> bytes(core::kDeviceStorageSchema.totalBytesHint);
  CHECK(core::encode_device_state(s, bytes.data(), bytes.size()));

  RtFs fs;
  core::FileOps ops{rtfs_write, rtfs_flush, rtfs_replace, rtfs_discard};

  // Positive: save on the control thread (no RtGuard) -> clean.
  rt::reset();
  CHECK(core::save_state_atomic(bytes.data(), bytes.size(), "t", "live", ops, &fs) ==
        core::SaveResult::ok);
  CHECK_EQ(rt::g_file_in_rt.load(), 0L);

  // Negative: a disk save attempted INSIDE the audio callback is caught.
  rt::reset();
  {
    rt::RtGuard guard;
    core::save_state_atomic(bytes.data(), bytes.size(), "t", "live", ops, &fs);
  }
  CHECK(rt::g_file_in_rt.load() > 0L);  // red symptom
}

int main() {
  atomic_replace_never_exposes_half_file();
  in_place_write_corrupts_live();
  debounce_throttles_but_never_drops();
  no_debounce_is_caught();
  tail_dropping_debounce_is_caught();
  round_trip_fidelity();
  omitted_field_breaks_round_trip();
  persistence_never_on_audio_thread();
  return ::test::finish("state_persistence");
}
