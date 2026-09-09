// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_app_state_store.cpp — task #105 (GH#12): the APP one-shot startup restore, the device-reopen
// config retention and the exit atomic save, driven through the REAL file adapter in a REAL
// temporary directory and the REAL engine candidate path.
//
// The store (host/include/host/app_state_store.h) is the narrow coordination layer; the engine
// (host/include/host/standalone_audio_engine.h) is the production owner. Nothing here calls
// buildMachineRuntimeCandidate / processFrame / decode+validate directly as a substitute for the
// agreed entry: a load goes exact-length gate -> decode -> migrate -> validate -> the store's
// pending -> StandaloneAudioEngine::applyDeviceState, and a save goes encode -> save_state_atomic.
//
// EXIT CODE. 0 = every criterion green; 1 = at least one red. Each criterion prints a stable label
// so the isolated source-mutation negative controls (tools/run_app_state_negatives.py) can assert
// WHICH assertion fired, not merely that the binary failed.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <host/app_state_store.h>
#include <lunar24/core/device_layout.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/core/state_validation.h>

namespace core = lunar24::core;
namespace host = lunar24::host;

using core::DeviceStateV1;
using core::JackId;
using host::AppStateStore;
using host::StandaloneAudioEngine;
using host::StateLoadOutcome;
using host::StateSaveOutcome;

// ---- the tiny named checker (same shape as the GH#12 probe / preset-action family) -------------

static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// ---- fixed format facts ----------------------------------------------------------------------

static constexpr std::uint64_t kSeed = 0x4C554E4152ULL;
static constexpr double kSr = 48000.0;
static constexpr int kBlock = 4096;
static constexpr int kInCh = 2;
static constexpr int kOutCh = 4;
static constexpr std::size_t kWire = host::kAppStateWireBytes;

// ---- real temp directory ---------------------------------------------------------------------

static std::string makeTempDir(const char* tag) {
  static std::uint64_t seq = 0u;
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  const std::string name = std::string("lunar24-app-state-") + tag + "-" +
                           std::to_string(static_cast<unsigned long long>(seq++));
  const auto dir = base / name;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

static void removeTree(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(dir), ec);
}

static bool readBytes(const std::string& path, std::vector<std::uint8_t>* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  if (std::fseek(f, 0L, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  const long end = std::ftell(f);
  if (end < 0 || std::fseek(f, 0L, SEEK_SET) != 0) {
    std::fclose(f);
    return false;
  }
  out->assign(static_cast<std::size_t>(end), 0u);
  const bool ok = out->empty() || std::fread(out->data(), 1u, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

static bool writeBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1u, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

static bool encodeState(const DeviceStateV1& st, std::vector<std::uint8_t>* out) {
  out->assign(kWire, 0u);
  std::size_t written = 0u;
  return core::encode_device_state(st, out->data(), out->size(), &written) && written == kWire;
}

// The WIRE equality the store promises: two states are the same persisted state iff their encoded
// records are byte-identical (this is the contract boundary — the store never claims more).
static bool wireEqual(const DeviceStateV1& a, const DeviceStateV1& b) {
  std::vector<std::uint8_t> wa, wb;
  if (!encodeState(a, &wa) || !encodeState(b, &wb)) return false;
  return wa == wb;
}

// ---- a LEGAL, distinctly non-default fixture -------------------------------------------------
// Non-default in exactly the regions the mandate names: identity seed, calibration, a two-sided
// keyboard live bank, all four preset slots, and a user cable set. Every value is built through the
// registry descriptor (legal_value) or through a directly finite-checked region, and the fixture is
// itself validated before it is used (C0) — a fixture that is not legal would make every criterion
// below vacuous.

static double legal_value(core::ParameterId id, double t) {
  const core::ParameterDescriptor* d = core::find_parameter(id);
  if (d == nullptr) return 0.0;
  if (d->optionCount > 0u) {
    const double n = static_cast<double>(d->optionCount - 1u);
    double idx = std::floor(t * n + 0.5);
    if (idx < 0.0) idx = 0.0;
    if (idx > n) idx = n;
    return idx;
  }
  double v = d->min + t * (d->max - d->min);
  if (d->step > 0.0) v = d->min + std::floor((v - d->min) / d->step + 0.5) * d->step;
  if (v < d->min) v = d->min;
  if (v > d->max) v = d->max;
  // The wire stores the scalar banks as float32: keep the fixture value EXACTLY representable so a
  // round trip is a byte comparison, not a tolerance question.
  float f = static_cast<float>(v);
  if (static_cast<double>(f) > d->max) f = std::nextafterf(f, -INFINITY);
  if (static_cast<double>(f) < d->min) f = std::nextafterf(f, INFINITY);
  return static_cast<double>(f);
}

static void setCable(DeviceStateV1& st, JackId source, JackId sink) {
  const std::uint32_t s = static_cast<std::uint32_t>(sink);
  st.inputCable[s] = 1u;
  st.cableSource[s] = source;
}

static DeviceStateV1 nonDefaultState() {
  DeviceStateV1 st = core::make_default_device_state(kSeed);

  st.identitySeed.seed = 0x1122334455667788ULL;  // identity comes from the FILE, not the constant
  st.calibration.vcfLeftTrim = 0.25f;
  st.calibration.vcfRightTrim = 2.0f;  // trims must be finite AND > 0 (state_validation.h:144-149)

  // Two-sided keyboard live bank: left = parameters[id], right = keyboardScalarRight[index].
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[j];
    const double t = 0.15 + 0.7 * (static_cast<double>((j * 7u) % 11u) / 10.0);
    const double v = legal_value(id, t);
    st.parameters[static_cast<std::size_t>(id)] = v;
    const std::int32_t i = core::keyboard_scalar_index(id);
    if (i >= 0) st.keyboardScalarRight[static_cast<std::size_t>(i)] = legal_value(id, 1.0 - t);
  }
  // The two coherence mirrors (state_validation field 9002/9003) follow the canonical LEFT values.
  st.keyboardSettings.pressureBehaviour = static_cast<std::uint8_t>(
      st.parameters[static_cast<std::uint32_t>(core::ParameterId::keyboard_behaviour)]);
  st.keyboardSettings.pressureOutput = static_cast<std::uint8_t>(
      st.parameters[static_cast<std::uint32_t>(core::ParameterId::keyboard_pressure_output)]);

  st.keyboardScaleEditor = 0x0123u;
  st.keyboardScaleEditorR = 0x0456u;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    st.keyboardPlateTune[i] = static_cast<float>(0.01 * (i + 1));
    st.keyboardPlateTuneR[i] = static_cast<float>(-0.02 * (i + 1));
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    st.keyboardPushbutton[i] = static_cast<float>(0.5 + 0.01 * (i + 1));
    st.keyboardPushbuttonR[i] = static_cast<float>(0.25 + 0.01 * (i + 1));
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    st.keyboardSeqCurrent.steps[i].note = static_cast<std::uint8_t>(i % 12u);
    st.keyboardSeqCurrent.steps[i].value = static_cast<float>(0.1 * (i + 1));
    st.keyboardSeqCurrent.steps[i].gate = static_cast<std::uint8_t>(i % 2u);
    st.keyboardSeqCurrentR.steps[i].note = static_cast<std::uint8_t>((i + 5u) % 12u);
    st.keyboardSeqCurrentR.steps[i].value = static_cast<float>(0.05 * (i + 1));
    st.keyboardSeqCurrentR.steps[i].gate = static_cast<std::uint8_t>((i + 1u) % 2u);
  }
  for (std::uint32_t i = 0; i < 4u; ++i) {
    st.keyboardClockSelectors[i] = static_cast<std::uint8_t>(i % 2u);
    st.keyboardClockSelectorsR[i] = static_cast<std::uint8_t>((i + 1u) % 2u);
  }

  // All four preset slots carry a DISTINCT payload (composite regions only; the 22 scalar pairs
  // keep the default's legal values, so the fixture stays inside check_presets).
  for (std::uint32_t k = 0; k < core::kDeviceKeyboardPresetCount; ++k) {
    core::KeyboardPreset& p = st.keyboardPresets[k];
    p.quantiseScaleEditor = static_cast<std::uint16_t>(0x0100u + k);
    p.quantiseScaleEditorR = static_cast<std::uint16_t>(0x0200u + k);
    for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
      p.plateTune[i] = static_cast<float>(0.1 * (k + 1) + 0.01 * (i + 1));
      p.plateTuneR[i] = static_cast<float>(-0.1 * (k + 1) - 0.01 * (i + 1));
    }
    for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
      p.pushbuttonValue[i] = static_cast<float>(0.2 * (k + 1) + 0.01 * (i + 1));
      p.pushbuttonValueR[i] = static_cast<float>(0.3 * (k + 1) + 0.01 * (i + 1));
    }
    for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
      p.seqSteps.steps[i].note = static_cast<std::uint8_t>((i + k) % 12u);
      p.seqSteps.steps[i].value = static_cast<float>(0.01 * (i + 1) * (k + 1));
      p.seqSteps.steps[i].gate = static_cast<std::uint8_t>((i + k) % 2u);
      p.seqStepsR.steps[i].note = static_cast<std::uint8_t>((i + k + 3u) % 12u);
      p.seqStepsR.steps[i].value = static_cast<float>(0.02 * (i + 1) * (k + 1));
      p.seqStepsR.steps[i].gate = static_cast<std::uint8_t>((i + k + 1u) % 2u);
    }
  }

  // User cables to NON-route sinks (no override bookkeeping): three distinct sources, all across
  // already-compiled supported modules.
  setCable(st, JackId::env_follower_env_out, JackId::drone_1_cv_mod_in);
  setCable(st, JackId::vco_b_vco_out, JackId::drone_2_cv_mod_in);
  setCable(st, JackId::joystick_x_out, JackId::drone_4_cv_mod_in);
  return st;
}

