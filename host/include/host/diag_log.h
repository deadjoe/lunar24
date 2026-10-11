// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/diag_log.h — the diagnostics log of the app and the plugins.
//
// One process-wide Logger. Every record has a level and a category and is written by a
// background thread to a session file under <settings folder>/logs/:
//
//   app-20261011-155440-p4242-001.log      (kind - start date - start time - process id - part)
//
// A session is one run of the app, or one process that hosts the plugin. A part rolls over at
// segment_kib; the oldest finished files are deleted once all sessions together pass total_mib.
// Each part starts with the same header (version, build, config), so any file reads on its own.
// The last line of a session is "session end"; the next session reports a missing one (crash,
// force quit, power loss).
//
// Threads: write() may be called from any thread EXCEPT the audio thread (audio-thread events
// go through host/rt_log.h and are written from the UI thread). A writer takes a mutex held only
// for a memcpy or a buffer swap, never for disk I/O, so slow storage cannot stall the panel or
// MIDI. When the queue is full a record is dropped and counted; the next written line says how
// many were lost. Nothing here throws.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace lunar24::host::diag {

// error: something failed. warn: something was lost or worked around (a dropped note event, a
// failed device open). info: what happened (devices, sessions, snapshots, marks). debug: every
// played note, cable, knob change. trace: high-rate detail (every MIDI CC).
enum class Level : std::uint8_t { error = 0, warn, info, debug, trace, off };
enum class Cat : std::uint8_t { app = 0, audio, midi, ui, state, engine, rec, log };
inline constexpr std::size_t kCatCount = 8;

inline const char* levelName(Level l) {
  static const char* names[] = {"error", "warn", "info", "debug", "trace", "off"};
  const auto i = static_cast<std::size_t>(l);
  return i < 6 ? names[i] : "?";
}
inline char levelLetter(Level l) { return "EWIDT-"[std::min<std::size_t>(static_cast<std::size_t>(l), 5)]; }
inline const char* catName(Cat c) {
  static const char* names[] = {"app", "audio", "midi", "ui", "state", "engine", "rec", "log"};
  const auto i = static_cast<std::size_t>(c);
  return i < kCatCount ? names[i] : "?";
}
inline bool parseLevel(std::string_view s, Level* out) {
  for (int i = 0; i <= static_cast<int>(Level::off); ++i)
    if (s == levelName(static_cast<Level>(i))) { *out = static_cast<Level>(i); return true; }
  return false;
}
inline bool parseCat(std::string_view s, Cat* out) {
  for (std::size_t i = 0; i < kCatCount; ++i)
    if (s == catName(static_cast<Cat>(i))) { *out = static_cast<Cat>(i); return true; }
  return false;
}

// ---- log.conf --------------------------------------------------------------------------------
struct Config {
  std::array<Level, kCatCount> threshold{};  // a record is kept when its level <= threshold
  std::uint64_t segmentBytes = 2u << 20;     // one part file
  std::uint64_t totalBytes = 64u << 20;      // every session's files together
  Config() { threshold.fill(Level::debug); }
};

inline constexpr const char* kConfigFileName = "log.conf";
inline constexpr const char* kConfigTemplate =
    "# Lunar 24 diagnostics log settings. Read when the app or a plugin starts.\n"
    "# Logs are in the logs folder next to this file, one file per session (and part).\n"
    "#\n"
    "# Levels, quietest first: off, error, warn, info, debug, trace.\n"
    "#   info  = devices, sessions, snapshots, marks, warnings\n"
    "#   debug = also every played note, cable, drone key and knob change (the default)\n"
    "#   trace = also every MIDI CC message\n"
    "# 'level' sets every category; a category line overrides it. Categories:\n"
    "#   app audio midi ui state engine rec log\n"
    "level = debug\n"
    "# midi = trace\n"
    "#\n"
    "# Size: one part file (KiB, 64..65536) and all sessions together (MiB, 4..4096).\n"
    "segment_kib = 2048\n"
    "total_mib = 64\n";

