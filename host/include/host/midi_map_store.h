// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// midi_map_store.h — the MIDI bindings file: load once at startup, save on change.
//
// Bindings are the user's controller rig, NOT the machine's sound state, so they live
// in their OWN product file (a sibling of settings.ini and lunar24-state.bin) — a
// preset load or a panel restore can never re-route the controller.
//
// The file discipline mirrors AppStateStore: the temp name is claimed by exclusive
// creation, the write is complete-checked, flushed and atomically replaced, and a
// file that failed to load is PRESERVED byte-for-byte (never overwritten) for the
// whole session. All IO is UI-thread; the audio thread never participates.

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <host/app_state_store.h>  // app_state_file_ops::{openNative, joinUtf8, reserveExclusiveCreate, realOps}
#include <lunar24/core/midi_map.h>

namespace lunar24::host {

inline constexpr const char* kMidiMapFileName = "lunar24-midi-map.bin";
inline constexpr const char* kMidiMapTempStem = "lunar24-midi-map.bin.tmp-";

enum class MidiMapLoadOutcome : std::uint8_t {
  NotAttempted = 0,
  NoPath,      // no settings directory was handed in
  NoFile,      // first run: the map starts empty
  Ok,          // decoded and adopted
  Unreadable,  // present but could not be read (permission / IO / it is a directory)
  Malformed,   // present but failed decode (bad magic/version/length/field)
};

// Owns the on-disk file and the ONE in-memory map the UI edits. Not the audio path.
class MidiMapStore {
 public:
  MidiMapStore() : ops_(app_state_file_ops::realOps()) {}
  MidiMapStore(const MidiMapStore&) = delete;
  MidiMapStore& operator=(const MidiMapStore&) = delete;

  void setDirectory(const std::string& dir) { directory_ = dir; }
  const std::string& directory() const { return directory_; }

  std::string livePath() const {
    if (directory_.empty()) return std::string();
    return app_state_file_ops::joinUtf8(directory_, kMidiMapFileName);
  }

  const core::MidiMap& map() const { return map_; }
  // The rig-level settings stored alongside the bindings (channel filter, octave
  // shift, velocity curve). Invalid edits are refused (false).
  const core::MidiRigSettings& settings() const { return settings_; }
  bool setSettings(const core::MidiRigSettings& s) {
    if (!core::midi_rig_settings_valid(s)) return false;
    settings_ = s;
    return true;
  }
  MidiMapLoadOutcome loadOutcome() const { return loadOutcome_; }

  // Read + decode + adopt. Never touches the file on failure.
  MidiMapLoadOutcome load() {
    loadAttempted_ = true;
    const std::string live = livePath();
    if (live.empty()) {
      saveAllowed_ = false;
      return loadOutcome_ = MidiMapLoadOutcome::NoPath;
    }
    std::FILE* f = app_state_file_ops::openNative(live, "rb");
    if (f == nullptr) {
      // ENOENT is a genuine first run; anything else must not masquerade as it.
      loadOutcome_ = (errno == ENOENT) ? MidiMapLoadOutcome::NoFile : MidiMapLoadOutcome::Unreadable;
      saveAllowed_ = (loadOutcome_ == MidiMapLoadOutcome::NoFile);
      return loadOutcome_;
    }
    // Size gate from the OPENED descriptor, before any read: the biggest legal record
    // is a full map; anything larger is malformed without pulling it into memory.
#if defined(_WIN32)
    struct _stat64 st;
    const int statRc = _fstat64(_fileno(f), &st);
#else
    struct stat st;
    const int statRc = ::fstat(::fileno(f), &st);
#endif
    if (statRc != 0 || !app_state_file_ops::statModeIsRegularFile(static_cast<int>(st.st_mode)) ||
        static_cast<std::uint64_t>(st.st_size) > core::midi_map_wire_bytes(core::kMidiMapCapacity)) {
      std::fclose(f);
      saveAllowed_ = false;
      return loadOutcome_ = MidiMapLoadOutcome::Malformed;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st.st_size));
    const std::size_t got =
        bytes.empty() ? 0u : std::fread(bytes.data(), 1u, bytes.size(), f);
    const int extra = std::fgetc(f);  // a byte beyond the declared size rejects too
    const bool ioError = std::ferror(f) != 0;
    std::fclose(f);
    core::MidiMap decoded;
    core::MidiRigSettings decodedSettings;
    if (ioError || got != bytes.size() || extra != EOF ||
        !core::midi_map_decode(bytes.data(), bytes.size(), &decoded, &decodedSettings)) {
      saveAllowed_ = false;
      return loadOutcome_ = MidiMapLoadOutcome::Malformed;
    }
    map_ = decoded;
    settings_ = decodedSettings;
    saveAllowed_ = true;
    return loadOutcome_ = MidiMapLoadOutcome::Ok;
  }

  // Encode + atomic save. Skipped (false) when no load was attempted yet (never
  // overwrite an unseen file) or when the file was present but could not be adopted
  // this session (a foreign/corrupt map is preserved byte-for-byte).
  bool save() {
    if (!loadAttempted_ || !saveAllowed_ || directory_.empty()) return false;
    std::vector<std::uint8_t> bytes(core::midi_map_wire_bytes(map_.count()));
    const std::size_t wrote = core::midi_map_encode(map_, settings_, bytes.data(), bytes.size());
    if (wrote != bytes.size()) return false;
    const std::string temp = reserveTempPath();
    if (temp.empty()) return false;
    return core::save_state_atomic(bytes.data(), bytes.size(), temp.c_str(), livePath().c_str(),
                                   ops_, opsCtx_) == core::SaveResult::ok;
  }

  // The edit surface the UI uses: every mutation is followed by save() by the caller.
  bool bind(const core::MidiBinding& b) { return map_.bind(b); }
  bool unbind(const core::MidiBindingKey& key) { return map_.unbind(key); }

  // Fault injection / call counting for tests.
  void setFileOps(const core::FileOps& ops, void* ctx) { ops_ = ops; opsCtx_ = ctx; }

 private:
  std::string reserveTempPath() const {
    if (directory_.empty()) return std::string();
    static std::atomic<std::uint64_t> counter{0u};
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto ticks = static_cast<unsigned long long>(
          std::chrono::steady_clock::now().time_since_epoch().count());
      const auto seq = counter.fetch_add(1u, std::memory_order_relaxed);
      const std::string candidate = app_state_file_ops::joinUtf8(
          directory_,
          std::string(kMidiMapTempStem) + std::to_string(ticks) + "-" + std::to_string(seq));
      if (app_state_file_ops::reserveExclusiveCreate(candidate)) return candidate;
    }
    return std::string();
  }

  core::MidiMap map_;
  core::MidiRigSettings settings_;
  std::string directory_;
  core::FileOps ops_{};
  void* opsCtx_ = nullptr;
  MidiMapLoadOutcome loadOutcome_ = MidiMapLoadOutcome::NotAttempted;
  bool loadAttempted_ = false;
  bool saveAllowed_ = true;  // empty directory/no file: a fresh file may be created
};

}  // namespace lunar24::host