// ---- fault double for the save matrix --------------------------------------------------------
// The backend's own contract: report failure without touching the destination unless the mutation
// explicitly asks it to (deleteBeforeReplace) — that mutation is what C5.4 must catch.

struct FaultFs {
  std::vector<std::uint8_t> temp;  // the temp file: written, then moved (or discarded)
  std::vector<std::uint8_t> live;  // the LIVE file: only ever replaced by a successful move
  bool liveExists = false;
  bool failWrite = false;
  bool failFlush = false;
  bool failReplace = false;
  bool deleteBeforeReplace = false;
  bool shortWrite = false;  // write HALF the bytes but still report success (a lying backend)
  int writeCalls = 0;
  int flushCalls = 0;
  int replaceCalls = 0;
  int discardCalls = 0;

  static bool write(void* ctx, const char* path, const std::uint8_t* bytes, std::size_t n) {
    (void)path;
    FaultFs* self = static_cast<FaultFs*>(ctx);
    ++self->writeCalls;
    if (self->failWrite) return false;
    if (self->shortWrite) {
      self->temp.assign(bytes, bytes + n / 2u);
      return true;  // LIES: half the bytes, success reported
    }
    self->temp.assign(bytes, bytes + n);
    return true;
  }
  static bool flush(void* ctx, const char*) {
    FaultFs* self = static_cast<FaultFs*>(ctx);
    ++self->flushCalls;
    return !self->failFlush;
  }
  static bool replace(void* ctx, const char*, const char*) {
    FaultFs* self = static_cast<FaultFs*>(ctx);
    ++self->replaceCalls;
    if (self->failReplace) {
      if (self->deleteBeforeReplace) {
        // The mutation under test: destroy the destination before the move, so a failed
        // replace still loses the prior live file.
        self->live.clear();
        self->liveExists = false;
      }
      return false;
    }
    self->live = self->temp;  // an atomic move makes the temp bytes the live file
    self->liveExists = true;
    return true;
  }
  static void discard(void* ctx, const char*) {
    FaultFs* self = static_cast<FaultFs*>(ctx);
    ++self->discardCalls;
    self->temp.clear();  // best-effort temp cleanup; NEVER touches the live file
  }
  core::FileOps ops() {
    core::FileOps o;
    o.writeFile = &write;
    o.flushFile = &flush;
    o.atomicReplace = &replace;
    o.discardFile = &discard;
    return o;
  }
};

// A counter double that only tallies (used by C7): it never mutates any state.
struct CountFs {
  int calls = 0;
  static bool write(void* ctx, const char*, const std::uint8_t*, std::size_t) {
    ++static_cast<CountFs*>(ctx)->calls;
    return false;
  }
  static bool flush(void* ctx, const char*) {
    ++static_cast<CountFs*>(ctx)->calls;
    return false;
  }
  static bool replace(void* ctx, const char*, const char*) {
    ++static_cast<CountFs*>(ctx)->calls;
    return false;
  }
  static void discard(void* ctx, const char*) { ++static_cast<CountFs*>(ctx)->calls; }
  core::FileOps ops() {
    core::FileOps o;
    o.writeFile = &write;
    o.flushFile = &flush;
    o.atomicReplace = &replace;
    o.discardFile = &discard;
    return o;
  }
};

