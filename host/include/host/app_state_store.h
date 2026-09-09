// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// app_state_store.h — the narrow APP state-persistence coordination layer (task #105, GH#12).
//
// The ONE agreed chain, with core owning every semantic step:
//     exact-length gate -> decode_device_state -> migrate_device_state -> validate_device_state
//     -> StandaloneAudioEngine::applyDeviceState (the REAL candidate) -> single stopped-stream publish.
//
// This layer owns ONLY the APP-session policy that core deliberately does not:
//   * ONE read attempt per APP session, guarded by an EXPLICIT latch — never inferred from
//     `canonicalState() == nullptr` (that is also true after a failed prepare());
//   * the pending transfer payload, captured BEFORE prepare() releases the owner, so a device
//     reopen can never fall back to the power-on default or re-read the disk;
//   * the lifecycle-save gate: ONLY a missing file or a successfully adopted file may be written.
//     A file that was present but unusable is preserved byte-for-byte and never overwritten;
//   * the real platform FileOps (complete-write check / flush+close result / same-directory
//     atomic replace / best-effort temp cleanup).
//
// It performs NO file IO on the audio path, and it holds NO second editable state bank: once a
// pending restore is published, the engine's canonical state is the single authority.
//
// HONEST BOUNDARY (kept, not widened): this is an exit/lifecycle save, NOT crash recovery, NOT
// real-time debounce and NOT panel auto-save. `flushFile` orders the write to the OS; it is not a
// power-loss guarantee. The wire record carries no magic, no checksum and no length prefix, so the
// host's exact-length gate detects truncation and growth but NOT an in-place bit flip.

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <host/standalone_audio_engine.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/state_migration.h>
#include <lunar24/core/state_persistence.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/core/state_validation.h>

namespace lunar24::host {

using lunar24::core::DeviceStateV1;
using lunar24::core::FileOps;
using lunar24::core::kDeviceStorageSchema;
using lunar24::core::MigrationStatus;
using lunar24::core::SaveResult;
using lunar24::core::StateValidationResult;

// The product file, in the APP's own per-user settings directory. It is a SIBLING of settings.ini,
// never a section inside it: the iPlug2 INI writer owns settings.ini and rewrites it wholesale.
inline constexpr const char* kAppStateFileName = "lunar24-state.bin";
// Temp files live in the SAME directory (the rename must stay intra-volume) and carry a unique
// suffix so two instances never share a temp name. There is no lock service and no lock file.
inline constexpr const char* kAppStateTempStem = "lunar24-state.bin.tmp-";

// The exact wire size this host accepts. Core's decode only rejects `size < totalBytesHint`; the
// APP host is stricter on purpose (an exact-equality gate), so a truncated OR grown file is
// rejected here rather than silently accepted with trailing garbage.
inline constexpr std::size_t kAppStateWireBytes = kDeviceStorageSchema.totalBytesHint;

// A fixed, inspectable outcome of the ONE startup read attempt.
enum class StateLoadOutcome : std::uint8_t {
  NotAttempted = 0,    // no read attempt yet in this APP session
  NoPath,              // no settings directory was handed in: NO read was attempted
  NoFile,              // the file is absent (first run) -> the existing constant seed default boots
  Ok,                  // decoded + migrated + validated; adopted as the pending restore candidate
  Unreadable,          // present but could not be read (permission / IO / it is a directory)
  LengthMismatch,      // not exactly kAppStateWireBytes
  UnsupportedVersion,  // migrate_device_state: schema older/unrecognized (no frozen old wire schema)
  RequiresNewerCodec,  // migrate_device_state: schema newer than this build understands
  InvalidState,        // validate_device_state rejected it (family + field in lastValidation())
  // NOTE: a state that VALIDATES but whose real engine candidate is refused is NOT a load outcome:
  // loadOnce() returns Ok (the file was adopted as a candidate) and publishPending() returns the
  // engine's exact StateApplyStatus (e.g. RejectedGraph). saveAllowed() is false either way.
};

// A fixed, inspectable outcome of a lifecycle (exit) save.
enum class StateSaveOutcome : std::uint8_t {
  NotAttempted = 0,
  Saved = 1,
  SkippedNoPath = 2,           // no settings directory -> never fall back to cwd or another dir
  SkippedNoConfig = 3,         // no committed canonical AND no retained legal config
  SkippedFileUnhealthy = 4,    // a file was present but not adopted -> must NOT be overwritten
  EncodeFailed = 5,            // the canonical state did not encode to the exact wire size
  TempWriteFailed = 6,         // save_state_atomic: live file untouched
  FlushFailed = 7,             // save_state_atomic: live file untouched
  ReplaceFailed = 8,           // save_state_atomic: live file untouched (no delete-before-replace)
};

// The real platform FileOps. Framework-free (no iPlug2), used by the product AND by the acceptance
// test, so the round trip is exercised through the SAME code the APP runs.
namespace app_state_file_ops {

// Complete-write check: a short fwrite is a FAILURE even if the OS accepted the prefix. The close
// result is part of the verdict (a failed close can lose buffered bytes).
inline bool realWriteFile(void* ctx, const char* path, const std::uint8_t* bytes, std::size_t n) {
  (void)ctx;
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const std::size_t wrote = (n == 0) ? 0u : std::fwrite(bytes, 1u, n, f);
  const int closed = std::fclose(f);
  return wrote == n && closed == 0;
}

// Flush the freshly written temp file to the OS. Opened "r+b" so the flush targets the real file
// rather than a second write; the durability boundary above still applies.
inline bool realFlushFile(void* ctx, const char* path) {
  (void)ctx;
  std::FILE* f = std::fopen(path, "r+b");
  if (!f) return false;
  const int flushed = std::fflush(f);
#if defined(_WIN32)
  const int synced = _commit(_fileno(f));
#else
  const int synced = ::fsync(::fileno(f));
#endif
  const int closed = std::fclose(f);
  return flushed == 0 && synced == 0 && closed == 0;
}

// Same-directory atomic replace. std::filesystem::rename maps to MoveFileExW(MOVEFILE_REPLACE_
// EXISTING) on Windows and to rename(2) on POSIX, both of which replace an existing destination.
// The destination is NEVER removed first: a failed replace must leave the prior live file intact.
inline bool realAtomicReplace(void* ctx, const char* from, const char* to) {
  (void)ctx;
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), ec);
  return !ec;
}

