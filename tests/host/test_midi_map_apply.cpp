// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// MIDI bindings at the engine: the published snapshot's match precedence, absolute
// pickup (no value jumps), relative-encoder deltas, and the action targets (drone
// keys, mute, cartridge step, preset load) with their audio-thread/UI split.

#include "mini_test.h"

#include <cstdio>
#include <cmath>
#include <vector>

#include <host/standalone_audio_engine.h>
#include <lunar24/core/midi_map.h>

using namespace lunar24;

static core::MidiBinding bindCc(const char* dev, std::uint8_t ch, std::uint8_t cc,
                                core::ParameterId p,
                                core::MidiInputMode mode = core::MidiInputMode::absolute) {
  core::MidiBinding b{};
  std::snprintf(b.key.device, core::kMidiBindingDeviceCapacity, "%s", dev);
  b.key.channel = ch;
  b.key.kind = core::MidiBindingKind::cc;
  b.key.number = cc;
  b.mode = mode;
  b.parameter = p;
  return b;
}

static core::MidiBinding bindAction(std::uint8_t ch, std::uint8_t note, core::MidiAction a) {
  core::MidiBinding b{};
  b.key.channel = ch;
  b.key.kind = core::MidiBindingKind::note;
  b.key.number = note;
  b.targetKind = core::MidiTargetKind::action;
  b.action = a;
  return b;
}