inline std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Unknown keys and bad values are reported in `problems` and otherwise ignored.
inline Config parseConfig(std::string_view text, std::vector<std::string>* problems = nullptr) {
  Config c;
  auto problem = [&](std::string_view line, const char* why) {
    if (problems) problems->push_back(std::string(why) + ": " + std::string(line));
  };
  auto number = [](std::string_view v, std::uint64_t* out) {
    if (v.empty() || v.size() > 9) return false;
    std::uint64_t n = 0;
    for (char ch : v) {
      if (ch < '0' || ch > '9') return false;
      n = n * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    *out = n;
    return true;
  };
  // 'level' first, so a category line overrides it wherever it appears.
  for (int pass = 0; pass < 2; ++pass) {
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t end = std::min(text.find('\n', start), text.size());
      std::string_view line = trim(text.substr(start, end - start));
      start = end + 1;
      if (line.empty() || line.front() == '#') continue;
      const std::size_t eq = line.find('=');
      if (eq == std::string_view::npos) {
        if (pass == 0) problem(line, "no '='");
        continue;
      }
      const std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      Level l{};
      Cat cat{};
      std::uint64_t n = 0;
      if (key == "level") {
        if (pass != 0) continue;
        if (parseLevel(value, &l)) c.threshold.fill(l); else problem(line, "unknown level");
      } else if (parseCat(key, &cat)) {
        if (pass != 1) continue;
        if (parseLevel(value, &l)) c.threshold[static_cast<std::size_t>(cat)] = l; else problem(line, "unknown level");
      } else if (key == "segment_kib") {
        if (pass != 0) continue;
        if (number(value, &n)) c.segmentBytes = std::clamp<std::uint64_t>(n, 64, 65536) << 10;
        else problem(line, "not a number");
      } else if (key == "total_mib") {
        if (pass != 0) continue;
        if (number(value, &n)) c.totalBytes = std::clamp<std::uint64_t>(n, 4, 4096) << 20;
        else problem(line, "not a number");
      } else if (pass == 0) {
        problem(line, "unknown setting");
      }
    }
  }
  if (c.totalBytes < 2 * c.segmentBytes) c.totalBytes = 2 * c.segmentBytes;
  return c;
}

inline std::string describeConfig(const Config& c) {
  std::string s = "levels:";
  for (std::size_t i = 0; i < kCatCount; ++i)
    s += std::string(" ") + catName(static_cast<Cat>(i)) + "=" + levelName(c.threshold[i]);
  s += "; segment_kib=" + std::to_string(c.segmentBytes >> 10) + " total_mib=" + std::to_string(c.totalBytes >> 20);
  return s;
}