// A backend that inspects the REAL temp file at write time: it proves the store claimed the name by
// exclusive creation BEFORE handing it to the backend, and that the cleanup removes exactly that
// path (and nothing else). The flush then fails, so the store's own cleanup has to run.
struct ReserveProbeFs {
  std::string seenPath;
  bool existedAtWrite = false;
  std::uintmax_t sizeAtWrite = 0u;
  static bool write(void* ctx, const char* path, const std::uint8_t*, std::size_t) {
    ReserveProbeFs* self = static_cast<ReserveProbeFs*>(ctx);
    self->seenPath = path;
    std::error_code ec;
    const std::filesystem::path p(path);
    self->existedAtWrite = std::filesystem::exists(p, ec);
    self->sizeAtWrite = std::filesystem::file_size(p, ec);
    return true;  // pretend the bytes landed; the failing flush below aborts the save
  }
  static bool flush(void*, const char*) { return false; }
  static bool replace(void*, const char*, const char*) { return false; }
  static void discard(void*, const char*) {}
  core::FileOps ops() {
    core::FileOps o;
    o.writeFile = &write;
    o.flushFile = &flush;
    o.atomicReplace = &replace;
    o.discardFile = &discard;
    return o;
  }
};

// ---- C0 the fixture is legal ----------------------------------------------------------------

static void c0_fixture_is_legal() {
  const DeviceStateV1 st = nonDefaultState();
  const auto v = core::validate_device_state(st);
  check(v.ok, "C0 the non-default fixture passes validate_device_state");
  const DeviceStateV1 def = core::make_default_device_state(kSeed);
  check(!wireEqual(st, def), "C0 the fixture is genuinely different from the default");
}

// ---- C1 the real file round trip ------------------------------------------------------------

