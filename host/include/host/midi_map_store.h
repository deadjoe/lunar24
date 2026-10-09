// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <host/app_state_store.h>
#include <lunar24/core/midi_map.h>
#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace lunar24::host {

inline constexpr std::size_t kMidiProfileNameBytes = 40;
inline constexpr std::size_t kMidiProfileHeaderBytes = 8 + kMidiProfileNameBytes;
inline constexpr std::size_t kMidiProfileMaxBytes =
    kMidiProfileHeaderBytes + core::midi_map_wire_bytes(core::kMidiMapCapacity);

enum class MidiMapLoadOutcome : std::uint8_t { NotAttempted, NoPath, NoFile, Ok, Unreadable, Malformed };
struct MidiProfileInfo {
  std::string id, name;
  std::uint32_t bindings = 0;
  bool readable = true;
};

namespace midi_profile_files {
// UI-thread transactions only. A crashed process releases the OS lock automatically.
class Lock {
 public:
  explicit Lock(const std::string& path) {
    file_ = app_state_file_ops::openNative(path, "a+b");
    if (!file_) return;
#if defined(_WIN32)
    handle_ = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_)));
    locked_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                         0, 1, 0, &overlap_) != 0;
#else
    locked_ = ::flock(::fileno(file_), LOCK_EX | LOCK_NB) == 0;
#endif
  }
  ~Lock() {
    if (!file_) return;
#if defined(_WIN32)
    if (locked_) UnlockFileEx(handle_, 0, 1, 0, &overlap_);
#else
    if (locked_) (void)::flock(::fileno(file_), LOCK_UN);
#endif
    std::fclose(file_);
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  explicit operator bool() const { return locked_; }
 private:
  std::FILE* file_ = nullptr;
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlap_{};
#endif
};