// ---- files -----------------------------------------------------------------------------------
namespace files {
namespace fs = std::filesystem;

// "<kind>-YYYYMMDD-HHMMSS-p<pid>-<part>.log"; anything else in the folder is never touched.
struct Name {
  std::string session;  // everything before "-<part>.log"
  std::string kind;
  int part = 0;
};
inline bool parseName(const std::string& file, Name* out) {
  const std::string ext = ".log";
  if (file.size() < ext.size() + 4 || file.compare(file.size() - ext.size(), ext.size(), ext) != 0) return false;
  const std::string stem = file.substr(0, file.size() - ext.size());
  const std::size_t dash = stem.rfind('-');
  if (dash == std::string::npos || dash + 1 >= stem.size()) return false;
  int part = 0;
  for (std::size_t i = dash + 1; i < stem.size(); ++i) {
    if (stem[i] < '0' || stem[i] > '9' || part > 99999) return false;
    part = part * 10 + (stem[i] - '0');
  }
  const std::string session = stem.substr(0, dash);
  // kind-YYYYMMDD-HHMMSS-p<pid>
  const std::size_t k = session.find('-');
  if (k == std::string::npos || k == 0 || session.size() < k + 1 + 8 + 1 + 6 + 3) return false;
  for (std::size_t i = 0; i < 8; ++i) if (!std::isdigit(static_cast<unsigned char>(session[k + 1 + i]))) return false;
  if (session[k + 9] != '-') return false;
  for (std::size_t i = 0; i < 6; ++i) if (!std::isdigit(static_cast<unsigned char>(session[k + 10 + i]))) return false;
  if (session.compare(k + 16, 2, "-p") != 0) return false;
  out->session = session;
  out->kind = session.substr(0, k);
  out->part = part;
  return true;
}
inline std::string partFileName(const std::string& session, int part) {
  char n[16];
  std::snprintf(n, sizeof n, "-%03d.log", part);
  return session + n;
}

struct Entry {
  fs::path path;
  Name name;
  std::uint64_t size = 0;
  fs::file_time_type mtime{};
};
inline std::vector<Entry> list(const fs::path& dir) {
  std::vector<Entry> out;
  std::error_code ec;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2) || e2) continue;
    Entry entry;
    // Our names are ASCII, so the narrow name is exact; a non-ASCII name simply never parses.
    std::string file;
    try { file = it->path().filename().string(); } catch (...) { continue; }
    if (!parseName(file, &entry.name)) continue;
    entry.path = it->path();
    entry.size = it->file_size(e2);
    if (e2) continue;
    entry.mtime = it->last_write_time(e2);
    if (e2) continue;
    out.push_back(std::move(entry));
  }
  return out;
}

// Delete the oldest files until the folder fits `budget`. Never deletes `current`, nor the newest
// part of a session written to within `liveGrace` (another process may still be writing it).
// Returns the number of files deleted.
inline int prune(const fs::path& dir, std::uint64_t budget, const fs::path& current,
                 std::chrono::seconds liveGrace) {
  std::vector<Entry> all = list(dir);
  std::uint64_t total = 0;
  for (const auto& e : all) total += e.size;
  if (total <= budget) return 0;
  std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
    return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
  });
  const auto now = fs::file_time_type::clock::now();
  auto newestPartOf = [&](const Entry& e) {
    for (const auto& o : all)
      if (o.name.session == e.name.session && o.name.part > e.name.part) return false;
    return true;
  };
  int deleted = 0;
  for (const auto& e : all) {
    if (total <= budget) break;
    if (e.path == current) continue;
    if (newestPartOf(e) && now - e.mtime < liveGrace) continue;
    std::error_code ec;
    if (fs::remove(e.path, ec) && !ec) {
      total -= e.size;
      ++deleted;
    }
  }
  return deleted;
}

// The newest earlier session of `kind`: its last part, and whether that part ends with a
// "session end" line. Returns false when there is none.
inline bool previousSession(const fs::path& dir, const std::string& kind, const std::string& exclude,
                            std::string* file, bool* endedCleanly) {
  const std::vector<Entry> all = list(dir);
  const Entry* best = nullptr;
  for (const auto& e : all) {
    if (e.name.kind != kind || e.name.session == exclude) continue;
    if (!best || e.mtime > best->mtime || (e.mtime == best->mtime && e.name.part > best->name.part)) best = &e;
  }
  if (!best) return false;
  *file = best->path.filename().string();
  *endedCleanly = false;
#if defined(_WIN32)
  std::FILE* f = ::_wfsopen(best->path.c_str(), L"rb", _SH_DENYNO);
#else
  std::FILE* f = std::fopen(best->path.c_str(), "rb");
#endif
  if (!f) return true;
  char tail[512] = {};
  const long size = static_cast<long>(best->size);
  const long from = size > static_cast<long>(sizeof tail - 1) ? size - static_cast<long>(sizeof tail - 1) : 0;
  std::size_t n = 0;
  if (std::fseek(f, from, SEEK_SET) == 0) n = std::fread(tail, 1, sizeof tail - 1, f);
  std::fclose(f);
  *endedCleanly = std::string_view(tail, n).find("session end") != std::string_view::npos;
  return true;
}
}  // namespace files

