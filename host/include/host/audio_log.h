// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace lunar24::host {

// Used ONLY by the log worker. Rotate before a whole record, so a reset snapshot
// is never split between files. Native filesystem paths preserve Windows Unicode.
class RotatingAudioLog {
 public:
  static constexpr std::size_t kFileBytes = 256 * 1024;
  explicit RotatingAudioLog(std::filesystem::path path) : path_(std::move(path)) {}
  bool operator()(std::string_view text) const {
    if (text.size() > kFileBytes) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec);
    if (!ec && size + text.size() > kFileBytes) {
      auto backup = [&](int n) { auto p = path_; p += "." + std::to_string(n); return p; };
      std::filesystem::remove(backup(3), ec);
      if (ec) return false;
      for (int n = 2; n >= 0; --n) {
        const auto from = n ? backup(n) : path_;
        const bool exists = std::filesystem::exists(from, ec);
        if (ec) return false;
        if (exists) {
          std::filesystem::rename(from, backup(n + 1), ec);
          if (ec) return false;  // never grow unbounded after a failed rotation
        }
      }
    }
#ifdef _WIN32
    FILE* f = _wfopen(path_.c_str(), L"ab");
#else
    FILE* f = std::fopen(path_.c_str(), "ab");
#endif
    if (!f) return false;
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && wrote;
  }
 private:
  std::filesystem::path path_;
};

// Bounded, best-effort diagnostics. Producers never perform file I/O, allocate,
// or wait for a mutex. A slow/full/broken logger drops records, never performance
// events. Do not call from the audio render path; existing backend errors may
// originate on another thread. Formatting/rotation/writes happen on the worker.
class AsyncAudioLog {
 public:
  static constexpr std::size_t kRecordBytes = 64 * 1024;
  static constexpr std::size_t kCapacity = 32;
  using Writer = std::function<bool(std::string_view)>;
  ~AsyncAudioLog() { stop(); }
  bool start(Writer writer, std::string_view firstRecord = {}) noexcept {
    if (started_) return false;
    started_ = true;
    try {
      if (firstRecord.size() >= kRecordBytes) return false;
      writer_ = std::move(writer);
      if (!firstRecord.empty()) {
        auto& r = queue_[0];
        r.time = std::chrono::system_clock::now();
        r.size = firstRecord.size();
        std::memcpy(r.text.data(), firstRecord.data(), r.size);
        count_ = 1;
      }
      worker_ = std::thread([this] { run(); });
      enabled_.store(true, std::memory_order_release);
      return true;
    } catch (...) { return false; }
  }
  bool submit(std::string_view text) noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return false;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || count_ == kCapacity || text.size() >= kRecordBytes) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    auto& r = queue_[(head_ + count_) % kCapacity];
    r.time = std::chrono::system_clock::now();
    r.size = text.size();
    std::memcpy(r.text.data(), text.data(), text.size());
    ++count_;
    lock.unlock();
    wake_.notify_one();
    return true;
  }
  // Shutdown only, after producers have stopped. Never called during playback.
  void stop() noexcept {
    enabled_.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
 private:
  struct Record {
    std::array<char, kRecordBytes> text{};
    std::size_t size = 0;
    std::chrono::system_clock::time_point time;
  };
  void run() noexcept {
    Record r;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || count_ != 0; });
        if (!count_) return;
        const auto& src = queue_[head_];
        r.size = src.size; r.time = src.time;
        std::memcpy(r.text.data(), src.text.data(), r.size);
        head_ = (head_ + 1) % kCapacity;
        --count_;
      }
      unsigned lost = 0;
      try {
        const auto t = std::chrono::system_clock::to_time_t(r.time);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[64];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(r.time.time_since_epoch()).count() % 1000;
        char fraction[16];
        std::snprintf(fraction, sizeof fraction, ".%03d  ", static_cast<int>(ms));
        std::string line = std::string(stamp) + fraction;
        lost = dropped_.exchange(0, std::memory_order_relaxed);
        if (lost) line += "[log records lost=" + std::to_string(lost) + "] ";
        line.append(r.text.data(), r.size);
        line += '\n';
        if (!writer_(line)) dropped_.fetch_add(lost + 1, std::memory_order_relaxed);
      } catch (...) { dropped_.fetch_add(lost + 1, std::memory_order_relaxed); }
    }
  }
  std::array<Record, kCapacity> queue_{};
  std::size_t head_ = 0, count_ = 0;
  bool stopping_ = false;
  bool started_ = false;
  std::atomic<bool> enabled_{false};
  std::atomic<unsigned> dropped_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  Writer writer_;
};
}  // namespace lunar24::host