static void c1_real_round_trip() {
  const std::string dir = makeTempDir("c1");
  const DeviceStateV1 st = nonDefaultState();

  // A settings.ini sentinel: the APP state must never touch the iPlug2 INI.
  const std::vector<std::uint8_t> ini = {'[', 'S', 't', 'a', 't', 'e', ']', '\n', 'k', '=', '1', '\n'};
  check(writeBytes(dir + "/settings.ini", ini), "C1.3 the settings.ini sentinel was written");

  AppStateStore store;
  store.setDirectory(dir);
  StandaloneAudioEngine engine;
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C1 the engine prepared");
  check(store.loadOnce() == StateLoadOutcome::NoFile, "C1 a first run reports NoFile");
  check(engine.applyDeviceState(st, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C1 the non-default fixture committed as the engine canonical");
  check(store.save(engine) == StateSaveOutcome::Saved, "C1.1 the lifecycle save wrote the file");

  std::vector<std::uint8_t> bytes;
  check(readBytes(store.livePath(), &bytes), "C1.1 the saved file is readable");
  check(bytes.size() == kWire, "C1.1 the saved file is EXACTLY the wire size");
  check(bytes.size() == core::kDeviceStorageSchema.totalBytesHint,
        "C1.1 the wire size is the frozen totalBytesHint");

  DeviceStateV1 decoded;
  const bool decodedOk =
      core::decode_device_state(bytes.data(), bytes.size(), &decoded) && decoded.schemaVersion ==
                                                                               st.schemaVersion;
  check(decodedOk, "C1.2 the saved bytes decode");
  check(decodedOk && wireEqual(decoded, st), "C1.2 the saved bytes decode back to the saved state");

  // The lying-backend criterion, restated on the PRODUCT file: a backend that reports success
  // after writing half the bytes must be caught by reading the published file back.
  std::vector<std::uint8_t> reread;
  check(readBytes(store.livePath(), &reread) && reread.size() == kWire,
        "C1.2 the published file is complete on disk, not just reported complete");

  std::vector<std::uint8_t> iniAfter;
  check(readBytes(dir + "/settings.ini", &iniAfter) && iniAfter == ini,
        "C1.3 settings.ini is byte-identical after the save");

  const std::string t1 = store.tempPath();
  const std::string t2 = store.tempPath();
  check(!t1.empty() && t1 != store.livePath() && t2 != t1,
        "C1.4 temp paths are unique and never the live path");
  check(std::filesystem::path(t1).parent_path() == std::filesystem::path(dir),
        "C1.4 the temp file lives in the SAME directory as the live file");

  // A brand-new store over the same directory: the file state must come back.
  AppStateStore store2;
  store2.setDirectory(dir);
  check(store2.loadOnce() == StateLoadOutcome::Ok, "C1.5 a new store reads the file");
  check(store2.pending() != nullptr && wireEqual(*store2.pending(), st),
        "C1.5 the non-default file state is restored");
  check(store2.pending() != nullptr &&
            store2.pending()->identitySeed.seed == 0x1122334455667788ULL,
        "C1.6 the identity seed comes from the file, not the startup constant");
  check(store2.pending() != nullptr && store2.pending()->identitySeed.seed != kSeed,
        "C1.6 the restored seed is NOT kLunarStartupSeed");

  // The directory holds only the two product files: no leftover temp.
  int entries = 0;
  int strays = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    ++entries;
    const std::string name = e.path().filename().string();
    if (name != host::kAppStateFileName && name != "settings.ini") ++strays;
  }
  check(entries == 2 && strays == 0,
        "C1.7 the directory holds only the state file and settings.ini (no leftover temp)");

  removeTree(dir);
}

// ---- C2 one read per session / pending transfer ---------------------------------------------

static void c2_one_read_and_transfer() {
  const std::string dir = makeTempDir("c2");
  const DeviceStateV1 fileState = nonDefaultState();
  std::vector<std::uint8_t> wire;
  check(encodeState(fileState, &wire), "C2 the file state encoded");
  check(writeBytes(dir + "/" + host::kAppStateFileName, wire), "C2 the file state was written");

  AppStateStore store;
  store.setDirectory(dir);
  check(store.loadOnce() == StateLoadOutcome::Ok, "C2.1 the first read adopted the file state");
  check(store.readAttempts() == 1u, "C2.1 exactly one read attempt was made");

  // Rewrite the disk file with a DIFFERENT state. A second boundary must NOT observe it.
  const DeviceStateV1 other = core::make_default_device_state(0xDEADBEEFULL);
  std::vector<std::uint8_t> otherWire;
  check(encodeState(other, &otherWire), "C2.2 the replacement disk state encoded");
  check(writeBytes(dir + "/" + host::kAppStateFileName, otherWire), "C2.2 the disk file was replaced");

  check(store.loadOnce() == StateLoadOutcome::Ok, "C2.2 the second boundary returns the latched outcome");
  check(store.readAttempts() == 1u, "C2.1 the second boundary performed NO disk read");
  check(store.pending() != nullptr && wireEqual(*store.pending(), fileState),
        "C2.2 the second boundary still uses the session's file state (no re-read)");

  // Publish it, then prove the pending is gone and the canonical state is the authority.
  StandaloneAudioEngine engine;
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C2.6 the engine prepared");
  check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C2.6 the pending restore published");
  check(!store.hasPending(), "C2.6 the pending is cleared once published");
  check(engine.canonicalState() != nullptr && wireEqual(*engine.canonicalState(), fileState),
        "C2.6 the engine canonical is now the single authority");

  // A device reopen: capture BEFORE prepare releases the owner; the session config must survive.
  DeviceStateV1 session = fileState;
  session.calibration.vcfLeftTrim = 0.75f;
  check(engine.applyDeviceState(session, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C2.5 the session committed a changed config");
  store.captureCanonical(engine);
  check(store.hasPending() && wireEqual(*store.pending(), session),
        "C2.5 the boundary captured the last committed session config");
  check(store.loadOnce() == StateLoadOutcome::Ok && store.readAttempts() == 1u,
        "C2.5 the reopen did not re-read the disk");
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C2.5 the reopened device prepared");
  check(engine.canonicalState() != nullptr &&
            wireEqual(*engine.canonicalState(), core::make_default_device_state(kSeed)),
        "C2.5 prepare() rebuilt the power-on default (the retention problem is real)");
  check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C2.5 the pending session config re-published");
  check(engine.canonicalState() != nullptr && wireEqual(*engine.canonicalState(), session),
        "C2.5 the device reopen kept the last committed session config");

  // C2.3/C2.4: an ILLEGAL format makes prepare() fail and release the owner; the pending must
  // survive and the NEXT legal boundary must restore it.
  store.captureCanonical(engine);
  check(store.hasPending(), "C2.3 the boundary captured the config before the illegal format");
  check(!engine.prepare(kSeed, 0.0, kBlock, kInCh, kOutCh),
        "C2.3 an illegal sample rate makes prepare() fail");
  check(engine.canonicalState() == nullptr, "C2.3 the failed prepare released the owner");
  check(store.hasPending() && wireEqual(*store.pending(), session),
        "C2.3 the pending survives the failed prepare (clearState_ cannot lose the session)");
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C2.4 the next legal boundary prepared");
  check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C2.4 the retained config re-published");
  check(engine.canonicalState() != nullptr && wireEqual(*engine.canonicalState(), session),
        "C2.4 the next legal boundary restored the retained config");

  removeTree(dir);
}

// ---- C3 startup restore ---------------------------------------------------------------------

static void c3_startup_restore() {
  const std::string dir = makeTempDir("c3");
  const DeviceStateV1 fileState = nonDefaultState();
  std::vector<std::uint8_t> wire;
  check(encodeState(fileState, &wire), "C3 the file state encoded");
  check(writeBytes(dir + "/" + host::kAppStateFileName, wire), "C3 the file state was written");

  AppStateStore store;
  store.setDirectory(dir);
  StandaloneAudioEngine engine;
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C3.1 the fresh engine prepared the default");
  check(engine.canonicalState() != nullptr &&
            wireEqual(*engine.canonicalState(), core::make_default_device_state(kSeed)),
        "C3.1 a fresh instance boots the default before the restore");
  check(store.loadOnce() == StateLoadOutcome::Ok, "C3.1 the startup read adopted the file state");
  check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C3.1 the file state published through the real candidate");
  check(engine.canonicalState() != nullptr && wireEqual(*engine.canonicalState(), fileState),
        "C3.1 a fresh instance restores the file state");
  check(engine.isReady(), "C3.1 the engine stays ready after the restore");

  // A missing file: the existing constant seed default boots, and the lifecycle save is allowed.
  const std::string emptyDir = makeTempDir("c3b");
  AppStateStore missing;
  missing.setDirectory(emptyDir);
  StandaloneAudioEngine engine2;
  check(engine2.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C3.2 the engine prepared");
  check(missing.loadOnce() == StateLoadOutcome::NoFile, "C3.2 a missing file reports NoFile");
  check(!missing.hasPending(), "C3.2 a missing file leaves no pending restore");
  check(engine2.canonicalState() != nullptr &&
            wireEqual(*engine2.canonicalState(), core::make_default_device_state(kSeed)),
        "C3.2 a missing file boots the constant seed default");
  check(missing.save(engine2) == StateSaveOutcome::Saved,
        "C3.3 a missing file allows the lifecycle save");

  removeTree(dir);
  removeTree(emptyDir);
}

// ---- C4 failure semantics -------------------------------------------------------------------

static void c4_failure_semantics() {
  // Every case: the file must be preserved byte-for-byte, the engine must still boot the default,
  // and the lifecycle save must be blocked.
  struct Case {
    const char* label;
    std::vector<std::uint8_t> bytes;
    StateLoadOutcome want;
  };
  std::vector<Case> cases;

  std::vector<std::uint8_t> good;
  check(encodeState(nonDefaultState(), &good), "C4 the good record encoded");

  cases.push_back({"C4.1 a truncated file is a length mismatch",
                   std::vector<std::uint8_t>(good.begin(), good.begin() + 100), StateLoadOutcome::LengthMismatch});
  {
    std::vector<std::uint8_t> big = good;
    big.push_back(0u);
    cases.push_back({"C4.2 an oversized file is a length mismatch", big, StateLoadOutcome::LengthMismatch});
  }
  {
    DeviceStateV1 old = nonDefaultState();
    old.schemaVersion = core::kDeviceStorageSchemaVersion - 1u;
    std::vector<std::uint8_t> w;
    check(encodeState(old, &w), "C4.3 the old-version record encoded");
    cases.push_back({"C4.3 an old schema version is unsupported", w, StateLoadOutcome::UnsupportedVersion});
  }
  {
    DeviceStateV1 newer = nonDefaultState();
    newer.schemaVersion = core::kDeviceStorageSchemaVersion + 1u;
    std::vector<std::uint8_t> w;
    check(encodeState(newer, &w), "C4.3b the newer-version record encoded");
    cases.push_back({"C4.3b a newer schema version requires a newer codec", w, StateLoadOutcome::RequiresNewerCodec});
  }
  {
    DeviceStateV1 bad = nonDefaultState();
    bad.parameters[0] = std::nan("");
    std::vector<std::uint8_t> w;
    check(encodeState(bad, &w), "C4.4 the non-finite record encoded");
    cases.push_back({"C4.4 an invalid state is rejected", w, StateLoadOutcome::InvalidState});
  }
  {
    // Valid per validate_device_state, but the real candidate graph cannot compile.
    DeviceStateV1 unexecutable = core::make_default_device_state(kSeed);
    setCable(unexecutable, JackId::env_follower_env_out, JackId::effector_cv_x_in);
    std::vector<std::uint8_t> w;
    check(encodeState(unexecutable, &w), "C4.5 the unexecutable record encoded");
    cases.push_back({"C4.5 a valid but unexecutable record is ADOPTED as a candidate "
                     "(the engine refuses it, not the loader)",
                     w, StateLoadOutcome::Ok});
  }

  for (const Case& c : cases) {
    const std::string dir = makeTempDir("c4");
    const std::string live = dir + "/" + host::kAppStateFileName;
    check(writeBytes(live, c.bytes), "C4 the bad file was written");

    AppStateStore store;
    store.setDirectory(dir);
    check(store.loadOnce() == c.want, c.label);
    check(store.saveAllowed() == false, "C4.7 a failed load blocks the lifecycle save gate");

    std::vector<std::uint8_t> after;
    check(readBytes(live, &after) && after == c.bytes,
          "C4.7 the failed load preserved the original file bytes");

    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C4.8 the engine booted the default");
    check(engine.isReady() && engine.canonicalState() != nullptr &&
              wireEqual(*engine.canonicalState(), core::make_default_device_state(kSeed)),
          "C4.8 a failed load still boots the constant seed default and stays ready");

    if (store.hasPending()) {
      const auto pub = store.publishPending(engine, kSr, kBlock, kInCh, kOutCh);
      check(pub != StandaloneAudioEngine::StateApplyStatus::Accepted,
            "C4.5 the unexecutable candidate was refused by the real engine");
      check(store.lastPublishStatus() == StandaloneAudioEngine::StateApplyStatus::RejectedGraph,
            "C4.5 the refusal is reported as RejectedGraph");
      check(store.hasRejectedCandidate(), "C4.5 the refused candidate is kept inspectable");
      check(!store.saveAllowed(), "C4.5 the refused file must never be overwritten");
    }

    check(store.save(engine) == StateSaveOutcome::SkippedFileUnhealthy,
          "C4.9 a failed load blocks the lifecycle save");
    std::vector<std::uint8_t> finalBytes;
    check(readBytes(live, &finalBytes) && finalBytes == c.bytes,
          "C4.9 the exit save did not overwrite the bad file");
    removeTree(dir);
  }

  // An unreadable path (it IS a directory) must be reported explicitly, never as "no file yet".
  {
    const std::string dir = makeTempDir("c4dir");
    std::error_code ec;
    check(std::filesystem::create_directory(std::filesystem::path(dir) / host::kAppStateFileName, ec) && !ec,
          "C4.6 the unreadable path was created");
    AppStateStore store;
    store.setDirectory(dir);
    check(store.loadOnce() == StateLoadOutcome::Unreadable,
          "C4.6 an unreadable path is NOT reported as a missing file");
    check(!store.saveAllowed(), "C4.6 an unreadable path blocks the lifecycle save");
    removeTree(dir);
  }

  // No path at all: NoPath (not NoFile), no fallback to the working directory.
  {
    const std::string cwdBefore = std::filesystem::current_path().string();
    AppStateStore store;
    check(!store.hasDirectory(), "C4.10 a store without a directory has no path");
    check(store.loadOnce() == StateLoadOutcome::NoPath, "C4.10 no path is reported as NoPath");
    check(store.livePath().empty(), "C4.10 no path means no live path");
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C4.10 the engine prepared");
    check(store.save(engine) == StateSaveOutcome::SkippedNoPath,
          "C4.10 no path is reported as SkippedNoPath");
    check(std::filesystem::current_path().string() == cwdBefore &&
              !std::filesystem::exists(std::filesystem::path(cwdBefore) / host::kAppStateFileName),
          "C4.10 the save never fell back to the working directory");
  }

  // C4.11/C4.12 — the protection is STICKY across device reopens. A legal file whose graph the REAL
  // candidate refuses must stay protected even after a SUCCESSFUL FromSession publish: the plugin's
  // OnReset sequence (captureCanonical -> loadOnce -> prepare -> publishPending) publishes the
  // power-on default on the next boundary, and that is exactly the sequence that used to re-open the
  // gate and let the exit save overwrite the refused file.
  {
    DeviceStateV1 unexecutable = core::make_default_device_state(kSeed);
    setCable(unexecutable, JackId::env_follower_env_out, JackId::effector_cv_x_in);
    std::vector<std::uint8_t> original;
    check(encodeState(unexecutable, &original), "C4.11 the refused record encoded");

    for (int reopens : {1, 3}) {
      const std::string dir = makeTempDir("c4sticky");
      const std::string live = dir + "/" + host::kAppStateFileName;
      check(writeBytes(live, original), "C4.11 the refused file was written");

      AppStateStore store;
      store.setDirectory(dir);
      StandaloneAudioEngine engine;
      check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C4.11 the engine booted");

      // Boundary 1: the file is adopted as a candidate and refused by the REAL engine.
      store.captureCanonical(engine);
      check(store.loadOnce() == StateLoadOutcome::Ok,
            "C4.11 the refused file was loaded as a candidate");
      check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C4.11 the boundary prepared");
      check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
                StandaloneAudioEngine::StateApplyStatus::RejectedGraph,
            "C4.11 the real candidate refused the graph");
      check(!store.saveAllowed() && store.fileUnadopted(),
            "C4.11 the refusal closed the lifecycle save gate");

      // Boundaries 2..N: device reopens. Each one publishes the DEFAULT config FromSession.
      for (int i = 0; i < reopens; ++i) {
        store.captureCanonical(engine);
        check(store.loadOnce() == StateLoadOutcome::Ok && store.readAttempts() == 1u,
              "C4.11 the reopen did not re-read the disk");
        check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh),
              "C4.11 the reopened device prepared");
        check(store.publishPending(engine, kSr, kBlock, kInCh, kOutCh) ==
                  StandaloneAudioEngine::StateApplyStatus::Accepted,
              "C4.11 the default config published successfully on the reopen");
        check(!store.saveAllowed() && store.fileUnadopted(),
              "C4.12 a successful FromSession publish did NOT re-open the gate");
      }

      check(store.save(engine) == StateSaveOutcome::SkippedFileUnhealthy,
            "C4.11 the exit save refused to touch the refused file");
      std::vector<std::uint8_t> after;
      check(readBytes(live, &after) && after == original,
            "C4.12 the refused file is preserved byte-for-byte across device reopens");
      removeTree(dir);
    }
  }

  // C4.13 — the size gate runs BEFORE any allocation or read: a huge record must be a typed
  // LengthMismatch without pulling the file into memory first (the old ftell+resize did exactly that
  // before it knew the length was wrong).
  {
    const std::string dir = makeTempDir("c4big");
    const std::string live = dir + "/" + host::kAppStateFileName;
    check(writeBytes(live, std::vector<std::uint8_t>()), "C4.13 the oversized record was created");
    std::error_code ec;
    std::filesystem::resize_file(live, 64u * 1024u * 1024u, ec);
    check(!ec && std::filesystem::exists(live), "C4.13 a 64 MiB oversized record was created");
    AppStateStore store;
    store.setDirectory(dir);
    check(store.loadOnce() == StateLoadOutcome::LengthMismatch,
          "C4.13 an oversized record is a length mismatch");
    check(store.bytesRead() == 0u, "C4.13 NOTHING was read: the size gate precedes allocation");
    check(!store.saveAllowed() && store.fileUnadopted(),
          "C4.13 the oversized file stays protected");
    removeTree(dir);
  }

  // C4.14 — the READ boundary itself: bounded to the wire size, and a trailing byte past the record
  // fails (the fstat size gate is a snapshot, not a lock, so growth after it must still be caught).
  {
    const std::string dir = makeTempDir("c4read");
    std::vector<std::uint8_t> good2;
    check(encodeState(nonDefaultState(), &good2), "C4.14 the record encoded");

    const std::string exactPath = dir + "/exact";
    check(writeBytes(exactPath, good2), "C4.14 the exact-size file was written");
    std::uint64_t readCount = 0u;
    bool ioError = false;
    std::vector<std::uint8_t> out;
    std::FILE* f = std::fopen(exactPath.c_str(), "rb");
    check(f != nullptr, "C4.14 the exact-size file opened");
    if (f != nullptr) {
      const bool ok = AppStateStore::readExactRecord(f, &out, &readCount, &ioError);
      std::fclose(f);
      check(ok && !ioError && readCount == kWire,
            "C4.14 an exact-size record is accepted and exactly that many bytes were read");
    }

    std::vector<std::uint8_t> grown = good2;
    grown.push_back(0x5Au);
    const std::string grownPath = dir + "/grown";
    check(writeBytes(grownPath, grown), "C4.14 the record with a trailing byte was written");
    readCount = 0u;
    f = std::fopen(grownPath.c_str(), "rb");
    check(f != nullptr, "C4.14 the grown file opened");
    if (f != nullptr) {
      const bool ok = AppStateStore::readExactRecord(f, &out, &readCount, &ioError);
      std::fclose(f);
      check(!ok && !ioError, "C4.14 a byte BEYOND the record is rejected, not accepted");
    }

    std::vector<std::uint8_t> shortRec(good2.begin(), good2.begin() + 100);
    const std::string shortPath = dir + "/short";
    check(writeBytes(shortPath, shortRec), "C4.14 the short record was written");
    readCount = 0u;
    f = std::fopen(shortPath.c_str(), "rb");
    check(f != nullptr, "C4.14 the short file opened");
    if (f != nullptr) {
      const bool ok = AppStateStore::readExactRecord(f, &out, &readCount, &ioError);
      std::fclose(f);
      check(!ok && !ioError, "C4.14 a SHORT read is rejected, not accepted");
    }
    removeTree(dir);
  }
}

// ---- C5 the save failure matrix -------------------------------------------------------------

static void c5_save_failures() {
  const DeviceStateV1 first = nonDefaultState();
  DeviceStateV1 second = first;
  second.calibration.vcfLeftTrim = 0.9f;

  struct Mode {
    const char* label;
    bool FaultFs::* flag;
    StateSaveOutcome want;
  };
  const Mode modes[] = {
      {"C5.1 a temp write failure leaves the live file", &FaultFs::failWrite, StateSaveOutcome::TempWriteFailed},
      {"C5.2 a flush failure leaves the live file", &FaultFs::failFlush, StateSaveOutcome::FlushFailed},
      {"C5.3 a replace failure leaves the live file", &FaultFs::failReplace, StateSaveOutcome::ReplaceFailed},
  };

  for (const Mode& m : modes) {
    const std::string dir = makeTempDir("c5");
    FaultFs fs;
    AppStateStore store;
    store.setDirectory(dir);
    store.setFileOps(fs.ops(), &fs);

    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C5 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C5 a first run reports NoFile");
    check(engine.applyDeviceState(first, kSr, kBlock, kInCh, kOutCh) ==
              StandaloneAudioEngine::StateApplyStatus::Accepted,
          "C5 the first state committed");
    check(store.save(engine) == StateSaveOutcome::Saved, "C5 the first save succeeded");
    const std::vector<std::uint8_t> baseline = fs.live;

    fs.*(m.flag) = true;
    check(engine.applyDeviceState(second, kSr, kBlock, kInCh, kOutCh) ==
              StandaloneAudioEngine::StateApplyStatus::Accepted,
          "C5 the second state committed");
    check(store.save(engine) == m.want, m.label);
    check(fs.live == baseline, "C5 the failed save left the prior live bytes untouched");
    check(fs.discardCalls > 0, "C5 the aborted temp file was discarded");
    DeviceStateV1 decoded;
    check(core::decode_device_state(fs.live.data(), fs.live.size(), &decoded) && wireEqual(decoded, first),
          "C5 the surviving live bytes still decode to the prior state");
    removeTree(dir);
  }

  // C5.4 — a failed replace must NOT remove the destination first, pinned against the REAL
  // backend: an existing (empty) directory at the live path makes rename() fail, and the
  // destination object must still be there afterwards. A delete-before-replace mutation removes
  // it (and would then even let the rename succeed), so this label goes RED.
  {
    const std::string dir = makeTempDir("c5d");
    AppStateStore store;
    store.setDirectory(dir);
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C5.4 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C5.4 the first run reports NoFile");
    std::error_code ec;
    check(std::filesystem::create_directory(std::filesystem::path(store.livePath()), ec) && !ec,
          "C5.4 an existing destination object was placed at the live path");
    check(store.save(engine) == StateSaveOutcome::ReplaceFailed,
          "C5.4 the real backend reported the replace failure");
    // The SAME destination object must survive, not merely "something exists at that path": a
    // delete-before-replace mutation removes this directory and lets the rename succeed, which
    // would leave a FILE here and satisfy a bare exists() check.
    check(std::filesystem::is_directory(std::filesystem::path(store.livePath())),
          "C5.4 a failed replace did not remove the destination first");
    removeTree(dir);
  }

  // C5.5 — the typed failure must reach the caller, never be swallowed into "saved".
  {
    const std::string dir = makeTempDir("c5s");
    FaultFs fs;
    fs.failReplace = true;
    AppStateStore store;
    store.setDirectory(dir);
    store.setFileOps(fs.ops(), &fs);
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C5.5 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C5.5 the first run reports NoFile");
    check(store.save(engine) == StateSaveOutcome::ReplaceFailed,
          "C5.5 a save failure is reported, not swallowed");
    check(store.lastSaveOutcome() == StateSaveOutcome::ReplaceFailed,
          "C5.5 the typed outcome is retained for the host to record");
    check(fs.writeCalls == 1 && fs.flushCalls == 1 && fs.replaceCalls == 1,
          "C5.5 the failure came from the replace step, not an earlier one");
    removeTree(dir);
  }

  // The lying backend (the mandated interface-violation boundary). A backend that writes half the
  // bytes and reports success is BELOW this layer: save_state_atomic's contract delegates the
  // complete-write check to writeFile ("Return true only on a COMPLETE write"), so the store cannot
  // and does not see through the lie — it reports Saved. What catches the lie is the PUBLISHED FILE:
  // C1.1/C1.2 read the real file back. The short_write_lies control mutates the REAL adapter and
  // therefore reds C1.1/C1.2, not this block. Asserting the honest boundary here (not a false
  // "the store detected it") is the point.
  {
    const std::string dir = makeTempDir("c5w");
    FaultFs fs;
    fs.shortWrite = true;
    AppStateStore store;
    store.setDirectory(dir);
    store.setFileOps(fs.ops(), &fs);
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C5 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C5 a first run reports NoFile");
    check(store.save(engine) == StateSaveOutcome::Saved,
          "C5 a lying backend is reported as a save: the complete-write check is the backend's "
          "contract, not a second store-side truth");
    check(fs.live.size() == kWire / 2u,
          "C5 the lie really did publish a short file (so C1.1/C1.2 on the real adapter are the pin)");
    removeTree(dir);
  }
}

// ---- C8 the exit-save SOURCE (mandate §5) ---------------------------------------------------

static void c8_save_source_precedence() {
  // C8.1 — no owner (a failed prepare released it) but a retained last legal config exists: the
  // exit save must encode THAT, never the power-on default.
  {
    const std::string dir = makeTempDir("c8a");
    DeviceStateV1 session = nonDefaultState();
    session.calibration.vcfLeftTrim = 1.25f;
    AppStateStore store;
    store.setDirectory(dir);
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C8.1 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C8.1 a first run reports NoFile");
    check(engine.applyDeviceState(session, kSr, kBlock, kInCh, kOutCh) ==
              StandaloneAudioEngine::StateApplyStatus::Accepted,
          "C8.1 the session committed a changed config");
    store.captureCanonical(engine);
    check(!engine.prepare(kSeed, 0.0, kBlock, kInCh, kOutCh) &&
              engine.canonicalState() == nullptr && store.hasPending(),
          "C8.1 the illegal format released the owner but kept the retained config");
    check(store.save(engine) == StateSaveOutcome::Saved,
          "C8.1 the retained legal config is the save source when no owner exists");
    std::vector<std::uint8_t> bytes;
    DeviceStateV1 decoded;
    check(readBytes(store.livePath(), &bytes) &&
              core::decode_device_state(bytes.data(), bytes.size(), &decoded) &&
              wireEqual(decoded, session),
          "C8.1 the written file is the retained session config, not the power-on default");
    removeTree(dir);
  }

  // C8.2 — no committed canonical AND no retained legal config: nothing to save, so the exit save
  // must NOT create a file at all (writing the default would be a silent product decision).
  {
    const std::string dir = makeTempDir("c8b");
    AppStateStore store;
    store.setDirectory(dir);
    StandaloneAudioEngine engine;
    check(!engine.prepare(kSeed, 0.0, kBlock, kInCh, kOutCh) &&
              engine.canonicalState() == nullptr,
          "C8.2 no legal config ever existed (the engine is not ready)");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C8.2 a first run reports NoFile");
    check(store.save(engine) == StateSaveOutcome::SkippedNoConfig,
          "C8.2 no legal config means SkippedNoConfig, not a written default");
    check(!std::filesystem::exists(std::filesystem::path(store.livePath())),
          "C8.2 no successful legal config means NO write");
    removeTree(dir);
  }
}

// ---- C6 multi-instance ----------------------------------------------------------------------

static void c6_multi_instance() {
  const std::string dir = makeTempDir("c6");
  DeviceStateV1 a = nonDefaultState();
  DeviceStateV1 b = a;
  b.calibration.vcfRightTrim = 0.875f;

  AppStateStore s1;
  AppStateStore s2;
  s1.setDirectory(dir);
  s2.setDirectory(dir);
  check(s1.tempPath() != s2.tempPath(), "C6.2 two instances use different temp names");
  check(std::filesystem::path(s1.tempPath()).parent_path() ==
            std::filesystem::path(s2.tempPath()).parent_path(),
        "C6.2 both temp names live in the shared directory");

  StandaloneAudioEngine e1;
  StandaloneAudioEngine e2;
  check(e1.prepare(kSeed, kSr, kBlock, kInCh, kOutCh) && e2.prepare(kSeed, kSr, kBlock, kInCh, kOutCh),
        "C6.1 both engines prepared");
  check(s1.loadOnce() == StateLoadOutcome::NoFile && s2.loadOnce() == StateLoadOutcome::NoFile,
        "C6.1 both instances see a first run");
  check(e1.applyDeviceState(a, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C6.1 instance 1 committed A");
  check(e2.applyDeviceState(b, kSr, kBlock, kInCh, kOutCh) ==
            StandaloneAudioEngine::StateApplyStatus::Accepted,
        "C6.1 instance 2 committed B");

  bool alwaysWhole = true;
  for (int i = 0; i < 4; ++i) {
    if (s1.save(e1) != StateSaveOutcome::Saved) alwaysWhole = false;
    std::vector<std::uint8_t> bytes;
    if (!readBytes(s1.livePath(), &bytes) || bytes.size() != kWire) alwaysWhole = false;
    DeviceStateV1 d;
    if (!core::decode_device_state(bytes.data(), bytes.size(), &d) || !wireEqual(d, a)) alwaysWhole = false;
    if (s2.save(e2) != StateSaveOutcome::Saved) alwaysWhole = false;
    if (!readBytes(s2.livePath(), &bytes) || bytes.size() != kWire) alwaysWhole = false;
    if (!core::decode_device_state(bytes.data(), bytes.size(), &d) || !wireEqual(d, b)) alwaysWhole = false;
  }
  check(alwaysWhole, "C6.1 interleaved saves never expose a half-written live file");

  // C6.3/C6.4 — the temp name is CLAIMED, not guessed. steady_clock + an in-process counter cannot
  // guarantee uniqueness across PROCESSES; the kernel's exclusive create is the arbiter (there is no
  // lock service and no lock file).
  {
    const std::string edir = makeTempDir("c6e");
    const std::string taken = edir + "/taken";
    check(writeBytes(taken, std::vector<std::uint8_t>(1u, 0x7Fu)), "C6.3 the colliding path exists");
    check(!host::app_state_file_ops::reserveExclusiveCreate(taken),
          "C6.3 exclusive creation refuses a path another owner already holds");
    const std::string fresh = edir + "/fresh";
    check(host::app_state_file_ops::reserveExclusiveCreate(fresh),
          "C6.3 exclusive creation succeeds on a fresh path");
    check(std::filesystem::exists(std::filesystem::path(fresh)),
          "C6.3 the reservation really created the file");

    AppStateStore reserved;
    reserved.setDirectory(edir);
    const std::string r1 = reserved.reserveTempPath();
    const std::string r2 = reserved.reserveTempPath();
    check(!r1.empty() && !r2.empty() && r1 != r2, "C6.4 reserved temp names are distinct");
    check(std::filesystem::exists(std::filesystem::path(r1)) &&
              std::filesystem::exists(std::filesystem::path(r2)),
          "C6.4 each reserved temp name exists on disk (claimed in the kernel)");
    check(std::filesystem::path(r1).parent_path() ==
              std::filesystem::path(reserved.livePath()).parent_path(),
          "C6.4 the reserved temp lives in the live directory");
    removeTree(edir);
  }

  // C6.5 — the reservation SPANS the write. At the moment the backend is asked to write, the temp
  // path it is handed already exists as the empty file WE claimed, so a plain create could not have
  // been substituted; and the cleanup removes exactly that path, nothing else.
  {
    const std::string pdir = makeTempDir("c6p");
    ReserveProbeFs fs;
    AppStateStore store;
    store.setDirectory(pdir);
    store.setFileOps(fs.ops(), &fs);
    StandaloneAudioEngine engine;
    check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C6.5 the engine prepared");
    check(store.loadOnce() == StateLoadOutcome::NoFile, "C6.5 a first run reports NoFile");
    check(store.save(engine) == StateSaveOutcome::FlushFailed, "C6.5 the save failed at the flush");
    check(fs.existedAtWrite && fs.sizeAtWrite == 0u,
          "C6.5 the temp was ALREADY reserved (empty file) when the backend was asked to write");
    check(std::filesystem::path(fs.seenPath).parent_path() ==
              std::filesystem::path(store.livePath()).parent_path(),
          "C6.5 the reserved temp lives in the live directory");
    check(!std::filesystem::exists(std::filesystem::path(fs.seenPath)),
          "C6.5 the reservation was cleaned up -- exactly the path we took");
    removeTree(pdir);
  }

  removeTree(dir);
}

// ---- C7 the audio path performs no file IO --------------------------------------------------

static void c7_audio_path_is_clean() {
  const std::string dir = makeTempDir("c7");
  CountFs fs;
  AppStateStore store;
  store.setDirectory(dir);
  store.setFileOps(fs.ops(), &fs);
  StandaloneAudioEngine engine;
  check(engine.prepare(kSeed, kSr, kBlock, kInCh, kOutCh), "C7.1 the engine prepared");
  check(store.loadOnce() == StateLoadOutcome::NoFile, "C7.1 the one startup read ran");

  const std::uint64_t readsBefore = store.readAttempts();
  const std::uint64_t writesBefore = store.writeAttempts();
  const int opCallsBefore = fs.calls;

  std::vector<double> in0(256, 0.0), in1(256, 0.0), out0(256, 0.0), out1(256, 0.0), out2(256, 0.0),
      out3(256, 0.0);
  const double* inputs[2] = {in0.data(), in1.data()};
  double* outputs[4] = {out0.data(), out1.data(), out2.data(), out3.data()};
  bool allRendered = true;
  for (int b = 0; b < 32; ++b) {
    if (engine.processBlock(inputs, outputs, kInCh, kOutCh, 256) != StandaloneAudioEngine::Status::Rendered)
      allRendered = false;
  }
  check(allRendered, "C7.1 the rendered blocks went through the production delegate");
  check(store.readAttempts() == readsBefore && store.writeAttempts() == writesBefore,
        "C7.1 rendering performed no store read or write attempt");
  check(fs.calls == opCallsBefore, "C7.1 rendering performed no file operation at all");
  removeTree(dir);
}

int main() {
  c0_fixture_is_legal();
  c1_real_round_trip();
  c2_one_read_and_transfer();
  c3_startup_restore();
  c4_failure_semantics();
  c5_save_failures();
  c8_save_source_precedence();
  c6_multi_instance();
  c7_audio_path_is_clean();
  if (g_fail != 0) {
    std::fprintf(stderr, "[app state store] %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("[app state store] %d checks OK\n", g_checks);
  return 0;
}