// ---- the logger ------------------------------------------------------------------------------
class Logger {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t kMaxRecordBytes = 128 * 1024;

  explicit Logger(std::size_t queueBytes = 1u << 20) {
    for (auto& t : threshold_) t.store(static_cast<std::uint8_t>(Level::debug), std::memory_order_relaxed);
    front_.resize(queueBytes);
    back_.resize(queueBytes);
  }
  ~Logger() {
    std::lock_guard<std::mutex> life(lifeMutex_);
    if (users_ > 0) { users_ = 1; stopLocked_(); }
  }
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  // The one logger of this process. Never destroyed: the last detach() stops its thread, so a
  // plugin's DLL can unload without joining a thread from a static destructor.
  static Logger& process() {
    static Logger* logger = new Logger();
    return *logger;
  }
  // A small per-process number for each plugin instance (shown as #n in its lines).
  static std::uint16_t nextInstance() {
    static std::atomic<std::uint16_t> next{0};
    return static_cast<std::uint16_t>(next.fetch_add(1, std::memory_order_relaxed) + 1);
  }

  // Every user (the app host, each plugin instance) attaches once and detaches once. The first
  // attach starts the writer; the last detach writes "session end", flushes and stops it.
  void attach() {
    std::lock_guard<std::mutex> life(lifeMutex_);
    if (users_++ > 0) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      sinkWanted_ = false;
      sinkFresh_ = false;
      used_ = 0;
    }
    startSteady_ = Clock::now();
    startWall_ = std::chrono::system_clock::now();
    try {
      worker_ = std::thread([this] { run_(); });
      running_.store(true, std::memory_order_release);
    } catch (...) {
      running_.store(false, std::memory_order_release);
    }
  }
  void detach() {
    std::lock_guard<std::mutex> life(lifeMutex_);
    if (users_ == 0) return;
    if (--users_ > 0) return;
    stopLocked_();
  }

  // Name the folder and start the session. The first call per run wins (the app host and a
  // plugin instance may both know the folder); returns false for later calls or with no folder.
  // Records written before this wait in the queue. `header` is repeated at the top of each part.
  bool open(const std::string& settingsDirUtf8, const std::string& kind, const std::string& header) {
    if (settingsDirUtf8.empty() || kind.empty()) return false;
    Config config;
    std::vector<std::string> problems;
    bool hadConfig = false;
    std::filesystem::path dir;
    try {
      const auto base = std::filesystem::u8path(settingsDirUtf8);
      dir = base / "logs";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const auto confPath = base / kConfigFileName;
      std::string text;
      if (readFile_(confPath, &text)) {
        hadConfig = true;
        config = parseConfig(text, &problems);
      } else {
        writeFile_(confPath, kConfigTemplate);  // so the owner finds the knobs
      }
    } catch (...) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (sinkWanted_ || !running_.load(std::memory_order_acquire)) return false;
    for (std::size_t i = 0; i < kCatCount; ++i)
      threshold_[i].store(static_cast<std::uint8_t>(config.threshold[i]), std::memory_order_relaxed);
    config_ = config;
    dir_ = dir;
    kind_ = kind;
    header_ = header;
    configNote_ = std::string(hadConfig ? "log.conf: " : "log.conf created with defaults: ") + describeConfig(config);
    for (const auto& p : problems) configNote_ += "\nlog.conf problem (ignored): " + p;
    sinkWanted_ = true;
    sinkFresh_ = true;
    ++submitted_;  // the header: flush() waits for it too
    wake_.notify_one();
    return true;
  }

  bool enabled(Cat c, Level l) const noexcept {
    return running_.load(std::memory_order_relaxed) &&
           static_cast<std::uint8_t>(l) <= threshold_[static_cast<std::size_t>(c)].load(std::memory_order_relaxed);
  }
  void setThreshold(Cat c, Level l) noexcept {
    threshold_[static_cast<std::size_t>(c)].store(static_cast<std::uint8_t>(l), std::memory_order_relaxed);
  }

  // One record; multi-line text stays together. `when` lets a caller keep the time an event
  // actually happened (audio-thread events are written later from the UI thread).
  bool write(Cat c, Level l, std::string_view text, std::uint16_t instance = 0,
             Clock::time_point when = Clock::now()) noexcept {
    if (!enabled(c, l)) return false;
    if (text.size() > kMaxRecordBytes) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    Header h;
    h.size = static_cast<std::uint32_t>(text.size());
    h.level = static_cast<std::uint8_t>(l);
    h.cat = static_cast<std::uint8_t>(c);
    h.instance = instance;
    h.when = when.time_since_epoch().count();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || used_ + sizeof h + text.size() > front_.size()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      std::memcpy(front_.data() + used_, &h, sizeof h);
      if (!text.empty()) std::memcpy(front_.data() + used_ + sizeof h, text.data(), text.size());
      used_ += sizeof h + text.size();
      ++submitted_;
    }
    wake_.notify_one();
    return true;
  }
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 5, 6)))
#endif
  bool writef(Cat c, Level l, std::uint16_t instance, const char* fmt, ...) noexcept {
    if (!enabled(c, l)) return false;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (n < 0) return false;
    return write(c, l, std::string_view(line, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof line - 1)),
                 instance);
  }

  // Wait (at most `timeout`) until everything written so far is on disk or counted as lost.
  bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t target = submitted_;
    return done_.wait_for(lock, timeout, [&] { return completed_ >= target || !running_.load(); });
  }

  struct Stats {
    std::uint64_t written = 0, dropped = 0, failed = 0;
  };
  Stats stats() const {
    Stats s;
    s.written = written_.load(std::memory_order_relaxed);
    s.dropped = droppedTotal_.load(std::memory_order_relaxed) + dropped_.load(std::memory_order_relaxed);
    s.failed = failedTotal_.load(std::memory_order_relaxed);
    return s;
  }
  // The file being written now (UTF-8), "" before open or after a failure.
  std::string currentFile() const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    return currentFile_;
  }
  std::string folder() const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    return folder_;
  }

 private:
  struct Header {
    std::uint32_t size = 0;
    std::uint8_t level = 0, cat = 0;
    std::uint16_t instance = 0;
    std::int64_t when = 0;
  };

  static bool readFile_(const std::filesystem::path& p, std::string* out) {
    std::FILE* f = openPath_(p, false);
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out->size() < 64 * 1024) out->append(buf, n);
    std::fclose(f);
    return true;
  }
  static void writeFile_(const std::filesystem::path& p, const char* text) {
    std::FILE* f = openPath_(p, true, false);
    if (!f) return;
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
  }
  // Readers (an editor, `tail -f`) may keep the file open while we append.
  static std::FILE* openPath_(const std::filesystem::path& p, bool write, bool append = true) {
#if defined(_WIN32)
    return ::_wfsopen(p.c_str(), write ? (append ? L"ab" : L"wb") : L"rb", _SH_DENYNO);
#else
    return std::fopen(p.c_str(), write ? (append ? "ab" : "wb") : "rb");
#endif
  }
  static int pid_() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
  }

  void stopLocked_() {
    running_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(fileMutex_);
    currentFile_.clear();
  }

  // Local wall time of a steady-clock instant: "2026-10-11 15:54:40.123".
  void stamp_(Clock::time_point t, char* out, std::size_t size) const {
    const auto wall = startWall_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - startSteady_);
    const std::time_t secs = std::chrono::system_clock::to_time_t(wall);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000;
    if (ms < 0) ms += 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    std::snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  }

  void format_(std::string& out, Level l, Cat c, std::uint16_t instance, Clock::time_point t,
               std::string_view text) const {
    char head[96];
    stamp_(t, head, 40);
    const double up = std::chrono::duration<double>(t - startSteady_).count();
    char rest[56];
    if (instance)
      std::snprintf(rest, sizeof rest, " %10.3f %c %-6s #%u ", up, levelLetter(l), catName(c), unsigned(instance));
    else
      std::snprintf(rest, sizeof rest, " %10.3f %c %-6s ", up, levelLetter(l), catName(c));
    out += head;
    out += rest;
    // Continuation lines are indented so every record starts at a timestamp.
    std::size_t start = 0;
    for (;;) {
      const std::size_t nl = text.find('\n', start);
      out.append(text.data() + start, (nl == std::string_view::npos ? text.size() : nl) - start);
      out += '\n';
      if (nl == std::string_view::npos || nl + 1 >= text.size()) break;
      out += "    | ";
      start = nl + 1;
    }
  }

  // ---- writer thread -------------------------------------------------------------------------
  struct Sink {
    std::FILE* file = nullptr;
    std::filesystem::path path;
    std::uint64_t size = 0;
    int part = 0;
    std::string session;
  };

  bool openPart_(Sink& s, int part) {
    if (s.file) { std::fclose(s.file); s.file = nullptr; }
    s.part = part;
    s.path = dir_ / files::partFileName(s.session, part);
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    s.file = openPath_(s.path, true);
    s.size = 0;
    if (s.file) {
      std::error_code e2;
      const auto existing = std::filesystem::file_size(s.path, e2);
      if (!e2) s.size = existing;
    }
    {
      std::lock_guard<std::mutex> lock(fileMutex_);
      currentFile_ = s.file ? s.path.u8string() : std::string();
      folder_ = dir_.u8string();
    }
    if (!s.file) return false;
    try { files::prune(dir_, config_.totalBytes, s.path, std::chrono::hours(6)); } catch (...) {}
    // Every part describes itself.
    std::string text = "session " + s.session + " part " + std::to_string(part) + " pid " + std::to_string(pid_());
    if (!header_.empty()) text += "\n" + header_;
    text += "\n" + configNote_;
    if (part == 1) {
      std::string prev;
      bool clean = false;
      try {
        if (files::previousSession(dir_, kind_, s.session, &prev, &clean))
          text += clean ? "\nprevious " + kind_ + " session ended cleanly: " + prev
                        : "\nprevious " + kind_ + " session has no end line (crash, force quit, power loss, or still running): " + prev;
      } catch (...) {}
    }
    std::string line;
    format_(line, Level::info, Cat::log, 0, Clock::now(), text);
    return put_(s, line);
  }

  bool put_(Sink& s, const std::string& line) {
    if (!s.file) return false;
    const bool ok = std::fwrite(line.data(), 1, line.size(), s.file) == line.size();
    if (ok) s.size += line.size();
    return ok;
  }

  // Write one formatted record, starting a new part first if it would not fit.
  bool emit_(Sink& s, const std::string& line) {
    if (!s.file && !openPart_(s, s.part > 0 ? s.part : 1)) return false;
    if (s.size > 0 && s.size + line.size() > config_.segmentBytes && !openPart_(s, s.part + 1)) return false;
    if (!put_(s, line)) {
      std::fclose(s.file);  // reopened (same part) before the next record
      s.file = nullptr;
      return false;
    }
    return true;
  }

  void run_() noexcept {
    Sink sink;
    std::string out;
    std::uint64_t lostToWrites = 0;
    for (;;) {
      std::size_t n = 0;
      std::uint64_t batch = 0;
      bool stop = false, haveSink = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || sinkFresh_ || (used_ > 0 && sinkWanted_); });
        stop = stopping_;
        haveSink = sinkWanted_;
        sinkFresh_ = false;
        if (haveSink || stop) {
          front_.swap(back_);
          n = used_;
          used_ = 0;
          batch = submitted_ - completed_;
        }
      }
      try {
        if (haveSink && sink.session.empty()) {
          char when[32];
          const std::time_t secs = std::chrono::system_clock::to_time_t(startWall_);
          std::tm tm{};
#if defined(_WIN32)
          localtime_s(&tm, &secs);
#else
          localtime_r(&secs, &tm);
#endif
          std::snprintf(when, sizeof when, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec);
          sink.session = kind_ + "-" + when + "-p" + std::to_string(pid_());
          // A plugin process may end and start a session within the same second.
          if (const int run = ++runs_; run > 1) sink.session += "." + std::to_string(run);
        }
        auto note = [&](Level l, const std::string& text) {
          out.clear();
          format_(out, l, Cat::log, 0, Clock::now(), text);
          if (emit_(sink, out)) written_.fetch_add(1, std::memory_order_relaxed);
        };
        if (haveSink) {
          if (!sink.file && sink.part == 0) openPart_(sink, 1);  // the header, right away
          if (const auto lost = dropped_.exchange(0, std::memory_order_relaxed)) {
            droppedTotal_.fetch_add(lost, std::memory_order_relaxed);
            note(Level::warn, std::to_string(lost) + " log records dropped (queue full or record too large)");
          }
          if (lostToWrites) {
            note(Level::warn, std::to_string(lostToWrites) + " log records lost to file write errors");
            if (sink.file) lostToWrites = 0;
          }
          for (std::size_t at = 0; at + sizeof(Header) <= n;) {
            Header h;
            std::memcpy(&h, back_.data() + at, sizeof h);
            const std::string_view text(back_.data() + at + sizeof h, h.size);
            at += sizeof h + h.size;
            out.clear();
            format_(out, static_cast<Level>(h.level), static_cast<Cat>(h.cat), h.instance,
                    Clock::time_point(Clock::duration(h.when)), text);
            if (emit_(sink, out)) {
              written_.fetch_add(1, std::memory_order_relaxed);
            } else {
              ++lostToWrites;
              failedTotal_.fetch_add(1, std::memory_order_relaxed);
            }
          }
          if (stop) {
            const Stats st = stats();
            note(Level::info, "session end (clean shutdown): " + std::to_string(st.written + 1) + " records written, " +
                                  std::to_string(st.dropped) + " dropped, " + std::to_string(st.failed) +
                                  " lost to write errors");
          }
          if (sink.file) std::fflush(sink.file);
        }
      } catch (...) {
        failedTotal_.fetch_add(batch, std::memory_order_relaxed);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        completed_ += batch;
      }
      done_.notify_all();
      if (stop) break;
    }
    if (sink.file) std::fclose(sink.file);
  }

  std::array<std::atomic<std::uint8_t>, kCatCount> threshold_{};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> dropped_{0}, droppedTotal_{0}, failedTotal_{0}, written_{0};

  std::mutex lifeMutex_;  // attach / detach
  int users_ = 0;
  int runs_ = 0;  // sessions started by this logger (writer thread)
  std::thread worker_;

  mutable std::mutex mutex_;  // the queue; never held during disk I/O
  std::condition_variable wake_, done_;
  std::vector<char> front_, back_;  // producers fill front_; the writer swaps and drains back_
  std::size_t used_ = 0;
  std::uint64_t submitted_ = 0, completed_ = 0;
  bool stopping_ = false;
  bool sinkWanted_ = false;
  bool sinkFresh_ = false;  // open() ran; the writer starts the first part at once

  // Set by open() under mutex_ before sinkWanted_; read by the writer afterwards.
  Config config_;
  std::filesystem::path dir_;
  std::string kind_, header_, configNote_;
  Clock::time_point startSteady_{};
  std::chrono::system_clock::time_point startWall_{};

  mutable std::mutex fileMutex_;
  std::string currentFile_, folder_;
};

// Shorthands for the process logger.
inline Logger& log() { return Logger::process(); }

}  // namespace lunar24::host::diag
