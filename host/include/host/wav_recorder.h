// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The REC button's recorder (not on the hardware). The audio thread copies every frame's
// four outputs (WET L, WET R, DRY A, DRY B) into a preallocated ring; a writer thread
// drains it into 24-bit stereo WAV files: the WET pair, the DRY pair (left = DRY A,
// right = DRY B), or both. The audio thread never allocates, locks or touches a file.
// If the disk falls behind for longer than the ring holds (~10 s), the extra frames are
// dropped and counted, never blocking the audio callback.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <host/standalone_audio_engine.h>  // AudioTap

namespace lunar24::host {

// What REC records. The order is the panel's SOURCE switch order.
enum class RecordSource : int { wet = 0, dry = 1, all = 2 };
inline const char* record_source_name(RecordSource s) {
  switch (s) {
    case RecordSource::dry: return "DRY";
    case RecordSource::all: return "ALL";
    default: return "WET";
  }
}

class WavRecorder final : public AudioTap {
 public:
  static constexpr int kChannels = 4;  // WET L, WET R, DRY A, DRY B
  explicit WavRecorder(std::size_t capacityFrames = std::size_t{1} << 19)
      : capacity_(capacityFrames), ring_(capacityFrames * kChannels) {}
  ~WavRecorder() override { (void)stop(); }
  WavRecorder(const WavRecorder&) = delete;
  WavRecorder& operator=(const WavRecorder&) = delete;

