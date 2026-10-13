// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// MIDI bindings at the engine: the published snapshot's match precedence, absolute
// pickup (no value jumps), relative-encoder deltas, and the action targets (drone
// keys, mute, cartridge step, preset load) with their audio-thread/UI split.

#include "mini_test.h"

#include <cstdio>
#include <cmath>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

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
    auto heap = std::make_unique<E>();
    E& e = *heap;
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
  // Several input devices: a device-named row answers only that device's messages, every
  // device plays with its own settings, and a switch key learned on one device is that
  // device's alone. A plugin (no device names) matches every row whatever the device.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("iPad", 1, 20, core::ParameterId::effector_blend)));
    CHECK(m.bind(bindCc("MPK mini IV", 1, 20, core::ParameterId::effector_master)));
    CHECK(m.bind(bindCc("", 1, 21, core::ParameterId::vcf_l_res)));
    E::MidiInputSet inputs;
    std::snprintf(inputs.name[0], sizeof(inputs.name[0]), "MPK mini IV");
    std::snprintf(inputs.name[1], sizeof(inputs.name[1]), "iPad");
    inputs.rig[0] = core::MidiRigSettings{0, 12, core::MidiVelocityCurve::soft, 48};
    inputs.rig[1] = core::MidiRigSettings{10, -12, core::MidiVelocityCurve::hard, 72};
    core::MidiGlobalSettings g;
    g.switchKey[1] = bindCc("iPad", 1, 30, core::ParameterId::effector_blend).key;
    g.switchBound[1] = true;
    const core::MidiMap sw = core::midi_switch_map(g);
    e.publishMidiMap(m, "MPK mini IV", {}, true, &sw, true, &inputs);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 0), 1);   // the MPK's row
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 1), 0);   // the iPad's row
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 2), -1);  // a device with no name
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 21, 0), 2);   // a generic row: every device
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 21, 1), 2);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 21, 3), 2);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 30, 0), -1);  // the iPad's switch key
    CHECK(e.midiBindingRow(1, core::MidiBindingKind::cc, 30, 1) >= E::kMidiSwitchRowBase);
    {
      E::MidiMessageScope message(e);
      CHECK_EQ(message.settings(0).octaveShift, 12);
      CHECK(message.settings(0).velocityCurve == core::MidiVelocityCurve::soft);
      CHECK_EQ(message.settings(1).channelFilter, 10);
      CHECK_EQ(message.settings(1).splitNote, 72);
      CHECK_EQ(message.settings(2).octaveShift, 0);  // a device not set up: the defaults
    }
    // The single-input call: device 0 is the named input, and every device plays its settings.
    const core::MidiRigSettings one{5, 24, core::MidiVelocityCurve::hard, 60};
    e.publishMidiMap(m, "iPad", one);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 0), 0);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 1), -1);
    {
      E::MidiMessageScope message(e);
      for (unsigned d = 0; d < core::kMidiMaxInputDevices; ++d) {
        CHECK_EQ(message.settings(d).channelFilter, 5);
        CHECK_EQ(message.settings(d).octaveShift, 24);
      }
    }
    // A plugin: the DAW's MIDI has no device, so the first device-named row matches.
    e.publishMidiMap(m, nullptr);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 0), 0);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20, 2), 0);
  }

  // Absolute pickup: nothing moves until the controller crosses the current value.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
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
    const core::MidiRigSettings settings{0, 12, core::MidiVelocityCurve::soft, 48};
    e.publishMidiMap(m, "TestKit", settings, false);
    e.applyMidiBindingFromAudioThread(0, 64);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::vcf_l_freq) - core::knob_to_value(*d, 64.0 / 127)) < 1e-9);
    const auto beforeSwitch = e.parameterValue(core::ParameterId::vcf_l_freq);
    e.publishMidiMap(m, "TestKit", settings);  // a profile switch DOES re-arm pickup
    e.applyMidiBindingFromAudioThread(0, 0);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.parameterValue(core::ParameterId::vcf_l_freq), beforeSwitch);

  }

  // Relative (bin-offset): a slow tick steps the parameter by 1/512 of its range, ticks in
  // quick succession by 4/512 (a spun encoder), and it clamps at the maximum.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("TestKit", 0, 22, core::ParameterId::effector_blend,
                        core::MidiInputMode::relativeBinOffset)));
    e.publishMidiMap(m, "TestKit");
    const core::ParameterDescriptor* d = core::find_parameter(core::ParameterId::effector_blend);
    CHECK(d != nullptr);
    const double base = e.parameterValue(core::ParameterId::effector_blend);
    const double step = (d->max - d->min) / 512.0;
    for (int i = 0; i < 5; ++i) e.applyMidiBindingFromAudioThread(0, 65);  // +1, a quick burst
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - (base + (1 + 4 * 4) * step)) < 1e-9);
    for (int i = 0; i < 48; ++i) e.processBlock(nullptr, outs, 0, 2, 256);  // a quarter second later
    const double before = e.parameterValue(core::ParameterId::effector_blend);
    e.applyMidiBindingFromAudioThread(0, 65);  // a slow tick: fine again
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - (before + step)) < 1e-9);
    for (int i = 0; i < 300; ++i) e.applyMidiBindingFromAudioThread(0, 65);
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK(e.parameterValue(core::ParameterId::effector_blend) == d->max);
  }

  // Drone-key action: heard at once (the runtime's voice state flips immediately,
  // before the UI records it), then the engine record follows on sync.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(!e.droneKey(0));  // the app starts with every DRONE VOICES key closed
    CHECK(!e.runtime()->droneVoiceKey(0));
    CHECK(e.postDroneKey(0, true));
    for (int i = 0; i < 20; ++i) e.processBlock(nullptr, outs, 0, 2, 256);
    CHECK(e.panelLed(E::kLedDrone1) > 0.0f);  // rising from silent (the drone's ATTACK)
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
    auto heap = std::make_unique<E>();
    E& e = *heap;
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

  // Cartridge action: the UI puts the next cartridge in the slot on sync; nothing loads until
  // a side's 1-2-3 switch flips (a MIDI-bound switch here), and then only that side loads it.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    const auto before = e.canonicalState()->leftEffector.program;
    const int cart = static_cast<int>(static_cast<std::uint32_t>(before) / 3u);
    core::MidiMap m;
    CHECK(m.bind(bindAction(0, 41, core::MidiAction::cartridge_next)));
    CHECK(m.bind(bindCc("", 0, 23, core::ParameterId::effector_select_r, core::MidiInputMode::absolute)));
    e.publishMidiMap(m, "TestKit");
    e.applyMidiBindingFromAudioThread(0, 127);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.slotCartridge(), (cart + 1) % 13);
    CHECK(e.canonicalState()->leftEffector.program == before);   // nothing loaded
    CHECK(e.canonicalState()->rightEffector.program == before);
    e.applyMidiBindingFromAudioThread(1, 0);                    // R switch moved (picks up at 1)
    e.syncParametersFromAudioThread();
    CHECK_EQ(static_cast<int>(static_cast<std::uint32_t>(e.canonicalState()->rightEffector.program) / 3u),
             (cart + 1) % 13);
    CHECK(e.canonicalState()->leftEffector.program == before);   // L keeps its cartridge
  }

  // Preset action: the UI loads the slot on sync.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
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

  // A publication cannot recycle a snapshot still used between lookup and apply.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap a, b;
    CHECK(a.bind(bindCc("", 0, 22, core::ParameterId::effector_blend,
                        core::MidiInputMode::relativeBinOffset)));
    CHECK(b.bind(bindCc("", 0, 22, core::ParameterId::effector_master,
                        core::MidiInputMode::relativeBinOffset)));
    e.publishMidiMap(a, "");
    const int row = e.midiBindingRow(1, core::MidiBindingKind::cc, 22);
    const double blend = e.parameterValue(core::ParameterId::effector_blend);
    const double master = e.parameterValue(core::ParameterId::effector_master);
    for (int i = 0; i < 5; ++i) e.publishMidiMap(b, "");
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 65);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - blend - 1.0/512) < 1e-9);
    CHECK_EQ(e.parameterValue(core::ParameterId::effector_master), master);
    // Real producer/consumer overlap (also run this target under ThreadSanitizer).
    std::atomic<bool> start{false};
    std::thread producer([&] {
      while (!start.load(std::memory_order_acquire)) {}
      for (int i = 0; i < 20000; ++i) e.publishMidiMap(i % 2 ? a : b, "");
    });
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 20000; ++i) {
      const int match = e.midiBindingRow(1, core::MidiBindingKind::cc, 22);
      CHECK_EQ(match, 0);
      e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(match), 64);
    }
    producer.join();
  }

  // The first encoder message uses panel edits made AFTER publication.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("", 0, 22, core::ParameterId::effector_blend,
                        core::MidiInputMode::relativeBinOffset)));
    e.publishMidiMap(m, "");
    CHECK(e.postParameter(core::ParameterId::effector_blend, .8));
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.applyMidiBindingFromAudioThread(0, 65);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - .8 - 1.0/512) < 1e-9);
  }

  // Discrete selectors get the SAME snapped value in DSP and saved state.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("", 0, 22, core::ParameterId::vco_a_oct_sel)));
    e.publishMidiMap(m, "");
    e.applyMidiBindingFromAudioThread(0, 0);
    e.applyMidiBindingFromAudioThread(0, 100);
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.parameterValue(core::ParameterId::vco_a_oct_sel), 2.0);
    CHECK_EQ(e.runtime()->vcoAOctSelect(), 2);
  }

  // A CC button toggles on the rising edge, never on release or repeated high values.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    auto button = bindAction(0, 22, core::MidiAction::master_mute);
    button.key.kind = core::MidiBindingKind::cc;
    core::MidiMap m;
    CHECK(m.bind(button));
    e.publishMidiMap(m, "");
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(e.muted());
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(e.muted());  // repeated high must not toggle, even before release
    e.applyMidiBindingFromAudioThread(0, 0);
    CHECK(e.muted());
    e.applyMidiBindingFromAudioThread(0, 127);
    CHECK(!e.muted());
  }

  // A momentary CC button (127 on press, 0 on release) on an on/off panel switch flips it on
  // every press, starting with the first one; release and repeated highs do nothing.
  {
    auto heap = std::make_unique<E>();  // keep main's stack small on Windows (1 MB)
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("", 0, 20, core::ParameterId::vco_b_sub_sel)));
    e.publishMidiMap(m, "");
    CHECK(core::midi_parameter_drive(m.at(0)) == core::MidiParameterDrive::toggleOnPress);
    auto send = [&](int raw) {
      e.applyMidiBindingFromAudioThread(0, raw);
      e.processBlock(nullptr, outs, 0, 2, 256);
      e.syncParametersFromAudioThread();
      return e.parameterValue(core::ParameterId::vco_b_sub_sel);
    };
    CHECK_EQ(e.parameterValue(core::ParameterId::vco_b_sub_sel), 0.0);
    CHECK_EQ(send(127), 1.0);  // first press: on
    CHECK_EQ(send(127), 1.0);  // repeated high
    CHECK_EQ(send(0), 1.0);    // release keeps it on
    CHECK_EQ(send(127), 0.0);  // second press: off
    CHECK_EQ(send(0), 0.0);
    // A panel click in between: the next press flips from the panel's value.
    CHECK(e.postParameter(core::ParameterId::vco_b_sub_sel, 1.0));
    e.processBlock(nullptr, outs, 0, 2, 256);
    CHECK_EQ(send(127), 0.0);
  }

  // A pad (note) on a switch steps it: an on/off switch flips, a 3-way lever moves to its
  // next position and wraps. The velocity does not matter.
  {
    auto heap = std::make_unique<E>();  // keep main's stack small on Windows (1 MB)
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    auto toggle = bindCc("", 0, 40, core::ParameterId::vco_b_sub_sel);
    toggle.key.kind = core::MidiBindingKind::note;
    auto lever = bindCc("", 0, 41, core::ParameterId::vco_a_oct_sel);
    lever.key.kind = core::MidiBindingKind::note;
    core::MidiMap m;
    CHECK(m.bind(toggle));
    CHECK(m.bind(lever));
    e.publishMidiMap(m, "");
    CHECK(core::midi_parameter_drive(m.at(1)) == core::MidiParameterDrive::stepOnPress);
    auto hit = [&](std::uint32_t row, int velocity, core::ParameterId id) {
      e.applyMidiBindingFromAudioThread(row, velocity);
      e.processBlock(nullptr, outs, 0, 2, 256);
      e.syncParametersFromAudioThread();
      return e.parameterValue(id);
    };
    CHECK_EQ(hit(0, 100, core::ParameterId::vco_b_sub_sel), 1.0);
    CHECK_EQ(hit(0, 20, core::ParameterId::vco_b_sub_sel), 0.0);
    const double oct0 = e.parameterValue(core::ParameterId::vco_a_oct_sel);
    const double oct1 = hit(1, 90, core::ParameterId::vco_a_oct_sel);
    const double oct2 = hit(1, 90, core::ParameterId::vco_a_oct_sel);
    const double oct3 = hit(1, 90, core::ParameterId::vco_a_oct_sel);
    CHECK(oct1 != oct0 && oct2 != oct1 && oct2 != oct0);
    CHECK_EQ(oct3, oct0);  // three hits on a 3-way lever come back around
  }

  // Pickup engages at once when the controller already sits on the current value (also on
  // the very first message), and when it arrives exactly on the value from below.
  {
    auto heap = std::make_unique<E>();  // keep main's stack small on Windows (1 MB)
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("", 0, 21, core::ParameterId::effector_blend)));
    e.publishMidiMap(m, "");
    const core::ParameterDescriptor* d = core::find_parameter(core::ParameterId::effector_blend);
    auto send = [&](int raw) {
      e.applyMidiBindingFromAudioThread(0, raw);
      e.processBlock(nullptr, outs, 0, 2, 256);
      e.syncParametersFromAudioThread();
      return e.parameterValue(core::ParameterId::effector_blend);
    };
    const double base = e.parameterValue(core::ParameterId::effector_blend);
    const int baseRaw = static_cast<int>((base - d->min) / (d->max - d->min) * 127.0 + 0.5);
    CHECK(std::fabs(send(baseRaw) - (d->min + baseRaw / 127.0 * (d->max - d->min))) < 1e-9);
    CHECK(std::fabs(send(baseRaw + 5) - (d->min + (baseRaw + 5) / 127.0 * (d->max - d->min))) < 1e-9);
    // Re-armed by a panel edit: approach from below, engage exactly on the value.
    CHECK(e.postParameter(core::ParameterId::effector_blend, base));
    e.processBlock(nullptr, outs, 0, 2, 256);
    CHECK_EQ(send(baseRaw - 4), base);
    CHECK_EQ(send(baseRaw - 2), base);
    CHECK(std::fabs(send(baseRaw) - (d->min + baseRaw / 127.0 * (d->max - d->min))) < 1e-9);
  }

  // Both the panel and MIDI action use this atomic toggle. An even total of
  // concurrent toggles must preserve the initial state.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    std::atomic<bool> start{false};
    std::thread panel([&] {
      while (!start.load(std::memory_order_acquire)) {}
      for (int i = 0; i < 100001; ++i) e.toggleMuted();
    });
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 100001; ++i) e.toggleMuted();
    panel.join();
    CHECK(!e.muted());
    e.toggleMuted();
    CHECK(e.muted());
  }

  // A pad's velocity is an absolute value, not a pickup knob needing a sweep.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    auto pad = bindCc("", 0, 40, core::ParameterId::effector_blend);
    pad.key.kind = core::MidiBindingKind::note;
    core::MidiMap m;
    CHECK(m.bind(pad));
    e.publishMidiMap(m, "");
    e.applyMidiBindingFromAudioThread(0, 100);
    e.syncParametersFromAudioThread();
    CHECK(std::fabs(e.parameterValue(core::ParameterId::effector_blend) - 100.0/127) < 1e-9);
  }

  // Loading presets while the UI keeps editing must never read UI state on audio.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindCc("", 0, 22, core::ParameterId::keyboard_clock_bpm,
                        core::MidiInputMode::relativeBinOffset)));
    e.publishMidiMap(m, "");
    std::thread ui([&] {
      for (int i = 0; i < 10000; ++i) {
        (void)e.postKeyboardPreset(E::PresetAction::Load, 0);
        (void)e.postParameter(core::ParameterId::keyboard_clock_bpm, (i % 100) / 100.0);
      }
    });
    for (int i = 0; i < 100; ++i) e.processBlock(nullptr, outs, 0, 2, 256);
    ui.join();
  }

  // Photo sensors: a knob sets the hand (absolute, or relative steps); a pad puts it down
  // on a hit, presses closer with aftertouch and lifts it on release.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    auto knob = bindCc("", 0, 30, core::ParameterId::vcf_l_freq);
    knob.targetKind = core::MidiTargetKind::action;
    knob.action = core::MidiAction::photo_drone_1;
    CHECK(m.bind(knob));
    auto rel = bindCc("", 0, 31, core::ParameterId::vcf_l_freq, core::MidiInputMode::relativeTwosComplement);
    rel.targetKind = core::MidiTargetKind::action;
    rel.action = core::MidiAction::photo_drone_2;
    CHECK(m.bind(rel));
    CHECK(m.bind(bindAction(10, 36, core::MidiAction::photo_drone_5)));
    auto trigger = bindAction(0, 37, core::MidiAction::master_mute);
    trigger.mode = core::MidiInputMode::relativeTwosComplement;
    CHECK(!m.bind(trigger));  // a press action stays absolute
    e.publishMidiMap(m, "Pads");

    int row = e.midiBindingRow(1, core::MidiBindingKind::cc, 30);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 127);
    CHECK(std::fabs(e.runtime()->dronePhotoShade(0) - 1.0) < 1e-9);
    row = e.midiBindingRow(1, core::MidiBindingKind::cc, 30);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 0);
    CHECK_EQ(e.runtime()->dronePhotoShade(0), 0.0);

    for (int i = 0; i < 16; ++i) {  // 16 steps up = a quarter of the way
      row = e.midiBindingRow(1, core::MidiBindingKind::cc, 31);
      e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 1);
    }
    CHECK(std::fabs(e.runtime()->dronePhotoShade(1) - 0.25) < 1e-9);
    row = e.midiBindingRow(1, core::MidiBindingKind::cc, 31);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 127);  // one step down
    CHECK(std::fabs(e.runtime()->dronePhotoShade(1) - 15.0 / 64.0) < 1e-9);

    row = e.midiBindingRow(10, core::MidiBindingKind::note, 36);
    CHECK(row >= 0);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 127, 10);
    CHECK(std::fabs(e.runtime()->dronePhotoShade(3) - 1.0) < 1e-9);
    CHECK(e.photoPadPressure(10, 36, 0.0));
    CHECK(std::fabs(e.runtime()->dronePhotoShade(3) - 0.4) < 1e-9);
    CHECK(e.photoPadPressure(10, -1, 1.0));  // channel aftertouch reaches the held pad
    CHECK(std::fabs(e.runtime()->dronePhotoShade(3) - 1.0) < 1e-9);
    CHECK(!e.photoPadPressure(1, -1, 0.5));  // another channel: the keyboard's aftertouch
    CHECK(!e.photoPadRelease(10, 37));
    CHECK(e.photoPadRelease(10, 36));
    CHECK_EQ(e.runtime()->dronePhotoShade(3), 0.0);
    CHECK(!e.photoPadPressure(10, 36, 0.5));  // released: no longer held
    // A DAW reset while the pad is held (its note-off may never come): the hand lifts; the
    // knob-driven hands stay where they are.
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 127, 10);
    const double knobShade = e.runtime()->dronePhotoShade(1);
    CHECK_EQ(e.releasePhotoPadsFromAudioThread(), 1);
    CHECK_EQ(e.runtime()->dronePhotoShade(3), 0.0);
    CHECK_EQ(e.runtime()->dronePhotoShade(1), knobShade);
    CHECK(!e.photoPadRelease(10, 36));  // the late note-off finds nothing
    CHECK_EQ(e.releasePhotoPadsFromAudioThread(), 0);
    // An app input closing (unplugged, or turned OFF in DEVICES) lifts only the hands its pads
    // hold: pads are known by lane + 1, input 2's channel 10 is 26.
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 127, 26);
    CHECK_EQ(e.releasePhotoPadsFromAudioThread(1, 16), 0);   // input 1 closed: not its pad
    CHECK(e.runtime()->dronePhotoShade(3) > 0.0);
    CHECK_EQ(e.releasePhotoPadsFromAudioThread(17, 32), 1);  // input 2 closed
    CHECK_EQ(e.runtime()->dronePhotoShade(3), 0.0);
    e.processBlock(nullptr, outs, 0, 2, 256);
  }
  // Filtering/settings and the matched binding stay on one snapshot, even when the
  // UI publishes a different profile between the message's filter and lookup.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap a, b;
    CHECK(a.bind(bindCc("Controller A", 3, 22, core::ParameterId::effector_blend,
                        core::MidiInputMode::relativeTwosComplement)));
    CHECK(b.bind(bindCc("Controller B", 9, 22, core::ParameterId::effector_master,
                        core::MidiInputMode::relativeBinOffset)));
    core::MidiRigSettings sa{3, 12, core::MidiVelocityCurve::soft, 48};
    core::MidiRigSettings sb{9, -12, core::MidiVelocityCurve::hard, 72};
    e.publishMidiMap(a, "Controller A", sa);
    {
      E::MidiMessageScope message(e);
      CHECK_EQ(message.settings().channelFilter, 3);
      e.publishMidiMap(b, "Controller B", sb);
      CHECK_EQ(message.settings().octaveShift, 12);
      CHECK_EQ(e.midiBindingRow(3, core::MidiBindingKind::cc, 22), 0);
      CHECK_EQ(e.midiBindingRow(9, core::MidiBindingKind::cc, 22), -1);
    }
    {
      E::MidiMessageScope message(e);
      CHECK_EQ(message.settings().channelFilter, 9);
      CHECK_EQ(message.settings().splitNote, 72);
      CHECK(message.settings().velocityCurve == core::MidiVelocityCurve::hard);
      CHECK_EQ(e.midiBindingRow(3, core::MidiBindingKind::cc, 22), -1);
      CHECK_EQ(e.midiBindingRow(9, core::MidiBindingKind::cc, 22), 0);
      e.applyMidiBindingFromAudioThread(0, 65);
    }
    std::thread publisher([&] {
      for (int i = 0; i < 20000; ++i)
        e.publishMidiMap(i % 2 ? a : b, i % 2 ? "Controller A" : "Controller B", i % 2 ? sa : sb);
    });
    for (int i = 0; i < 20000; ++i) {
      E::MidiMessageScope message(e);
      const auto& rig = message.settings();
      const bool first = rig.channelFilter == 3;
      CHECK_EQ(rig.octaveShift, first ? 12 : -12);
      CHECK_EQ(rig.splitNote, first ? 48 : 72);
      CHECK(rig.velocityCurve == (first ? core::MidiVelocityCurve::soft : core::MidiVelocityCurve::hard));
      CHECK_EQ(e.midiBindingRow(rig.channelFilter, core::MidiBindingKind::cc, 22), 0);
      CHECK_EQ(e.midiBindingRow(first ? 9 : 3, core::MidiBindingKind::cc, 22), -1);
    }
    publisher.join();
    // A held photo pad still releases its old target after switching to an empty map.
    core::MidiMap pads;
    CHECK(pads.bind(bindAction(10, 36, core::MidiAction::photo_drone_5)));
    e.publishMidiMap(pads, nullptr, sa);
    e.applyMidiBindingFromAudioThread(0, 127, 10);
    e.publishMidiMap(core::MidiMap{}, nullptr, sb);
    {
      E::MidiMessageScope message(e);
      CHECK_EQ(e.midiBindingRow(10, core::MidiBindingKind::note, 36), -1);
      CHECK(e.photoPadRelease(10, 36));
      CHECK_EQ(e.runtime()->dronePhotoShade(3), 0.0);
    }
  }
  // Profile switch keys: outrank profile rows, one step per press (pad hit or CC rising past
  // 64), swallowed while the MIDI panel is open, and stepped by the UI thread.
  {
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::MidiMap m;
    CHECK(m.bind(bindAction(10, 40, core::MidiAction::drone_key_1)));   // the same pad in the profile
    CHECK(m.bind(bindCc("TestKit", 1, 20, core::ParameterId::effector_blend)));
    core::MidiGlobalSettings g;
    g.switchKey[1] = bindAction(10, 40, core::MidiAction::drone_key_1).key;  // NEXT = pad 40, ch 10
    g.switchBound[1] = true;
    g.switchKey[0] = bindCc("TestKit", 1, 21, core::ParameterId::effector_blend).key;  // PREV = CC 21
    g.switchBound[0] = true;
    const core::MidiMap sw = core::midi_switch_map(g);
    e.publishMidiMap(m, "TestKit", {}, true, &sw, true);
    const int nextRow = e.midiBindingRow(10, core::MidiBindingKind::note, 40);
    CHECK(nextRow >= E::kMidiSwitchRowBase);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(nextRow), 100, 10);
    const int prevRow = e.midiBindingRow(1, core::MidiBindingKind::cc, 21);
    CHECK(prevRow >= E::kMidiSwitchRowBase);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(prevRow), 127);  // press
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(e.midiBindingRow(1, core::MidiBindingKind::cc, 21)), 127);  // held: no repeat
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(e.midiBindingRow(1, core::MidiBindingKind::cc, 21)), 0);    // release
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(e.midiBindingRow(1, core::MidiBindingKind::cc, 21)), 127);  // press again
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.takeProfileSteps(), 1 - 2);  // one NEXT, two PREVs
    CHECK_EQ(e.takeProfileSteps(), 0);
    CHECK(!e.runtime()->droneVoiceKey(0));   // the profile's pad row never fired
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20), 1);  // other rows unaffected
    // The MIDI panel is open: the key is still swallowed (never a note) but does not step.
    e.publishMidiMap(m, "TestKit", {}, false, &sw, false);
    const int closedRow = e.midiBindingRow(10, core::MidiBindingKind::note, 40);
    CHECK(closedRow >= E::kMidiSwitchRowBase);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(closedRow), 100, 10);
    e.processBlock(nullptr, outs, 0, 2, 256);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.takeProfileSteps(), 0);
    // No switch keys published: the profile row is back.
    e.publishMidiMap(m, "TestKit", {}, false);
    CHECK_EQ(e.midiBindingRow(10, core::MidiBindingKind::note, 40), 0);
    // A plugin (no device name; Live sends every note on channel 1): the switch key the app
    // learned on channel 10 still switches, and still never plays (owner's Live log, 2026-10-12).
    e.publishMidiMap(m, nullptr, {}, false, &sw, true);
    const int pluginRow = e.midiBindingRow(1, core::MidiBindingKind::note, 40);
    CHECK(pluginRow >= E::kMidiSwitchRowBase);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(pluginRow), 87, 1);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.takeProfileSteps(), 1);
    // Live routes the pads' track to channel 10 (owner's AU log, 2026-10-12): from the first
    // message on channel 10, rows on 10 answer 10 only, so a key on channel 1 with the pad's
    // note plays again; the pad still switches.
    const int routedRow = e.midiBindingRow(10, core::MidiBindingKind::note, 40);
    CHECK(routedRow >= E::kMidiSwitchRowBase);
    e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(routedRow), 90, 10);
    e.syncParametersFromAudioThread();
    CHECK_EQ(e.takeProfileSteps(), 1);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::note, 40), -1);
    CHECK(e.midiBindingRow(10, core::MidiBindingKind::note, 40) >= E::kMidiSwitchRowBase);
    CHECK_EQ(e.midiBindingRow(1, core::MidiBindingKind::cc, 20), 1);   // channel 1 is real too:
    CHECK_EQ(e.midiBindingRow(5, core::MidiBindingKind::cc, 20), -1);  // its rows answer 1 only
    // A switch key named for another device does not match this input.
    e.publishMidiMap(m, "OtherKit", {}, false, &sw, true);
    CHECK(e.midiBindingRow(1, core::MidiBindingKind::cc, 21) < E::kMidiSwitchRowBase);
  }
  return test::finish("test_midi_map_apply");
}
