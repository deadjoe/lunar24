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
//   * the lifecycle-save gate: ONLY a missing file or a file the REAL candidate ADOPTED may be
//     written. A file that was present but not adopted (bad format, IO failure, or a graph the
//     engine refuses) is preserved byte-for-byte and never overwritten, and that verdict is STICKY
//     for the whole session: a later successful FromSession publish (the default config after a
//     device reopen) must never re-open the gate over a file this session failed to adopt;
//   * the real platform FileOps (complete-write check / flush+close result / same-directory
//     atomic replace / best-effort temp cleanup), with the temp name claimed by EXCLUSIVE creation
//     (O_CREAT|O_EXCL) so two PROCESSES cannot share it -- there is no lock service.
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
#include <string>
#include <vector>

#if defined(_WIN32)
// Every file call below is WIDE: the UTF-8 the host hands in is converted ONCE, at this boundary
// (nativePath). WIN32_LEAN_AND_MEAN / NOMINMAX keep <windows.h> from dragging the rarely-used
// headers and the min/max macros into every TU that includes this store.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
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
// suffix. Uniqueness across PROCESSES is not assumed from the name: the name is claimed by exclusive
// creation. There is no lock service and no lock file.
inline constexpr const char* kAppStateTempStem = "lunar24-state.bin.tmp-";
// How many fresh candidate names to try when another process wins the exclusive-create race.
inline constexpr int kTempReserveAttempts = 16;

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

#if defined(_WIN32)
// The native path form the Windows file APIs take.
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

// ---- the ONE path-encoding boundary ----------------------------------------------------------
// The APP host resolves the per-user directory as UTF-8 (SHGetSpecialFolderPathUTF8) and the store
// keeps it as UTF-8 end to end. POSIX paths ARE byte strings, so the UTF-8 bytes pass through
// unchanged; Windows needs UTF-16 for the wide file APIs and must NOT go through the process ANSI
// code page -- a Chinese user name is not representable in a legacy code page, and "it happens to
// work on an English machine" is exactly the defect this boundary removes. Invalid UTF-8 yields an
// empty native path, which every caller below turns into a typed failure rather than a guess.
inline NativePath nativePath(const std::string& utf8) {
#if defined(_WIN32)
  if (utf8.empty()) return std::wstring();
  const int len = static_cast<int>(utf8.size());
  const int need =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(need), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, &wide[0], need) != need)
    return std::wstring();
  return wide;
#else
  return utf8;
#endif
}

// std::FILE* on the SAME native path every other call in this namespace uses, so a UTF-8 path can
// never reach the narrow fopen by accident on Windows.
inline std::FILE* openNative(const std::string& utf8Path, const char* mode) {
#if defined(_WIN32)
  const NativePath native = nativePath(utf8Path);
  if (native.empty()) return nullptr;
  std::wstring wideMode;
  for (const char* p = mode; p != nullptr && *p != '\0'; ++p)
    wideMode.push_back(static_cast<wchar_t>(*p));
  // _wfopen_s, not _wfopen: MSVC deprecates the latter (C4996), which this repo's /WX policy
  // promotes to an error, and its errno_t result drops straight into the same typed-failure path as
  // the _wsopen_s create below -- one secure CRT call shape for every wide open, no suppression.
  std::FILE* fp = nullptr;
  if (::_wfopen_s(&fp, native.c_str(), wideMode.c_str()) != 0) return nullptr;
  return fp;
#else
  // POSIX paths ARE byte strings, so nativePath() is the identity here and this is the same fopen on
  // the same bytes. The indirection is deliberate: ONE boundary on BOTH platforms, so a conversion
  // defect cannot hide behind a second, unconverted call site.
  const NativePath native = nativePath(utf8Path);
  return std::fopen(native.c_str(), mode);
#endif
}

