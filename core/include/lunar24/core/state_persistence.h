// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// StatePersistence (design/07 §6, master plan line 93): write-disk debounce +
// atomic state save. The control thread takes a consistent snapshot, debounces it
// (coalescing rapid changes, never dropping the final state), writes a TEMP file,
// flushes it, then ATOMICALLY renames it over the live file. The audio thread
// never participates in disk saving.
//
// This is the P2-⑤ Half 2 (persistence) deliverable. The FILE SYSTEM calls are
// delegated to an injectable FileOps backend so the order/fail-safety logic is a
// framework-free, testable core property and a later platform layer supplies the
// real fopen/fflush/rename. Because core/ cannot depend on a filesystem layer
// (forbidden includes keep it framework-free), the exact FS ops are function
// pointers; the ORDER and FAIL-SAFETY here are what the tests prove.
//
// Honest boundary (per mandate): the temp->flush->rename ORDER and the
// interruption visibility are provable here; true power-off durability (fsync),
// which the unistd.h include ban keeps out of core, is NOT — so this file never
// claims "crash-safe".

#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar24::core {

// A debounce for write-disk requests: rapid state changes coalesce into a single
// real write, but the FINAL state is never dropped (a trailing change can always
// be flushed). Pure throttle logic — no file side effects.
class StateSaveDebounce {
 public:
  explicit StateSaveDebounce(std::uint64_t debounceMs = 250u) : windowMs_(debounceMs) {}

  // A state change happened at `nowMs`. Returns true if the caller should write to
  // disk NOW (the debounce window elapsed since the last write). A burst within a
  // window returns true for at most the first one; the rest are held.
  bool markChanged(std::uint64_t nowMs) {
    dirty_ = true;
    if (!everWrote_ || nowMs - lastWriteMs_ >= windowMs_) {
      ++writeEmitCount_;
      lastWriteMs_ = nowMs;
      dirty_ = false;
      everWrote_ = true;
      return true;
    }
    return false;
  }

  // Emit the pending dirty state no matter how recent — the final write is never
  // lost to the debounce window. Returns true if a write was emitted.
  bool flushDirty(std::uint64_t nowMs) {
    if (!dirty_) return false;
    ++writeEmitCount_;
    lastWriteMs_ = nowMs;
    dirty_ = false;
    everWrote_ = true;
    return true;
  }

  // Let the caller record a real disk write it performed (so a test can tally the
  // actual writeFile invocations, not the debounce decisions).
  void noteWritten() { ++writeCount_; }
  // Number of COMPLETED disk writes (the caller increments via noteWritten()).
  std::uint64_t writeCount() const { return writeCount_; }
  bool dirty() const { return dirty_; }

 private:
  std::uint64_t windowMs_ = 250u;
  std::uint64_t lastWriteMs_ = 0u;
  bool everWrote_ = false;
  bool dirty_ = false;
  std::uint64_t writeEmitCount_ = 0u;
  std::uint64_t writeCount_ = 0u;
};

// The result of an atomic save attempt.
enum class SaveResult : std::uint8_t {
  ok = 0,
  temp_write_failed = 1,  // failed before any rename: live file untouched
  flush_failed = 2,       // failed before any rename: live file untouched
  replace_failed = 3,     // rename failed: temp kept/discarded, live file untouched
};

// File-system operations the save logic drives. Function pointers let the test
// inject faults (partial write, dropped rename, in-place write) or count calls —
// the real platform layer fills these in with fopen/fflush/rename later.
struct FileOps {
  // Write `bytes[0..n)` to `path`. Return true only on a COMPLETE write; a
  // backend may fail partway (partial write) to simulate a fault.
  bool (*writeFile)(void* ctx, const char* path, const std::uint8_t* bytes,
                    std::size_t n) = nullptr;
  // Flush a freshly written file's buffers to storage. Order-only: true durability
  // (fsync) is not provable in this environment — see the honest boundary above.
  bool (*flushFile)(void* ctx, const char* path) = nullptr;
  // Atomically replace `to` by `from` (rename within the same directory/volume).
  bool (*atomicReplace)(void* ctx, const char* from, const char* to) = nullptr;
  // Best-effort discard of an aborted temp file (on a pre-replace failure).
  void (*discardFile)(void* ctx, const char* path) = nullptr;
};

// Save `bytes[0..n)` as the live state at `livePath`, via temp-flush-rename.
// If the temp write or flush fails, the temp is discarded and the live file is
// UNTOUCHED — a half-written file can never become visible. Returns the outcome.
inline SaveResult save_state_atomic(const std::uint8_t* bytes, std::size_t n,
                                    const char* tempPath, const char* livePath,
                                    const FileOps& ops, void* ctx) {
  if (!ops.writeFile || !ops.flushFile || !ops.atomicReplace || !ops.discardFile)
    return SaveResult::temp_write_failed;
  if (!ops.writeFile(ctx, tempPath, bytes, n)) {
    ops.discardFile(ctx, tempPath);
    return SaveResult::temp_write_failed;
  }
  if (!ops.flushFile(ctx, tempPath)) {
    ops.discardFile(ctx, tempPath);
    return SaveResult::flush_failed;
  }
  if (!ops.atomicReplace(ctx, tempPath, livePath)) {
    ops.discardFile(ctx, tempPath);
    return SaveResult::replace_failed;
  }
  return SaveResult::ok;
}

}  // namespace lunar24::core