int main() {
  using E = host::StandaloneAudioEngine;
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};

  // Match precedence: exact device+channel > device-only > channel-only > wildcard;
  // a device-named binding never matches a different published input device.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("TestKit", 1, 74, core::ParameterId::vcf_l_freq)));
    CHECK(m.bind(bindCc("TestKit", 0, 74, core::ParameterId::vcf_l_res)));
    CHECK(m.bind(bindCc("", 2, 74, core::ParameterId::effector_blend)));
    CHECK(m.bind(bindCc("", 0, 74, core::ParameterId::effector_master)));
    e.publishMidiMap(m, "TestKit");
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 74), 0);   // exact device+channel
    CHECK_EQ(e.midiBindingRow(9, core::MidiBindingKind::cc, 74), 1);   // the device's any-channel row
    CHECK_EQ(e.midiBindingRow(2, core::MidiBindingKind::cc, 74), 1);   // device tier outranks the channel-only generic row
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 71), -1);  // unbound CC
    e.publishMidiMap(m, "OtherKit");  // the device-named rows go dormant
    CHECK_EQ(e.midiBindingRow(2, core::MidiBindingKind::cc, 74), 2);   // channel-only now surfaces
    CHECK_EQ(e.midiBindingRow(9, core::MidiBindingKind::cc, 74), 3);   // wildcard
  }

  // Absolute pickup: nothing moves until the controller crosses the current value.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("TestKit", 1, 74, core::ParameterId::vcf_l_freq)));
    e.publishMidiMap(m, "TestKit");
    const core::ParameterDescriptor* d = core::find_parameter(core::ParameterId::vcf_l_freq);
    CHECK(d != nullptr);
    const double base = e.parameterValue(core::ParameterId::vcf_l_freq);
    const int baseRaw = static_cast<int>((base - d->min) / (d->max - d->min) * 127.0 + 0.5);
    const int farRaw = baseRaw > 63 ? baseRaw - 40 : baseRaw + 40;
    e.applyMidiBindingFromAudioThread(0, farRaw);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    e.syncParametersFromAudioThread();
    CHECK(e.parameterValue(core::ParameterId::vcf_l_freq) == base);  // not engaged yet
    // Sweep across the base: engages, then tracks.
    const int step = farRaw < baseRaw ? 4 : -4;
    int raw = farRaw;
    bool engaged = false;
    for (int i = 0; i < 40 && !engaged; ++i) {
      e.applyMidiBindingFromAudioThread(0, raw);
      e.processBlock(nullptr, outs, 0, 2, 256);
      e.syncParametersFromAudioThread();
      engaged = e.parameterValue(core::ParameterId::vcf_l_freq) != base;
      if (!engaged) raw += step;
    }
    CHECK(engaged);
    // Once engaged the parameter follows the controller.
    e.applyMidiBindingFromAudioThread(0, 127);
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::vcf_l_freq) - d->max) < 1e-9);
  }

  // Relative (bin-offset): each +1 tick steps the parameter by 1/127 of its range and
  // it clamps at the maximum.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("TestKit", 0, 22, core::ParameterId::effector_blend,
                        core::MidiInputMode::relativeBinOffset)));
    e.publishMidiMap(m, "TestKit");
    const core::ParameterDescriptor* d = core::find_parameter(core::ParameterId::effector_blend);
    CHECK(d != nullptr);
    const double base = e.parameterValue(core::ParameterId::effector_blend);
    const double step = (d->max - d->min) / 127.0;
    for (int i = 0; i < 5; ++i) e.applyMidiBindingFromAudioThread(0, 65);  // +1
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - (base + 5 * step)) < 1e-9);
    for (int i = 0; i < 300; ++i) e.applyMidiBindingFromAudioThread(0, 65);
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK(e.parameterValue(core::ParameterId::effector_blend) == d->max);
  }

  // Drone-key action: heard at once (the runtime's voice state flips immediately,
  // before the UI records it), then the engine record follows on sync.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    for (int i = 0; i < 20; ++i) e.processBlock(nullptr, outs, 0, 2, 256);
    CHECK(e.panelLed(E::kLedDrone1) > 0.9f);
    CHECK(e.droneKey(0));
    core::MidiMap m;
    CHECK(m.bind(bindAction(10, 36, core::MidiAction::drone_key_1)));
    e.publishMidiMap(m, "TestKit");
    CHECK_EQ(e.midiBindingRow(10, core::MidiBindingKind::note, 36), 0);
    e.applyMidiBindingFromAudioThread(0, 100);
    CHECK(!e.runtime()->droneVoiceKey(0));  // the runtime closed the voice at once
    CHECK(e.droneKey(0));                   // the UI-side record lags until sync
    e.syncParametersFromAudioThread();
    CHECK(!e.droneKey(0));
    e.applyMidiBindingFromAudioThread(0, 100);  // toggle back on
    CHECK(e.runtime()->droneVoiceKey(0));
    e.syncParametersFromAudioThread();
    CHECK(e.droneKey(0));
  }

  // Mute action: immediate, nothing to record.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindAction(0, 40, core::MidiAction::master_mute)));
    e.publishMidiMap(m, "TestKit");
    CHECK(!e.muted());
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(e.muted());
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(!e.muted());
  }

  // Cartridge action: the UI steps both sides' programs on sync (cartridge = program/3).
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    const auto before = e.canonicalState()->leftEffector.program;
    core::MidiMap m;
    CHECK(m.bind(bindAction(0, 41, core::MidiAction::cartridge_next)));
    e.publishMidiMap(m, "TestKit");
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(e.canonicalState()->leftEffector.program == before);  // nothing yet
    e.syncParametersFromAudioThread();
    CHECK(static_cast<std::uint32_t>(e.canonicalState()->leftEffector.program) ==
          (static_cast<std::uint32_t>(before) + 3) % 39);
    CHECK(e.canonicalState()->leftEffector.program == e.canonicalState()->rightEffector.program);
  }

  // Preset action: the UI loads the slot on sync.
  {
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 1.0));       // arpeggiator
    CHECK(e.postKeyboardPreset(E::PresetAction::Save, 1));               // stash into B
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 0.0));       // back to keyboard
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    core::MidiMap m;
    CHECK(m.bind(bindAction(0, 42, core::MidiAction::preset_load_b)));
    e.publishMidiMap(m, "TestKit");
    e.applyMidiBindingFromAudioThread(0, 127);
    e.syncParametersFromAudioThread();
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.parameterValue(core::ParameterId::keyboard_mode) == 1.0);    // preset B is back
  }

  return test::finish("test_midi_map_apply");
}