  // ---- UI thread ------------------------------------------------------------------
  // Start writing to the given files (either may be null, not both). Takes ownership of
  // both FILE*s; they are closed by stop(). Returns false (closing what it was given) when
  // already recording or nothing to write.
  bool start(std::FILE* wet, std::FILE* dry, int sampleRate) {
    if (recording() || (wet == nullptr && dry == nullptr) || sampleRate <= 0) {
      if (wet != nullptr) std::fclose(wet);
      if (dry != nullptr) std::fclose(dry);
      return false;
    }
    files_[0] = wet;
    files_[1] = dry;
    rate_ = sampleRate;
    dataBytes_ = 0;
    framesWritten_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    for (std::FILE* f : files_)
      if (f != nullptr) writeHeader_(f, 0);
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_relaxed);  // forget old frames
    running_.store(true, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
    writer_ = std::thread([this] {
      while (running_.load(std::memory_order_acquire)) {
        drain_();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    return true;
  }

  struct Result {
    bool wasRecording = false;
    double seconds = 0.0;
    std::uint64_t droppedFrames = 0;
  };
  // Stop, write what is left, fix the WAV headers and close the files.
  Result stop() {
    Result r;
    if (!recording()) return r;
    armed_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    drain_();
    for (std::FILE*& f : files_) {
      if (f == nullptr) continue;
      std::fflush(f);
      if (std::fseek(f, 0, SEEK_SET) == 0) writeHeader_(f, dataBytes_);
      std::fclose(f);
      f = nullptr;
    }
    r.wasRecording = true;
    r.seconds = seconds();
    r.droppedFrames = dropped_.load(std::memory_order_relaxed);
    return r;
  }
  bool recording() const { return files_[0] != nullptr || files_[1] != nullptr; }
  int sampleRate() const { return rate_; }
  double seconds() const {
    return rate_ > 0 ? double(framesWritten_.load(std::memory_order_relaxed)) / rate_ : 0.0;
  }

  // ---- audio thread (AudioTap) --------------------------------------------------------
  bool armed() const override { return armed_.load(std::memory_order_acquire); }
  void push(const float* frames, int count) override {
    if (!armed() || frames == nullptr || count <= 0) return;
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t used = head - tail_.load(std::memory_order_acquire);
    const std::uint64_t room = capacity_ - std::min<std::uint64_t>(used, capacity_);
    const std::uint64_t n = std::min<std::uint64_t>(room, static_cast<std::uint64_t>(count));
    for (std::uint64_t i = 0; i < n; ++i) {
      float* slot = &ring_[((head + i) % capacity_) * kChannels];
      for (int c = 0; c < kChannels; ++c) slot[c] = frames[i * kChannels + c];
    }
    head_.store(head + n, std::memory_order_release);
    if (n < static_cast<std::uint64_t>(count))
      dropped_.fetch_add(static_cast<std::uint64_t>(count) - n, std::memory_order_relaxed);
  }

 private:
  // A WAV data chunk is limited to 4 GB: stop a little before (~6 h of 24-bit stereo at 48 kHz).
  static constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - 1024;
  static constexpr int kBytesPerFrame = 2 * 3;  // stereo, 24-bit

  static void put16_(std::FILE* f, std::uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b, 1, 2, f);
  }
  static void put32_(std::FILE* f, std::uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, f);
  }
  void writeHeader_(std::FILE* f, std::uint64_t dataBytes) const {
    const auto data = static_cast<std::uint32_t>(dataBytes);
    std::fwrite("RIFF", 1, 4, f);
    put32_(f, 36u + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32_(f, 16);                                            // fmt chunk size
    put16_(f, 1);                                             // PCM
    put16_(f, 2);                                             // stereo
    put32_(f, static_cast<std::uint32_t>(rate_));
    put32_(f, static_cast<std::uint32_t>(rate_) * kBytesPerFrame);
    put16_(f, kBytesPerFrame);                                // block align
    put16_(f, 24);                                            // bits per sample
    std::fwrite("data", 1, 4, f);
    put32_(f, data);
  }
  static void put24_(unsigned char* p, float v) {
    const double c = std::clamp(static_cast<double>(v), -1.0, 1.0);
    const auto s = static_cast<std::int32_t>(std::lrint(c * 8388607.0));
    p[0] = static_cast<unsigned char>(s);
    p[1] = static_cast<unsigned char>(s >> 8);
    p[2] = static_cast<unsigned char>(s >> 16);
  }
  // Writer thread (and stop() after the join): move everything in the ring to the files.
  void drain_() {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    while (tail < head) {
      const std::uint64_t n = std::min<std::uint64_t>(head - tail, 4096);
      std::uint64_t keep = n;
      if (dataBytes_ + n * kBytesPerFrame > kMaxDataBytes) {
        keep = (kMaxDataBytes - dataBytes_) / kBytesPerFrame;
        dropped_.fetch_add(n - keep, std::memory_order_relaxed);
      }
      for (int pair = 0; pair < 2; ++pair) {
        std::FILE* f = files_[pair];
        if (f == nullptr || keep == 0) continue;
        scratch_.resize(static_cast<std::size_t>(keep) * kBytesPerFrame);
        for (std::uint64_t i = 0; i < keep; ++i) {
          const float* slot = &ring_[((tail + i) % capacity_) * kChannels];
          put24_(&scratch_[i * kBytesPerFrame], slot[pair * 2]);
          put24_(&scratch_[i * kBytesPerFrame + 3], slot[pair * 2 + 1]);
        }
        std::fwrite(scratch_.data(), 1, scratch_.size(), f);
      }
      dataBytes_ += keep * kBytesPerFrame;
      framesWritten_.fetch_add(keep, std::memory_order_relaxed);
      tail += n;
      tail_.store(tail, std::memory_order_release);
    }
  }

  const std::size_t capacity_;
  std::vector<float> ring_;
  std::atomic<std::uint64_t> head_{0}, tail_{0};
  std::atomic<bool> armed_{false}, running_{false};
  std::atomic<std::uint64_t> framesWritten_{0}, dropped_{0};
  std::FILE* files_[2] = {nullptr, nullptr};  // WET pair, DRY pair
  int rate_ = 0;
  std::uint64_t dataBytes_ = 0;  // per file (both files hold the same frame count)
  std::vector<unsigned char> scratch_;
  std::thread writer_;
};

}  // namespace lunar24::host