inline bool read(const std::string& path, std::vector<std::uint8_t>& bytes,
                 std::size_t limit = kMidiProfileMaxBytes) {
  auto* f = app_state_file_ops::openNative(path, "rb");
  if (!f) return false;
#if defined(_WIN32)
  struct _stat64 st{};
  const int rc = _fstat64(_fileno(f), &st);
#else
  struct stat st{};
  const int rc = ::fstat(::fileno(f), &st);
#endif
  if (rc != 0 || !app_state_file_ops::statModeIsRegularFile(static_cast<int>(st.st_mode)) ||
      st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > limit) {
    std::fclose(f);
    return false;
  }
  bytes.resize(static_cast<std::size_t>(st.st_size));
  const auto got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
  const int extra = std::fgetc(f);
  const bool ok = got == bytes.size() && extra == EOF && !std::ferror(f);
  std::fclose(f);
  return ok;
}
inline bool validName(const std::string& name) {
  return !name.empty() && name.size() < kMidiProfileNameBytes &&
         name.front() != ' ' && name.back() != ' ' &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
inline bool validId(const std::string& id) {
  return id.size() > 2 && id.size() <= 64 && id[0] == 'p' &&
         std::all_of(id.begin() + 1, id.end(), [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
}
inline std::string folded(std::string name) {
  for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return name;
}
inline std::vector<std::uint8_t> encode(const std::string& name, const core::MidiMap& map,
                                       const core::MidiRigSettings& settings) {
  std::vector<std::uint8_t> bytes(kMidiProfileHeaderBytes + core::midi_map_wire_bytes(map.count()), 0);
  std::memcpy(bytes.data(), "L24P", 4);
  core::put_u32le(bytes.data() + 4, 1);
  std::memcpy(bytes.data() + 8, name.data(), name.size());
  core::midi_map_encode(map, settings, bytes.data() + kMidiProfileHeaderBytes,
                        bytes.size() - kMidiProfileHeaderBytes);
  return bytes;
}
inline bool decode(const std::vector<std::uint8_t>& bytes, std::string& name,
                   core::MidiMap& map, core::MidiRigSettings& settings) {
  if (bytes.size() < kMidiProfileHeaderBytes || std::memcmp(bytes.data(), "L24P", 4) ||
      core::get_u32le(bytes.data() + 4) != 1) return false;
  const char* text = reinterpret_cast<const char*>(bytes.data() + 8);
  const auto* end = static_cast<const char*>(std::memchr(text, 0, kMidiProfileNameBytes));
  if (!end) return false;
  const std::string parsed(text, end);
  if (!validName(parsed) || !std::all_of(end, text + kMidiProfileNameBytes, [](char c) { return c == 0; }))
    return false;
  if (!core::midi_map_decode(bytes.data() + kMidiProfileHeaderBytes,
                             bytes.size() - kMidiProfileHeaderBytes, &map, &settings)) return false;
  name = parsed;
  return true;
}
}  // namespace midi_profile_files

// Shared library on disk; one editable profile per instance. Never used by the audio thread.
class MidiMapStore {
 public:
  MidiMapStore() : ops_(app_state_file_ops::realOps()) {}
  MidiMapStore(const MidiMapStore&) = delete;
  MidiMapStore& operator=(const MidiMapStore&) = delete;
  void setDirectory(const std::string& dir) { directory_ = dir; }
  const std::string& directory() const { return directory_; }
  std::string libraryPath() const { return app_state_file_ops::joinUtf8(directory_, "midi-profiles"); }
  std::string livePath() const { return path(id_ + ".bin"); }
  const core::MidiMap& map() const { return map_; }
  const core::MidiRigSettings& settings() const { return settings_; }
  const std::string& id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::string& error() const { return error_; }
  const std::vector<MidiProfileInfo>& profiles() const { return profiles_; }
  bool editable() const { return !id_.empty(); }
  bool dirty() const { return dirty_; }
  std::uint64_t generation() const { return generation_; }
  MidiMapLoadOutcome loadOutcome() const { return loadOutcome_; }

  MidiMapLoadOutcome load() {
    if (directory_.empty()) { fail("Profile folder unavailable."); return loadOutcome_ = MidiMapLoadOutcome::NoPath; }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(libraryPath()), ec);
    if (ec) { fail("Cannot open profile folder."); return loadOutcome_ = MidiMapLoadOutcome::Unreadable; }
    midi_profile_files::Lock lock(path("library.lock"));
    if (!lock || !scan()) { fail("Profile library unavailable. Retry."); return loadOutcome_ = MidiMapLoadOutcome::Unreadable; }
    if (profiles_.empty()) {
      if (!createUnlocked("Untitled", false)) return loadOutcome_ = MidiMapLoadOutcome::Unreadable;
      return loadOutcome_ = MidiMapLoadOutcome::NoFile;
    }
    std::vector<std::uint8_t> selected;
    midi_profile_files::read(path("selected.txt"), selected, 64);
    const std::string wanted(selected.begin(), selected.end());
    const auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.id == wanted && p.readable; });
    const auto fallback = std::find_if(profiles_.begin(), profiles_.end(), [](const auto& p) { return p.readable; });
    if (fallback == profiles_.end() || !adoptFile(it != profiles_.end() ? it->id : fallback->id, false)) {
      fail("No readable profile. Create a new one.");
      return loadOutcome_ = MidiMapLoadOutcome::Malformed;
    }
    return loadOutcome_ = MidiMapLoadOutcome::Ok;
  }
  bool refresh() {
    if (directory_.empty()) return fail("Profile folder unavailable.");
    midi_profile_files::Lock lock(path("library.lock"));
    return lock ? scan() : fail("Profile library busy. Retry.");
  }
  bool setSettings(const core::MidiRigSettings& s) {
    if (!editable() || !core::midi_rig_settings_valid(s)) return false;
    settings_ = s; dirty_ = true; return true;
  }
  bool bind(const core::MidiBinding& b) {
    if (!editable() || !map_.bind(b)) return false;
    dirty_ = true; return true;
  }
  bool unbind(const core::MidiBindingKey& key) {
    if (!editable() || !map_.unbind(key)) return false;
    dirty_ = true; return true;
  }
  bool save() {
    if (!editable()) return fail("No editable profile.");
    midi_profile_files::Lock lock(path("library.lock"));
    return lock ? saveUnlocked() : fail("Profile library busy. Retry save.");
  }
  bool select(const std::string& id) {
    midi_profile_files::Lock lock(path("library.lock"));
    if (!lock) return fail("Profile library busy. Retry.");
    if (dirty_ && !saveUnlocked()) return false;
    return adoptFile(id, true);
  }
  bool create(const std::string& name, bool copyCurrent) {
    midi_profile_files::Lock lock(path("library.lock"));
    if (!lock) return fail("Profile library busy. Retry.");
    // A copy also rescues local edits after a conflicting save in another instance.
    if (!copyCurrent && dirty_ && !saveUnlocked()) return false;
    return createUnlocked(name, copyCurrent);
  }
  bool rename(const std::string& name) {
    midi_profile_files::Lock lock(path("library.lock"));
    if (!lock) return fail("Profile library busy. Retry.");
    if (!editable() || !scan() || !nameAvailable(name, id_)) return false;
    const std::string previous = name_;
    name_ = name;
    if (!saveUnlocked()) { name_ = previous; return false; }
    return scan();
  }
  bool removeCurrent() {
    midi_profile_files::Lock lock(path("library.lock"));
    if (!lock) return fail("Profile library busy. Retry.");
    if (!editable() || !scan()) return false;
    const auto other = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.id != id_ && p.readable; });
    if (other == profiles_.end()) return fail("Keep at least one readable profile.");
    if (!unchanged()) return false;
    const std::string oldPath = livePath(), nextId = other->id;
    // Read the replacement before deleting anything. Selection may safely outlive a failed delete.
    core::MidiMap replacement; core::MidiRigSettings settings; std::string name;
    std::vector<std::uint8_t> bytes;
    if (!readProfile(nextId, bytes, name, replacement, settings) || !remember(nextId)) return false;
    std::error_code ec;
    if (!std::filesystem::remove(std::filesystem::u8path(oldPath), ec) || ec)
      return fail("Cannot delete profile.");
    adopt(nextId, name, replacement, settings, bytes);
    (void)scan();
    return true;
  }
  void setFileOps(const core::FileOps& ops, void* ctx) { ops_ = ops; opsCtx_ = ctx; }

 private:
  std::string path(const std::string& file) const { return app_state_file_ops::joinUtf8(libraryPath(), file); }
  bool fail(const char* message) { error_ = message; return false; }
  bool scan() {
    std::vector<MidiProfileInfo> list;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::u8path(libraryPath()), ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto file = it->path();
      const auto id = file.stem().u8string();
      if (file.extension() != ".bin" || !midi_profile_files::validId(id)) continue;
      MidiProfileInfo info{id, "Unreadable profile", 0, false};
      std::vector<std::uint8_t> bytes; core::MidiMap map; core::MidiRigSettings settings;
      info.readable = midi_profile_files::read(file.u8string(), bytes) &&
                      midi_profile_files::decode(bytes, info.name, map, settings);
      info.bindings = map.count();
      list.push_back(info);
    }
    if (ec) return fail("Cannot read profile list.");
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
      const auto an = midi_profile_files::folded(a.name), bn = midi_profile_files::folded(b.name);
      return an == bn ? a.id < b.id : an < bn;
    });
    profiles_ = std::move(list);
    return true;
  }
  bool nameAvailable(const std::string& name, const std::string& except = "") {
    if (!midi_profile_files::validName(name)) return fail("Use 1-39 English characters; no outer spaces.");
    for (const auto& p : profiles_)
      if (p.id != except && midi_profile_files::folded(p.name) == midi_profile_files::folded(name))
        return fail("That profile name already exists.");
    return true;
  }
  bool readProfile(const std::string& id, std::vector<std::uint8_t>& bytes, std::string& name,
                   core::MidiMap& map, core::MidiRigSettings& settings) {
    if (!midi_profile_files::validId(id) || !midi_profile_files::read(path(id + ".bin"), bytes) ||
        !midi_profile_files::decode(bytes, name, map, settings)) return fail("Cannot read profile. Current setup kept.");
    return true;
  }
  bool write(const std::string& target, const std::vector<std::uint8_t>& bytes) {
    static std::atomic<std::uint64_t> seq{0};
    std::string temp;
    for (int i = 0; i < 16; ++i) {
      temp = target + ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
      if (app_state_file_ops::reserveExclusiveCreate(temp)) break;
      temp.clear();
    }
    if (temp.empty() || core::save_state_atomic(bytes.data(), bytes.size(), temp.c_str(), target.c_str(),
                                               ops_, opsCtx_) != core::SaveResult::ok)
      return fail("Could not save. Local edits kept; retry.");
    return true;
  }
  bool remember(const std::string& id) { return write(path("selected.txt"), {id.begin(), id.end()}); }
  bool unchanged() {
    std::vector<std::uint8_t> current;
    if (!midi_profile_files::read(livePath(), current) || current != baseline_)
      return fail("Changed elsewhere. Copy to keep your edits.");
    return true;
  }
  bool saveUnlocked() {
    if (!editable() || !unchanged()) return false;
    const auto bytes = midi_profile_files::encode(name_, map_, settings_);
    if (!write(livePath(), bytes)) return false;
    baseline_ = bytes; dirty_ = false; error_.clear();
    return true;
  }
  void adopt(const std::string& id, const std::string& name, const core::MidiMap& map,
             const core::MidiRigSettings& settings, const std::vector<std::uint8_t>& bytes) {
    id_ = id; name_ = name; map_ = map; settings_ = settings; baseline_ = bytes;
    dirty_ = false; error_.clear(); ++generation_;
    loadOutcome_ = MidiMapLoadOutcome::Ok;
  }
  bool adoptFile(const std::string& id, bool persist) {
    core::MidiMap map; core::MidiRigSettings settings; std::string name;
    std::vector<std::uint8_t> bytes;
    if (!readProfile(id, bytes, name, map, settings) || (persist && !remember(id))) return false;
    adopt(id, name, map, settings, bytes);
    return true;
  }
  bool createUnlocked(const std::string& name, bool copyCurrent) {
    if (!scan() || !nameAvailable(name)) return false;
    static std::atomic<std::uint64_t> sequence{0};
    std::string id;
    std::error_code ec;
    do {
      id = "p" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    } while (std::filesystem::exists(std::filesystem::u8path(path(id + ".bin")), ec) && !ec);
    if (ec) return fail("Cannot create profile.");
    const core::MidiMap map = copyCurrent ? map_ : core::MidiMap{};
    const auto bytes = midi_profile_files::encode(name, map, settings_);
    if (!write(path(id + ".bin"), bytes)) return false;
    if (!remember(id)) {
      std::filesystem::remove(std::filesystem::u8path(path(id + ".bin")), ec);
      return false;
    }
    adopt(id, name, map, settings_, bytes);
    (void)scan();
    return true;
  }
  core::MidiMap map_;
  core::MidiRigSettings settings_;
  std::string directory_, id_, name_, error_;
  std::vector<std::uint8_t> baseline_;
  std::vector<MidiProfileInfo> profiles_;
  core::FileOps ops_{};
  void* opsCtx_ = nullptr;
  MidiMapLoadOutcome loadOutcome_ = MidiMapLoadOutcome::NotAttempted;
  std::uint64_t generation_ = 0;
  bool dirty_ = false;
};
}  // namespace lunar24::host