// Best-effort cleanup of an aborted temp file. Never touches the live file.
inline void realDiscardFile(void* ctx, const char* path) {
  (void)ctx;
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(path), ec);
}

inline FileOps realOps() {
  FileOps ops;
  ops.writeFile = &realWriteFile;
  ops.flushFile = &realFlushFile;
  ops.atomicReplace = &realAtomicReplace;
  ops.discardFile = &realDiscardFile;
  return ops;
}

}  // namespace app_state_file_ops

class AppStateStore {
 public:
  AppStateStore() : ops_(app_state_file_ops::realOps()) {}
  AppStateStore(const AppStateStore&) = delete;
  AppStateStore& operator=(const AppStateStore&) = delete;

  // ---- injected environment -------------------------------------------------------------------
  // The APP host hands in the ALREADY-RESOLVED per-user settings directory (it must not be
  // re-derived here: one resolution, one truth). Tests hand in a real temporary directory.
  void setDirectory(const std::string& dir) {
    directory_ = dir;
    // Re-arm the session latch only while no read has actually been attempted yet; once the
    // session has read (or decided there is no path), the latch holds for the whole APP session.
    if (readAttempts_ == 0) {
      restoreAttempted_ = false;
      loadOutcome_ = StateLoadOutcome::NotAttempted;
    }
  }
  bool hasDirectory() const { return !directory_.empty(); }
  const std::string& directory() const { return directory_; }
  std::string livePath() const {
    if (directory_.empty()) return std::string();
    std::filesystem::path p(directory_);
    p /= kAppStateFileName;
    return p.string();
  }
  // A fresh, non-colliding temp path in the SAME directory on every call.
  std::string tempPath() const {
    if (directory_.empty()) return std::string();
    static std::atomic<std::uint64_t> counter{0u};
    const auto ticks =
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1u, std::memory_order_relaxed);
    std::filesystem::path p(directory_);
    p /= std::string(kAppStateTempStem) + std::to_string(ticks) + "-" + std::to_string(seq);
    return p.string();
  }

  // Fault injection / call counting for tests. The default is the real platform ops above.
  void setFileOps(const FileOps& ops, void* ctx) {
    ops_ = ops;
    opsCtx_ = ctx;
  }
  void useRealFileOps() {
    ops_ = app_state_file_ops::realOps();
    opsCtx_ = nullptr;
  }

  // ---- the ONE startup read attempt ------------------------------------------------------------
  // Latched: the second and every later call in the same APP session returns the SAME outcome and
  // performs NO disk access (readAttempts() stays 1). The disk is never re-read on a device reopen.
  StateLoadOutcome loadOnce() {
    if (restoreAttempted_) return loadOutcome_;
    restoreAttempted_ = true;

    const std::string live = livePath();
    if (live.empty()) {
      loadOutcome_ = StateLoadOutcome::NoPath;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    ++readAttempts_;
    std::FILE* f = std::fopen(live.c_str(), "rb");
    if (!f) {
      // ENOENT is a genuine first run; anything else (permission, it is a directory, IO) is an
      // EXPLICIT unreadable status and must never masquerade as "no file yet".
      loadOutcome_ =
          (errno == ENOENT) ? StateLoadOutcome::NoFile : StateLoadOutcome::Unreadable;
      saveAllowed_ = (loadOutcome_ == StateLoadOutcome::NoFile);
      return loadOutcome_;
    }

    std::vector<std::uint8_t> bytes;
    bool readOk = true;
    if (std::fseek(f, 0L, SEEK_END) != 0) {
      readOk = false;
    } else {
      const long end = std::ftell(f);
      if (end < 0) {
        readOk = false;
      } else {
        if (std::fseek(f, 0L, SEEK_SET) != 0) {
          readOk = false;
        } else {
          bytes.resize(static_cast<std::size_t>(end));
          if (!bytes.empty()) readOk = (std::fread(bytes.data(), 1u, bytes.size(), f) == bytes.size());
        }
      }
    }
    std::fclose(f);
    if (!readOk) {
      loadOutcome_ = StateLoadOutcome::Unreadable;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    if (bytes.size() != kAppStateWireBytes) {
      loadOutcome_ = StateLoadOutcome::LengthMismatch;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    DeviceStateV1 decoded;
    if (!lunar24::core::decode_device_state(bytes.data(), bytes.size(), &decoded)) {
      loadOutcome_ = StateLoadOutcome::LengthMismatch;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    DeviceStateV1 migrated;
    const auto m = lunar24::core::migrate_device_state(decoded, migrated);
    if (!m.ok) {
      loadOutcome_ = (m.status == MigrationStatus::requires_newer_codec)
                         ? StateLoadOutcome::RequiresNewerCodec
                         : StateLoadOutcome::UnsupportedVersion;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    lastValidation_ = lunar24::core::validate_device_state(migrated);
    if (!lastValidation_.ok) {
      loadOutcome_ = StateLoadOutcome::InvalidState;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    // Validated: adopt it as the pending restore. The caller publishes it through the REAL
    // candidate at the stopped-stream boundary; the engine's verdict lands in publishPending().
    pending_ = migrated;
    pendingOrigin_ = PendingOrigin::FromFile;
    pendingValid_ = true;
    loadOutcome_ = StateLoadOutcome::Ok;
    return loadOutcome_;
  }

  // ---- the stopped-stream transfer payload -----------------------------------------------------
  // Capture the engine's committed canonical BEFORE prepare() releases the owner. A not-ready
  // engine has no owner to transfer, so an existing pending restore is left untouched.
  void captureCanonical(const StandaloneAudioEngine& engine) {
    const DeviceStateV1* canonical = engine.canonicalState();
    if (canonical == nullptr) return;
    pending_ = *canonical;
    pendingOrigin_ = PendingOrigin::FromSession;
    pendingValid_ = true;
  }

  // Publish the pending restore through the engine's ONE real candidate path. On success the
  // pending is cleared (canonical is now the single authority) and the file counts as adopted.
  StandaloneAudioEngine::StateApplyStatus publishPending(StandaloneAudioEngine& engine,
                                                         double sampleRate, int maxBlockSize,
                                                         int inputCapability,
                                                         int outputCapability) {
    if (!pendingValid_) return StandaloneAudioEngine::StateApplyStatus::NotAttempted;

    const DeviceStateV1 candidate = pending_;  // copy: a rejection must leave the store intact
    const auto status = engine.applyDeviceState(candidate, sampleRate, maxBlockSize, inputCapability,
                                                outputCapability);
    lastPublishStatus_ = status;

    if (status == StandaloneAudioEngine::StateApplyStatus::Accepted) {
      pendingValid_ = false;
      pendingOrigin_ = PendingOrigin::None;
      if (loadOutcome_ == StateLoadOutcome::Ok) saveAllowed_ = true;  // the file was adopted
      return status;
    }

    if (pendingOrigin_ == PendingOrigin::FromFile) {
      // The file validated but the engine refuses to run it: the file is NOT adopted, so the
      // lifecycle save must never overwrite it. Keep it inspectable; do not keep re-trying it.
      rejectedCandidate_ = candidate;
      rejectedCandidateValid_ = true;
      pendingValid_ = false;
      pendingOrigin_ = PendingOrigin::None;
      saveAllowed_ = false;
    }
    // FromSession: keep the pending so the NEXT legal boundary can re-publish it; the save gate is
    // whatever the adopted-file verdict already decided.
    return status;
  }

  // ---- the lifecycle (exit) save ---------------------------------------------------------------
  // Source precedence: the engine's committed canonical state; failing that, the retained last
  // legal config. Never a fresh disk read, never the power-on default when a session existed.
  StateSaveOutcome save(const StandaloneAudioEngine& engine) {
    lastSaveOutcome_ = saveImpl(engine);
    return lastSaveOutcome_;
  }

  // ---- inspectable -----------------------------------------------------------------------------
  bool restoreAttempted() const { return restoreAttempted_; }
  StateLoadOutcome loadOutcome() const { return loadOutcome_; }
  StateSaveOutcome lastSaveOutcome() const { return lastSaveOutcome_; }
  bool saveAllowed() const { return saveAllowed_; }
  bool hasPending() const { return pendingValid_; }
  const DeviceStateV1* pending() const { return pendingValid_ ? &pending_ : nullptr; }
  bool hasRejectedCandidate() const { return rejectedCandidateValid_; }
  const DeviceStateV1* rejectedCandidate() const {
    return rejectedCandidateValid_ ? &rejectedCandidate_ : nullptr;
  }
  const StateValidationResult& lastValidation() const { return lastValidation_; }
  StandaloneAudioEngine::StateApplyStatus lastPublishStatus() const { return lastPublishStatus_; }
  std::uint64_t readAttempts() const { return readAttempts_; }
  std::uint64_t writeAttempts() const { return writeAttempts_; }
  const FileOps& fileOps() const { return ops_; }

 private:
  StateSaveOutcome saveImpl(const StandaloneAudioEngine& engine) {
    if (!hasDirectory()) return StateSaveOutcome::SkippedNoPath;
    if (!saveAllowed_) return StateSaveOutcome::SkippedFileUnhealthy;

    const DeviceStateV1* source = engine.canonicalState();
    if (source == nullptr) source = pendingValid_ ? &pending_ : nullptr;
    if (source == nullptr) return StateSaveOutcome::SkippedNoConfig;

    std::vector<std::uint8_t> wire(kAppStateWireBytes, 0u);
    std::size_t written = 0u;
    if (!lunar24::core::encode_device_state(*source, wire.data(), wire.size(), &written) ||
        written != kAppStateWireBytes)
      return StateSaveOutcome::EncodeFailed;

    ++writeAttempts_;
    const std::string live = livePath();
    const std::string temp = tempPath();
    const SaveResult result = lunar24::core::save_state_atomic(wire.data(), written, temp.c_str(),
                                                              live.c_str(), ops_, opsCtx_);
    switch (result) {
      case SaveResult::ok: return StateSaveOutcome::Saved;
      case SaveResult::flush_failed: return StateSaveOutcome::FlushFailed;
      case SaveResult::replace_failed: return StateSaveOutcome::ReplaceFailed;
      case SaveResult::temp_write_failed: break;
    }
    return StateSaveOutcome::TempWriteFailed;
  }

  enum class PendingOrigin : std::uint8_t { None = 0, FromFile, FromSession };

  std::string directory_;
  FileOps ops_{};
  void* opsCtx_ = nullptr;

  bool restoreAttempted_ = false;
  StateLoadOutcome loadOutcome_ = StateLoadOutcome::NotAttempted;
  StateSaveOutcome lastSaveOutcome_ = StateSaveOutcome::NotAttempted;
  bool saveAllowed_ = false;

  bool pendingValid_ = false;
  PendingOrigin pendingOrigin_ = PendingOrigin::None;
  DeviceStateV1 pending_{};

  bool rejectedCandidateValid_ = false;
  DeviceStateV1 rejectedCandidate_{};

  StateValidationResult lastValidation_{};
  StandaloneAudioEngine::StateApplyStatus lastPublishStatus_ =
      StandaloneAudioEngine::StateApplyStatus::NotAttempted;

  std::uint64_t readAttempts_ = 0u;
  std::uint64_t writeAttempts_ = 0u;
};

}  // namespace lunar24::host
