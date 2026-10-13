// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// When a device list change reopens the audio stream, and when a stalled stream does.
#include "mini_test.h"

#include <host/audio_device_watch.h>

#include <string>
#include <vector>

using namespace lunar24::host;
using Devices = std::vector<AudioDeviceSeen>;

static AudioDeviceSeen dev(std::uint32_t id, const char* name, unsigned in, unsigned out, bool defOut = false) {
  AudioDeviceSeen d;
  d.id = id;
  d.name = name;
  d.inputs = in;
  d.outputs = out;
  d.isDefaultOutput = defOut;
  return d;
}

int main() {
  // The usual setup: speakers out (following the system output), the built-in mic in.
  AudioStreamOpen open;
  open.running = true;
  open.outId = 131;
  open.outName = "MacBook Pro Speakers";
  open.inputOpen = true;
  open.inId = 130;
  open.inName = "MacBook Pro Microphone";
  const Devices base = {dev(130, "MacBook Pro Microphone", 1, 0), dev(131, "MacBook Pro Speakers", 0, 2, true),
                        dev(129, "BlackHole 2ch", 2, 2)};
  auto verdict = [&](const Devices& now, bool follow = true, bool wantIn = true,
                     const std::string& wantName = "MacBook Pro Microphone") {
    return audio_device_change(open, now, follow, wantIn, wantName).reopen;
  };

  // Devices that are not the stream's come and go: keep playing.
  {
    Devices withIpad = base;
    withIpad.push_back(dev(140, "iPad", 2, 0));          // an iPad enabled for USB audio + MIDI
    CHECK(!verdict(withIpad));
    CHECK(!verdict(base));                              // ... and unplugged again
    Devices withVirtual = base;
    withVirtual.push_back(dev(141, "Microsoft Teams Audio", 1, 1));
    CHECK(!verdict(withVirtual));
    Devices noBlackHole = {base[0], base[1]};           // an unused device removed
    CHECK(!verdict(noBlackHole));
  }
  // The stream's own devices.
  {
    CHECK(verdict({base[0], base[2]}));                 // the output went away
    CHECK(verdict({base[1], base[2]}));                 // the input went away
    Devices renamed = base;
    renamed[1].name = "Other Speakers";                 // the id now names another device
    CHECK(verdict(renamed));
    Devices lostOutputs = base;
    lostOutputs[1].outputs = 0;                         // it no longer has outputs
    CHECK(verdict(lostOutputs));
    Devices lostInputs = base;
    lostInputs[0].inputs = 0;
    CHECK(verdict(lostInputs));
    // A Bluetooth headset that reconnects comes back under a new id with the same name.
    Devices airpodsBefore = {dev(150, "AirPods Pro", 0, 2, true), dev(151, "AirPods Pro", 1, 0), base[0]};
    AudioStreamOpen onAirpods = open;
    onAirpods.outId = 150;
    onAirpods.outName = "AirPods Pro";
    CHECK(!audio_device_change(onAirpods, airpodsBefore, true, true, "MacBook Pro Microphone").reopen);
    Devices airpodsAgain = {dev(152, "AirPods Pro", 0, 2, true), dev(153, "AirPods Pro", 1, 0), base[0]};
    CHECK(audio_device_change(onAirpods, airpodsAgain, true, true, "MacBook Pro Microphone").reopen);
    // The output half of AirPods is the stream's, not the same-named input half.
    Devices inputHalfOnly = {dev(151, "AirPods Pro", 1, 0), base[0], base[1]};
    CHECK(audio_device_change(onAirpods, inputHalfOnly, true, true, "MacBook Pro Microphone").reopen);
  }
  // The system output moved (headphones plugged in, AirPods connected).
  {
    Devices headphones = {base[0], dev(131, "MacBook Pro Speakers", 0, 2, false), dev(160, "External Headphones", 0, 2, true)};
    CHECK(verdict(headphones));                         // following the system output: move there
    CHECK(!verdict(headphones, false));                 // an output picked in Preferences: stay
  }
  // Input off: the mic does not matter, and its absence is not a reason.
  {
    AudioStreamOpen outOnly = open;
    outOnly.inputOpen = false;
    CHECK(!audio_device_change(outOnly, {base[1]}, true, false, "MacBook Pro Microphone").reopen);
    CHECK(!audio_device_change(outOnly, base, true, false, "").reopen);
  }
  // Input wanted but not open (it would not open, the stream fell back to output only): its
  // device being listed is worth one more try; another device appearing is not.
  {
    AudioStreamOpen fellBack = open;
    fellBack.inputOpen = false;
    CHECK(audio_device_change(fellBack, base, true, true, "MacBook Pro Microphone").reopen);
    CHECK(!audio_device_change(fellBack, {base[1], base[2]}, true, true, "MacBook Pro Microphone").reopen);
    CHECK(!audio_device_change(fellBack, base, true, true, "").reopen);
  }
  // No stream (NO AUDIO): any change may make it work, try.
  {
    AudioStreamOpen none;
    CHECK(audio_device_change(none, base, true, true, "MacBook Pro Microphone").reopen);
    CHECK(audio_device_change(none, {}, false, false, "").reopen);
  }

  // The heartbeat: a stream whose callback stops is reopened after a second, not during the
  // grace after opening, not across a long gap between checks (sleep), and not in a loop.
  {
    AudioHeartbeat hb;
    CHECK(!hb.check(0, 0.0));                           // nothing open: never
    hb.opened(10.0);
    std::uint64_t count = 0;
    double t = 10.0;
    for (; t < 15.0; t += 0.03) CHECK(!hb.check(count += 4, t));  // callbacks running
    for (double end = t + 0.9; t < end; t += 0.03) CHECK(!hb.check(count, t));  // stopped < 1 s
    bool stalled = false;
    for (double end = t + 0.3; t < end && !stalled; t += 0.03) stalled = hb.check(count, t);
    CHECK(stalled);                                     // stopped for a second: reopen
    CHECK(!hb.check(count, t + 0.03));                  // once (the reopen opens again)
    // Right after an open, a slow first callback is not a stall.
    hb.opened(t);
    for (double end = t + 1.9; t < end; t += 0.03) CHECK(!hb.check(count, t));
    // A long gap between checks (the Mac slept) starts the clock over.
    hb.opened(100.0);
    CHECK(!hb.check(1, 103.0));
    CHECK(!hb.check(1, 104.5));                         // 1.5 s gap: start over
    CHECK(!hb.check(1, 104.9));
    CHECK(!hb.check(1, 105.3));
    CHECK(hb.check(1, 105.6));                          // now a real second without callbacks
    hb.closed();
    CHECK(!hb.check(1, 200.0));                         // closed: never
    // A device that keeps stalling: three reopens in a row, then give up.
    AudioHeartbeat stuck;
    int reopens = 0;
    double s = 0.0;
    for (int i = 0; i < 6; ++i) {
      stuck.opened(s);
      for (double end = s + 4.0; s < end; s += 0.1) reopens += stuck.check(7, s) ? 1 : 0;
    }
    CHECK_EQ(reopens, AudioHeartbeat::kMaxStallReopens);
    CHECK(stuck.gaveUp());
    // Running healthily for a while after a stall clears the count.
    AudioHeartbeat recovers;
    double u = 0.0;
    std::uint64_t c = 0;
    int stalls = 0;
    for (int i = 0; i < 2; ++i) {                     // two stalls in a row
      recovers.opened(u);
      for (double end = u + 4.0; u < end; u += 0.1) stalls += recovers.check(c, u) ? 1 : 0;
    }
    CHECK_EQ(stalls, 2);
    recovers.opened(u);
    for (double end = u + 11.0; u < end; u += 0.05) CHECK(!recovers.check(c += 3, u));  // healthy
    stalls = 0;
    for (int i = 0; i < 5; ++i) {                     // the full allowance again
      recovers.opened(u);
      for (double end = u + 4.0; u < end; u += 0.1) stalls += recovers.check(c, u) ? 1 : 0;
    }
    CHECK_EQ(stalls, AudioHeartbeat::kMaxStallReopens);
  }
  return test::finish("test_audio_device_watch");
}
