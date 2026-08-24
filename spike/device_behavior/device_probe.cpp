// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// spike/device_behavior/device_probe.cpp
//
// Lunar24 P1 slice ③, requirement 1 — REAL device channel-count authority.
// The DeviceAdapter must learn how many output channels a device has from the
// authoritative CoreAudio property, not from `system_profiler` + a homebrew
// filter (that path leaks — e.g. a Studio Display is actually 8 out).
//
// This probe enumerates every CoreAudio device and reads
// kAudioDevicePropertyStreamConfiguration (Output scope) to get the real,
// framework-reported per-scope channel count. It is the "true count" the
// DeviceAdapter clamps against, and it supplies the >4-output device case that
// slice-③ requirement 3 uses (the sensor: "Core only ever sees four logical
// outputs").
//
// Disposable spike. Never referenced by core/ or generated/. Self-contained
// C++17, framework CoreAudio + CoreFoundation only.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

// Total channels of a device**scope** per the authoritative stream config.
static UInt32 scope_channels(AudioDeviceID dev, AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress addr = {
    kAudioDevicePropertyStreamConfiguration,
    scope,
    kAudioObjectPropertyElementMain
  };
  if (AudioObjectHasProperty(dev, &addr) == 0) return 0;   // kAudioObjectPropertySectionInvalid
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr) return 0;
  if (size < sizeof(AudioBufferList)) return 0;
  auto buf = std::make_unique<std::uint8_t[]>(size);
  if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, buf.get()) != noErr) return 0;
  UInt32 total = 0;
  auto* abl = reinterpret_cast<AudioBufferList*>(buf.get());
  for (UInt32 i = 0; i < abl->mNumberBuffers; ++i)
    total += abl->mBuffers[i].mNumberChannels;
  return total;
}

static std::string device_name(AudioDeviceID dev) {
  AudioObjectPropertyAddress addr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };
  CFStringRef str = nullptr;
  UInt32 size = sizeof(str);
  if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &str) != noErr || !str)
    return std::string("<unnamed>");
  char tmp[256] = {0};
  CFStringGetCString(str, tmp, sizeof(tmp), kCFStringEncodingUTF8);
  CFRelease(str);
  return std::string(tmp);
}

// Real hot-unplug detection (slice-③ req 4): a CoreAudio property listener on
// kAudioHardwarePropertyDevices. Any device add/remove on the system fires this.
// This is the detection half of hot-swap; the containment (swap path holds
// slice-② invariants) is proven in device_behavior.cpp.
static std::atomic<bool> g_devices_changed{false};
static OSStatus device_listener(AudioObjectID /*object*/, UInt32 /*n*/, const AudioObjectPropertyAddress* /*a*/, void* /*ctx*/) {
  g_devices_changed.store(true, std::memory_order_relaxed);
  return noErr;
}

static int watch_devices(int seconds) {
  AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  OSStatus rc = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, device_listener, nullptr);
  std::printf("\n== req 4 detection: real CoreAudio device listener ==\n");
  std::printf("  AudioObjectAddPropertyListener(kAudioHardwarePropertyDevices) -> %s (noErr=%d)\n",
              rc == noErr ? "OK" : "FAIL", (int)noErr);
  if (rc != noErr) return 2;
  std::printf("  watching up to %d s for a device add/remove ...\n", seconds);
  for (int i = 0; i < seconds * 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_devices_changed.load()) break;
  }
  if (g_devices_changed.load())
    std::printf("  -> a device add/remove was DETECTED (listener fired)\n");
  else
    std::printf("  -> no device change in the window (nothing unplugged / attached). Listener stays armed; detection is the same callback the real host installs.\n");
  return 0;
}

int main(int argc, char** argv) {
  std::printf("== P1 slice ③, req 1: REAL CoreAudio device channel-count authority ==\n");

  AudioObjectPropertyAddress dev_addr = {
    kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &dev_addr, 0, nullptr, &size) != noErr) {
    std::printf("FATAL: cannot read device list size\n");
    return 2;
  }
  UInt32 count = size / sizeof(AudioDeviceID);
  if (count == 0) { std::printf("No CoreAudio devices found.\n"); return 0; }

  auto devs = std::make_unique<AudioDeviceID[]>(count);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &dev_addr, 0, nullptr, &size, devs.get()) != noErr) {
    std::printf("FATAL: cannot read device list\n");
    return 2;
  }

  std::printf("%-3s %-40s %8s %8s %8s %8s\n", "#", "name", "OUT", "IN", "OUT(buf)", "IN(buf)");
  for (UInt32 i = 0; i < count; ++i) {
    AudioDeviceID dev = devs[i];
    UInt32 out = scope_channels(dev, kAudioObjectPropertyScopeOutput);
    UInt32 in  = scope_channels(dev, kAudioObjectPropertyScopeInput);
    AudioObjectPropertyAddress buf_addr = {
      kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 bmul = 0;
    UInt32 bsize = 0;
    if (AudioObjectGetPropertyDataSize(dev, &buf_addr, 0, nullptr, &bsize) == noErr && bsize) {
      auto bb = std::make_unique<std::uint8_t[]>(bsize);
      if (AudioObjectGetPropertyData(dev, &buf_addr, 0, nullptr, &bsize, bb.get()) == noErr)
        bmul = reinterpret_cast<AudioBufferList*>(bb.get())->mNumberBuffers;
    }
    std::printf("%-3u %-40s %8u %8u %8u %8u\n",
                (unsigned)dev, device_name(dev).c_str(), (unsigned)out, (unsigned)in,
                (unsigned)bmul, bmul ? 0u : 0u);
  }

  std::printf("\n= Devices with OUTPUT >= 1 (candidates for mapping / clamp) =\n");
  for (UInt32 i = 0; i < count; ++i) {
    AudioDeviceID dev = devs[i];
    UInt32 out = scope_channels(dev, kAudioObjectPropertyScopeOutput);
    if (out > 0)
      std::printf("  %s: OUT=%u channels\n", device_name(dev).c_str(), (unsigned)out);
  }
  std::printf("\n= Devices with OUTPUT > 4 (the Core-only-sees-four sensor case) =\n");
  for (UInt32 i = 0; i < count; ++i) {
    AudioDeviceID dev = devs[i];
    UInt32 out = scope_channels(dev, kAudioObjectPropertyScopeOutput);
    if (out > 4)
      std::printf("  %s: OUT=%u channels\n", device_name(dev).c_str(), (unsigned)out);
  }

  // Optional: `--watch N` installs the real hot-unplug listener for N seconds.
  for (int a = 1; a + 1 < argc; ++a) {
    if (std::strcmp(argv[a], "--watch") == 0) {
      int secs = std::atoi(argv[a + 1]);
      return watch_devices(secs);
    }
  }
  return 0;
}
