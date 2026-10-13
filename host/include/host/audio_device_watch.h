// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// audio_device_watch.h — when the app's audio stream must be reopened.
//
// Framework-free and UI-thread only (the host feeds it what RtAudio and the system report).
// - A device list change (a device added or removed anywhere on the system) reopens the stream
//   only when it concerns the stream: one of the devices it runs on is gone or changed, the
//   followed system output moved, the stream is not running, or the input the user wants (but
//   could not open) is back. Anything else (an iPad or another controller that is also an audio
//   device, a virtual sound card appearing) keeps the stream, so playing is not interrupted.
// - A stalled stream (no audio callback for a while although one is open) is reopened whatever
//   the system reported, so a stream that died without a notification never stays silent.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lunar24::host {

// One audio device as the system lists it now.
struct AudioDeviceSeen {
  std::uint32_t id = 0;
  std::string name;
  unsigned inputs = 0, outputs = 0;
  bool isDefaultOutput = false;
};

// The stream that is running (or not).
struct AudioStreamOpen {
  bool running = false;
  std::uint32_t outId = 0;
  std::string outName;
  bool inputOpen = false;
  std::uint32_t inId = 0;
  std::string inName;
};

struct AudioDeviceChangeVerdict {
  bool reopen = false;
  const char* why = "";  // for the log
};

// A device list change: reopen, or keep the running stream.
// `followDefaultOutput`: the output follows the system's (a new default moves the sound there).
// `inputWanted` / `wantedInputName`: the user turned the input on and chose this device; when the
// stream runs without it (it would not open), its return is worth one more try.
inline AudioDeviceChangeVerdict audio_device_change(const AudioStreamOpen& open,
                                                    const std::vector<AudioDeviceSeen>& now,
                                                    bool followDefaultOutput, bool inputWanted,
                                                    const std::string& wantedInputName) {
  if (!open.running) return {true, "no stream running"};
  auto find = [&](std::uint32_t id) -> const AudioDeviceSeen* {
    for (const auto& d : now)
      if (d.id == id) return &d;
    return nullptr;
  };
  const AudioDeviceSeen* out = find(open.outId);
  if (out == nullptr || out->outputs == 0 || out->name != open.outName) return {true, "output device gone or changed"};
  if (open.inputOpen) {
    const AudioDeviceSeen* in = find(open.inId);
    if (in == nullptr || in->inputs == 0 || in->name != open.inName) return {true, "input device gone or changed"};
  }
  if (followDefaultOutput)
    for (const auto& d : now)
      if (d.isDefaultOutput && d.outputs > 0 && d.id != open.outId) return {true, "system output moved"};
  if (inputWanted && !open.inputOpen && !wantedInputName.empty())
    for (const auto& d : now)
      if (d.inputs > 0 && d.name == wantedInputName) return {true, "wanted input available"};
  return {false, "not the stream's devices"};
}

// The audio callback's heartbeat: the callback counts its calls, the UI thread checks the count
// about once a tick. A count that stops moving for kStallSeconds while a stream is open is a
// stall. A long gap between checks (the Mac slept, the UI hung) restarts the clock instead, and
// stall reopens back off so a device that keeps stalling cannot reopen in a loop.
class AudioHeartbeat {
 public:
  static constexpr double kStallSeconds = 1.0;     // callbacks come every few ms; tuned by ear
  static constexpr double kGraceSeconds = 2.0;     // after an open, before the first check
  static constexpr double kCheckGapSeconds = 0.5;  // a longer gap between checks: start over
  static constexpr int kMaxStallReopens = 3;       // in a row, then give up (logged)
  static constexpr double kHealthySeconds = 10.0;  // callbacks this long: the count starts over

  // A stream was opened and started at `now` (seconds, any steady clock).
  void opened(double now) {
    open_ = true;
    openedAt_ = now;
    lastCheck_ = now;
    lastMoveAt_ = now;
    seenCount_ = kUnseen;
  }
  void closed() { open_ = false; }
  bool gaveUp() const { return stallReopens_ >= kMaxStallReopens; }

  // True when the stream stalled and should be reopened now.
  bool check(std::uint64_t count, double now) {
    if (!open_) return false;
    const bool gap = now - lastCheck_ > kCheckGapSeconds;
    lastCheck_ = now;
    if (seenCount_ == kUnseen || count != seenCount_ || gap) {
      if (seenCount_ != kUnseen && count != seenCount_ && now - openedAt_ > kHealthySeconds) stallReopens_ = 0;
      seenCount_ = count;
      lastMoveAt_ = now;
      return false;
    }
    if (now - openedAt_ < kGraceSeconds || now - lastMoveAt_ < kStallSeconds) return false;
    if (gaveUp()) return false;
    ++stallReopens_;
    open_ = false;  // the reopen calls opened() again
    return true;
  }

 private:
  static constexpr std::uint64_t kUnseen = ~std::uint64_t{0};
  bool open_ = false;
  double openedAt_ = 0, lastCheck_ = 0, lastMoveAt_ = 0;
  std::uint64_t seenCount_ = kUnseen;
  int stallReopens_ = 0;
};

}  // namespace lunar24::host