// Join a directory and a file name in UTF-8 WITHOUT a std::filesystem narrow round trip: on Windows
// path(std::string) / path::string() go through the ANSI code page, which would corrupt a non-ASCII
// directory before the wide conversion ever ran. A trailing separator is dropped so the result is
// always `dir + "/" + name`.
inline std::string joinUtf8(const std::string& dir, const std::string& name) {
  std::size_t n = dir.size();
  while (n > 0u && (dir[n - 1u] == '/' || dir[n - 1u] == '\\')) --n;
  std::string out = dir.substr(0u, n);
  out.push_back('/');
  out += name;
  return out;
}

// Claim `path` by EXCLUSIVE creation (O_CREAT|O_EXCL). steady_clock + an in-process counter cannot
// guarantee uniqueness across PROCESSES; the kernel's exclusive create is the only portable arbiter,
// and it is what the store uses instead of a lock service or lock file. Returns false when the name
// is already taken (another instance won) or the directory is unusable.
inline bool reserveExclusiveCreate(const std::string& path) {
#if defined(_WIN32)
  const NativePath native = nativePath(path);
  if (native.empty()) return false;
  int fd = -1;
  if (_wsopen_s(&fd, native.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW,
                _S_IREAD | _S_IWRITE) != 0)
    return false;
  _close(fd);
  return true;
#else
  const NativePath native = nativePath(path);
  const int fd = ::open(native.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
  if (fd < 0) return false;
  ::close(fd);
  return true;
#endif
}

// st_mode -> "is a regular file", portable across the two stat flavours the loader uses.
inline bool statModeIsRegularFile(int mode) {
#if defined(_WIN32)
  return (mode & _S_IFMT) == _S_IFREG;
#else
  return S_ISREG(mode);
#endif
}

// Write into an ALREADY-RESERVED temp file. The temp name is claimed by reserveExclusiveCreate
// BEFORE this call, so this NEVER creates a file: "r+b" fails when the reservation is missing, which
// is what makes the exclusivity span the write instead of being a pre-check a plain create could
// bypass. The file is truncated to exactly the record length first, so a stale longer temp can never
// leave trailing bytes. Complete-write check: a short fwrite is a FAILURE even if the OS accepted the
// prefix. The close result is part of the verdict (a failed close can lose buffered bytes).
inline bool realWriteFile(void* ctx, const char* path, const std::uint8_t* bytes, std::size_t n) {
  (void)ctx;
  std::FILE* f = openNative(path, "r+b");
  if (!f) return false;
#if defined(_WIN32)
  if (_chsize_s(_fileno(f), 0) != 0) {
    std::fclose(f);
    return false;
  }
#else
  if (::ftruncate(::fileno(f), 0) != 0) {
    std::fclose(f);
    return false;
  }
#endif
  const std::size_t wrote = (n == 0) ? 0u : std::fwrite(bytes, 1u, n, f);
  const int closed = std::fclose(f);
  return wrote == n && closed == 0;
}

// Flush the freshly written temp file to the OS. Opened "r+b" so the flush targets the real file
// rather than a second write; the durability boundary above still applies.
inline bool realFlushFile(void* ctx, const char* path) {
  (void)ctx;
  std::FILE* f = openNative(path, "r+b");
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

// Same-directory atomic replace. On Windows this is MoveFileExW(MOVEFILE_REPLACE_EXISTING) on the
// SAME native (wide) paths the write used; on POSIX it is rename(2). Both replace an existing
// destination. MOVEFILE_COPY_ALLOWED is deliberately NOT set: a cross-volume move would silently
// degrade into copy-then-delete -- not atomic, and a failed copy can destroy the destination -- so a
// cross-volume temp is a typed ReplaceFailed instead. The destination is NEVER removed first: a
// failed replace must leave the prior live file intact.
inline bool realAtomicReplace(void* ctx, const char* from, const char* to) {
  (void)ctx;
#if defined(_WIN32)
  const NativePath fromNative = nativePath(from);
  const NativePath toNative = nativePath(to);
  if (fromNative.empty() || toNative.empty()) return false;
  return ::MoveFileExW(fromNative.c_str(), toNative.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  // POSIX paths ARE byte strings: rename(2) takes the UTF-8 bytes unchanged (nativePath() is the
  // identity). No std::filesystem anywhere in this file, so no narrow/ACP round trip can creep in on
  // either platform.
  const NativePath fromNative = nativePath(from);
  const NativePath toNative = nativePath(to);
  return ::rename(fromNative.c_str(), toNative.c_str()) == 0;
#endif
}

// Best-effort cleanup of an aborted temp file. Never touches the live file. DeleteFileW on the same
// native path form; POSIX removes the byte path.
inline void realDiscardFile(void* ctx, const char* path) {
  (void)ctx;
#if defined(_WIN32)
  const NativePath native = nativePath(path);
  if (!native.empty()) (void)::DeleteFileW(native.c_str());
#else
  const NativePath native = nativePath(path);
  (void)::remove(native.c_str());
#endif
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
    return app_state_file_ops::joinUtf8(directory_, kAppStateFileName);
  }
  // A fresh, non-colliding temp CANDIDATE path in the SAME directory on every call. This only
  // generates a name; it does not claim it. reserveTempPath() is what makes the name ours.
  std::string tempPath() const {
    if (directory_.empty()) return std::string();
    static std::atomic<std::uint64_t> counter{0u};
    const auto ticks =
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1u, std::memory_order_relaxed);
    return app_state_file_ops::joinUtf8(
        directory_,
        std::string(kAppStateTempStem) + std::to_string(ticks) + "-" + std::to_string(seq));
  }

  // Reserve a temp path in the SAME directory by EXCLUSIVE creation, retrying with a fresh candidate
  // name when another PROCESS won the race. Returns "" when no name could be claimed; the caller then
  // reports a typed failure and the live file is untouched. Only the returned path is ever cleaned
  // up -- never a pattern, never another instance's file.
  std::string reserveTempPath() const {
    if (directory_.empty()) return std::string();
    for (int attempt = 0; attempt < kTempReserveAttempts; ++attempt) {
      const std::string candidate = tempPath();
      if (app_state_file_ops::reserveExclusiveCreate(candidate)) return candidate;
    }
    return std::string();
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

  // The read boundary, exposed so the boundary itself is directly testable: read AT MOST the exact
  // wire size from an already-open record and accept it only when it is EXACTLY that long. A short
  // read fails, and so does a byte BEYOND the record — the file may have grown after the fstat size
  // gate, which is a snapshot, not a lock, so the size gate alone never decides completeness.
  // `*ioError` distinguishes a read error (Unreadable) from a length problem.
  static bool readExactRecord(std::FILE* f, std::vector<std::uint8_t>* out,
                              std::uint64_t* bytesRead, bool* ioError) {
    out->assign(kAppStateWireBytes, 0u);
    const std::size_t got = std::fread(out->data(), 1u, out->size(), f);
    *bytesRead += got;
    const int extra = (got == out->size()) ? std::fgetc(f) : EOF;
    *ioError = std::ferror(f) != 0;
    return !*ioError && got == out->size() && extra == EOF;
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
    std::FILE* f = app_state_file_ops::openNative(live, "rb");
    if (!f) {
      // ENOENT is a genuine first run; anything else (permission, it is a directory, IO) is an
      // EXPLICIT unreadable status and must never masquerade as "no file yet".
      loadOutcome_ =
          (errno == ENOENT) ? StateLoadOutcome::NoFile : StateLoadOutcome::Unreadable;
      fileUnadopted_ = (loadOutcome_ != StateLoadOutcome::NoFile);
      saveAllowed_ = (loadOutcome_ == StateLoadOutcome::NoFile);
      return loadOutcome_;
    }

    // Size gate BEFORE any allocation, taken from the OPENED descriptor: fstat is a snapshot of THIS
    // file object (a path-based size can be swapped underneath us between check and read), and it
    // classifies a non-regular path -- the file IS a directory -- as such instead of as a length
    // problem. An oversized record must fail as a typed LengthMismatch without first pulling the
    // whole file into memory.
#if defined(_WIN32)
    struct _stat64 st;
    const int statRc = _fstat64(_fileno(f), &st);
#else
    struct stat st;
    const int statRc = ::fstat(::fileno(f), &st);
#endif
    if (statRc != 0 || !app_state_file_ops::statModeIsRegularFile(static_cast<int>(st.st_mode))) {
      std::fclose(f);
      loadOutcome_ = StateLoadOutcome::Unreadable;
      fileUnadopted_ = true;
      saveAllowed_ = false;
      return loadOutcome_;
    }
    if (static_cast<std::uintmax_t>(st.st_size) !=
        static_cast<std::uintmax_t>(kAppStateWireBytes)) {
      std::fclose(f);
      loadOutcome_ = StateLoadOutcome::LengthMismatch;
      fileUnadopted_ = true;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    std::vector<std::uint8_t> bytes;
    bool ioError = false;
    const bool exact = readExactRecord(f, &bytes, &bytesRead_, &ioError);
    std::fclose(f);
    if (!exact) {
      loadOutcome_ = ioError ? StateLoadOutcome::Unreadable : StateLoadOutcome::LengthMismatch;
      fileUnadopted_ = true;
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
      fileUnadopted_ = true;
      saveAllowed_ = false;
      return loadOutcome_;
    }

    lastValidation_ = lunar24::core::validate_device_state(migrated);
    if (!lastValidation_.ok) {
      loadOutcome_ = StateLoadOutcome::InvalidState;
      fileUnadopted_ = true;
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
    const PendingOrigin origin = pendingOrigin_;
    const auto status = engine.applyDeviceState(candidate, sampleRate, maxBlockSize, inputCapability,
                                                outputCapability);
    lastPublishStatus_ = status;

    if (status == StandaloneAudioEngine::StateApplyStatus::Accepted) {
      pendingValid_ = false;
      pendingOrigin_ = PendingOrigin::None;
      // ONLY a publish that came FROM THE FILE can lift the protection, and only if this session has
      // not already judged that file unadoptable. A FromSession publish (the default config after a
      // device reopen) must NEVER re-open the exit save over a file we failed to adopt.
      if (origin == PendingOrigin::FromFile && !fileUnadopted_) {
        saveAllowed_ = true;  // the file was adopted by the REAL candidate
      }
      return status;
    }

    if (origin == PendingOrigin::FromFile) {
      // The file validated but the engine refuses to run it: the file is NOT adopted, so the
      // lifecycle save must never overwrite it. Keep it inspectable; do not keep re-trying it. The
      // verdict is STICKY for the rest of the session.
      rejectedCandidate_ = candidate;
      rejectedCandidateValid_ = true;
      pendingValid_ = false;
      pendingOrigin_ = PendingOrigin::None;
      fileUnadopted_ = true;
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
  // STICKY for the session: a file was present and this session did NOT adopt it. Once true, no
  // later publish (however successful) can re-open the exit save over that file.
  bool fileUnadopted() const { return fileUnadopted_; }
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
  // Bytes actually pulled off the disk by the ONE read attempt. A rejected-by-size record must leave
  // this at 0: the size gate runs BEFORE any allocation or read.
  std::uint64_t bytesRead() const { return bytesRead_; }
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
    // Claim the temp name by EXCLUSIVE creation before any bytes are written. The kernel is the only
    // cross-process arbiter here (no lock service, no lock file); the reservation also spans the
    // write, because realWriteFile refuses a temp it did not reserve.
    const std::string temp = reserveTempPath();
    if (temp.empty()) return StateSaveOutcome::TempWriteFailed;
    const SaveResult result = lunar24::core::save_state_atomic(wire.data(), written, temp.c_str(),
                                                              live.c_str(), ops_, opsCtx_);
    if (result != SaveResult::ok) {
      // The backend's discardFile is the backend's own cleanup; this only drops the reservation WE
      // took, and only for the exact path we reserved (never a pattern, never another instance's).
      app_state_file_ops::realDiscardFile(nullptr, temp.c_str());
    }
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
  // MONOTONIC protection: set when a present file is not adopted (bad format, IO failure, or a graph
  // the real candidate refuses). Never cleared in this session -- that is the whole point.
  bool fileUnadopted_ = false;

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
  std::uint64_t bytesRead_ = 0u;
};

}  // namespace lunar24::host
