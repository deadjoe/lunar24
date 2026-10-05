// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// MIDI bindings: the ordered map's edit rules (replace-in-place, ordered removal,
// capacity, field validation) and the versioned wire format (round-trip, and
// all-or-nothing rejection of malformed files).

#include "mini_test.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include <lunar24/core/midi_map.h>

using namespace lunar24::core;

static const MidiRigSettings kDefaults{};

static MidiBinding ccBinding(const char* dev, std::uint8_t ch, std::uint8_t num, ParameterId p) {
  MidiBinding b{};
  std::snprintf(b.key.device, kMidiBindingDeviceCapacity, "%s", dev);
  b.key.channel = ch;
  b.key.kind = MidiBindingKind::cc;
  b.key.number = num;
  b.parameter = p;
  return b;
}

static MidiBindingKey keyOf(const char* dev, std::uint8_t ch, MidiBindingKind kind,
                            std::uint8_t num) {
  MidiBindingKey k{};
  std::snprintf(k.device, kMidiBindingDeviceCapacity, "%s", dev);
  k.channel = ch;
  k.kind = kind;
  k.number = num;
  return k;
}

int main() {
  // bind / find / replace-in-place / ordered unbind / clear.
  {
    MidiMap m;
    CHECK_EQ(m.count(), 0u);
    CHECK(m.bind(ccBinding("MPK mini IV", 1, 74, ParameterId::vcf_l_freq)));
    CHECK(m.bind(ccBinding("MPK mini IV", 1, 71, ParameterId::vcf_l_res)));
    CHECK_EQ(m.count(), 2u);
    CHECK_EQ(m.find(keyOf("MPK mini IV", 1, MidiBindingKind::cc, 74)), 0);
    // Same hardware message again: the target is replaced, the row keeps its place.
    CHECK(m.bind(ccBinding("MPK mini IV", 1, 74, ParameterId::vcf_r_freq)));
    CHECK_EQ(m.count(), 2u);
    CHECK(m.at(0).parameter == ParameterId::vcf_r_freq);
    CHECK(m.at(1).parameter == ParameterId::vcf_l_res);
    // Another channel or another device is a DIFFERENT binding.
    CHECK(m.bind(ccBinding("MPK mini IV", 2, 74, ParameterId::vcf_l_freq)));
    CHECK(m.bind(ccBinding("Other Board", 1, 74, ParameterId::effector_master)));
    CHECK_EQ(m.count(), 4u);
    // Unbind keeps the survivors' order.
    CHECK(m.unbind(keyOf("MPK mini IV", 1, MidiBindingKind::cc, 74)));
    CHECK_EQ(m.count(), 3u);
    CHECK(m.at(0).parameter == ParameterId::vcf_l_res);
    CHECK(!m.unbind(keyOf("MPK mini IV", 1, MidiBindingKind::cc, 74)));
    m.clear();
    CHECK_EQ(m.count(), 0u);
  }
  // Capacity: the map is full exactly at kMidiMapCapacity; a new key is refused but a
  // replace still lands.
  {
    MidiMap m;
    for (std::uint32_t i = 0; i < kMidiMapCapacity; ++i) {
      char dev[32];
      std::snprintf(dev, sizeof(dev), "device-%u", i);
      CHECK(m.bind(ccBinding(dev, 1, 74, ParameterId::vcf_l_freq)));
    }
    CHECK_EQ(m.count(), kMidiMapCapacity);
    CHECK(!m.bind(ccBinding("one-more", 1, 74, ParameterId::vcf_l_freq)));
    CHECK(m.bind(ccBinding("device-7", 1, 74, ParameterId::vcf_r_freq)));
    CHECK_EQ(m.count(), kMidiMapCapacity);
  }
  // Field validation.
  {
    MidiMap m;
    MidiBinding b = ccBinding("X", 17, 74, ParameterId::vcf_l_freq);  // channel 17
    CHECK(!m.bind(b));
    b = ccBinding("X", 1, 200, ParameterId::vcf_l_freq);  // CC number > 127
    CHECK(!m.bind(b));
    b = ccBinding("X", 1, 74, ParameterId::vcf_l_freq);
    std::memset(b.key.device, 'a', kMidiBindingDeviceCapacity);  // no NUL terminator
    CHECK(!m.bind(b));
    b = ccBinding("X", 1, 74, static_cast<ParameterId>(kParameterCount));  // unknown parameter
    CHECK(!m.bind(b));
    b = ccBinding("X", 1, 74, ParameterId::vcf_l_freq);
    b.mode = static_cast<MidiInputMode>(9);  // unknown input mode
    CHECK(!m.bind(b));
    b = ccBinding("X", 1, 74, ParameterId::vcf_l_freq);
    b.targetKind = MidiTargetKind::action;
    b.action = MidiAction::drone_key_4;  // a valid action binding
    CHECK(m.bind(b));
    CHECK_EQ(m.at(0).action, MidiAction::drone_key_4);
    b.action = static_cast<MidiAction>(kMidiActionCount);  // unknown action
    CHECK(!m.bind(b));
    b.action = MidiAction::drone_key_4;
    b.mode = MidiInputMode::relativeBinOffset;  // actions keep the canonical mode
    CHECK(!m.bind(b));
    b.action = MidiAction::photo_drone_4;  // ... except a photo sensor, a continuous target
    CHECK(m.bind(b));
    MidiRigSettings rig{};
    std::vector<std::uint8_t> bytes(midi_map_wire_bytes(m.count()));
    CHECK_EQ(midi_map_encode(m, rig, bytes.data(), bytes.size()), bytes.size());
    MidiMap back;
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &back, &rig));
    CHECK_EQ(back.at(0).action, MidiAction::photo_drone_4);
    CHECK_EQ(back.at(0).mode, MidiInputMode::relativeBinOffset);
  }
  // Wire round-trip: empty and populated maps, field by field.
  {
    MidiMap m;
    std::vector<std::uint8_t> bytes(midi_map_wire_bytes(0));
    CHECK_EQ(midi_map_encode(m, kDefaults, bytes.data(), bytes.size()),
             kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes);
    MidiMap back;
    MidiRigSettings rig;
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &back, &rig));
    CHECK_EQ(back.count(), 0u);
    CHECK_EQ(rig.channelFilter, 0u);

    CHECK(m.bind(ccBinding("MPK mini IV", 1, 74, ParameterId::vcf_l_freq)));
    MidiBinding rel = ccBinding("LinnStrument", 0, 22, ParameterId::effector_blend);
    rel.mode = MidiInputMode::relativeTwosComplement;
    CHECK(m.bind(rel));
    MidiBinding act = ccBinding("MPK mini IV", 10, 36, ParameterId::vcf_l_freq);
    act.key.kind = MidiBindingKind::note;
    act.targetKind = MidiTargetKind::action;
    act.action = MidiAction::cartridge_next;
    CHECK(m.bind(act));

    MidiRigSettings custom;
    custom.channelFilter = 10;
    custom.octaveShift = -12;
    custom.velocityCurve = MidiVelocityCurve::hard;
    custom.splitNote = 55;  // G3
    bytes.assign(midi_map_wire_bytes(m.count()), 0u);
    CHECK_EQ(midi_map_encode(m, custom, bytes.data(), bytes.size()), bytes.size());
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &back, &rig));
    CHECK_EQ(back.count(), 3u);
    CHECK(back.at(0).key.equals(keyOf("MPK mini IV", 1, MidiBindingKind::cc, 74)));
    CHECK(back.at(0).parameter == ParameterId::vcf_l_freq);
    CHECK(back.at(1).mode == MidiInputMode::relativeTwosComplement);
    CHECK(back.at(2).key.kind == MidiBindingKind::note);
    CHECK(back.at(2).targetKind == MidiTargetKind::action);
    CHECK(back.at(2).action == MidiAction::cartridge_next);
    CHECK_EQ(rig.channelFilter, 10u);
    CHECK_EQ(rig.octaveShift, -12);
    CHECK(rig.velocityCurve == MidiVelocityCurve::hard);
    CHECK_EQ(rig.splitNote, 55u);
    // A file written before SPLIT existed has a zero there: the default C4.
    bytes[kMidiMapWireHeaderBytes + 3] = 0;
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &back, &rig));
    CHECK_EQ(rig.splitNote, kMidiDefaultSplitNote);
    // Outside C1..C7 is rejected.
    bytes[kMidiMapWireHeaderBytes + 3] = 120;
    CHECK(!midi_map_decode(bytes.data(), bytes.size(), &back, &rig));
  }
  // Encode: a too-small buffer fails without writing.
  {
    MidiMap m;
    CHECK(m.bind(ccBinding("X", 1, 74, ParameterId::vcf_l_freq)));
    std::vector<std::uint8_t> bytes(4, 0xAB);
    CHECK_EQ(midi_map_encode(m, kDefaults, bytes.data(), bytes.size()), 0u);
    CHECK_EQ(bytes[0], 0xAB);  // untouched
  }
  // Decode: malformed files are rejected all-or-nothing.
  {
    MidiMap m;
    CHECK(m.bind(ccBinding("X", 1, 74, ParameterId::vcf_l_freq)));
    std::vector<std::uint8_t> good(midi_map_wire_bytes(m.count()));
    CHECK_EQ(midi_map_encode(m, kDefaults, good.data(), good.size()), good.size());
    MidiMap out;
    MidiRigSettings rigOut;
    CHECK(!midi_map_decode(nullptr, good.size(), &out, &rigOut));
    CHECK(!midi_map_decode(good.data(), 4, &out, &rigOut));           // truncated header
    CHECK(!midi_map_decode(good.data(), good.size() - 1, &out, &rigOut));  // truncated record
    CHECK(!midi_map_decode(good.data(), good.size() + 1, &out, &rigOut));  // grown
    auto bad = good;
    bad[0] = 'X';                                            // magic
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    put_u32le(bad.data() + 4, 99);                           // unsupported version
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    put_u32le(bad.data() + 8, kMidiMapCapacity + 1);         // over-capacity count
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    bad[kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes + 64] = 17;  // channel out of range
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    bad[kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes + 70] = 1;   // reserved byte set
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    std::memset(bad.data() + kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes, 'a',
                kMidiBindingDeviceCapacity);
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));   // unterminated device name
    bad = good;
    bad[kMidiMapWireHeaderBytes + 3] = 1;                    // globals reserved byte set
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    bad = good;
    bad[kMidiMapWireHeaderBytes + 0] = 17;                   // channel filter out of range
    CHECK(!midi_map_decode(bad.data(), bad.size(), &out, &rigOut));
    // A rejected decode writes nothing.
    CHECK_EQ(out.count(), 0u);
  }
  // A version-1 file (no globals block) decodes with default rig settings.
  {
    MidiMap m;
    CHECK(m.bind(ccBinding("X", 1, 74, ParameterId::vcf_l_freq)));
    const std::size_t v1Bytes = kMidiMapWireHeaderBytes + kMidiBindingWireBytes;
    std::vector<std::uint8_t> bytes(v1Bytes);
    std::memcpy(bytes.data(), kMidiMapWireMagic, 4);
    put_u32le(bytes.data() + 4, 1);
    put_u32le(bytes.data() + 8, 1);
    std::vector<std::uint8_t> rec(midi_map_wire_bytes(1));
    CHECK_EQ(midi_map_encode(m, kDefaults, rec.data(), rec.size()), rec.size());
    std::memcpy(bytes.data() + kMidiMapWireHeaderBytes,
                rec.data() + kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes,
                kMidiBindingWireBytes);
    MidiMap out;
    MidiRigSettings rigOut;
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &out, &rigOut));
    CHECK_EQ(out.count(), 1u);
    CHECK_EQ(rigOut.channelFilter, 0u);
    CHECK_EQ(rigOut.octaveShift, 0);
    CHECK_EQ(rigOut.splitNote, kMidiDefaultSplitNote);
  }
  // Duplicate keys in one file collapse to the LAST record (same rule as bind()).
  // Hand-built as a version-1 file, so this also covers the v1 record layout.
  {
    std::vector<std::uint8_t> bytes(kMidiMapWireHeaderBytes + 2 * kMidiBindingWireBytes);
    std::memcpy(bytes.data(), kMidiMapWireMagic, 4);
    put_u32le(bytes.data() + 4, 1);
    put_u32le(bytes.data() + 8, 2);
    MidiMap one;
    CHECK(one.bind(ccBinding("X", 1, 74, ParameterId::vcf_l_freq)));
    std::vector<std::uint8_t> rec(midi_map_wire_bytes(1));
    CHECK_EQ(midi_map_encode(one, kDefaults, rec.data(), rec.size()), rec.size());
    std::memcpy(bytes.data() + kMidiMapWireHeaderBytes,
                rec.data() + kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes,
                kMidiBindingWireBytes);
    MidiMap two;
    CHECK(two.bind(ccBinding("X", 1, 74, ParameterId::vcf_r_freq)));
    CHECK_EQ(midi_map_encode(two, kDefaults, rec.data(), rec.size()), rec.size());
    std::memcpy(bytes.data() + kMidiMapWireHeaderBytes + kMidiBindingWireBytes,
                rec.data() + kMidiMapWireHeaderBytes + kMidiMapWireGlobalsBytes,
                kMidiBindingWireBytes);
    MidiMap out;
    MidiRigSettings rigOut;
    CHECK(midi_map_decode(bytes.data(), bytes.size(), &out, &rigOut));
    CHECK_EQ(out.count(), 1u);
    CHECK(out.at(0).parameter == ParameterId::vcf_r_freq);
  }
  // midi_map_find: tiered matching (device+channel > device > channel > wildcard).
  {
    MidiMap m;
    CHECK(m.bind(ccBinding("Kit", 1, 74, ParameterId::vcf_l_freq)));
    CHECK(m.bind(ccBinding("Kit", 0, 74, ParameterId::vcf_l_res)));
    CHECK(m.bind(ccBinding("", 2, 74, ParameterId::effector_blend)));
    CHECK(m.bind(ccBinding("", 0, 74, ParameterId::effector_master)));
    const auto cc = MidiBindingKind::cc;
    CHECK(midi_map_find(m, "Kit", 1, cc, 74)->parameter == ParameterId::vcf_l_freq);
    CHECK(midi_map_find(m, "Kit", 9, cc, 74)->parameter == ParameterId::vcf_l_res);
    CHECK(midi_map_find(m, "Kit", 2, cc, 74)->parameter == ParameterId::vcf_l_res);  // device tier wins
    CHECK(midi_map_find(m, "Other", 2, cc, 74)->parameter == ParameterId::effector_blend);
    CHECK(midi_map_find(m, "Other", 9, cc, 74)->parameter == ParameterId::effector_master);
    CHECK(midi_map_find(m, "Kit", 1, MidiBindingKind::note, 74) == nullptr);  // kind must match
    CHECK(midi_map_find(m, "Kit", 1, cc, 71) == nullptr);
    // Unknown input (a plugin): bindings for any device match, ranked the same way.
    CHECK(midi_map_find(m, nullptr, 1, cc, 74)->parameter == ParameterId::vcf_l_freq);
    CHECK(midi_map_find(m, nullptr, 9, cc, 74)->parameter == ParameterId::vcf_l_res);
    CHECK(midi_map_find(m, nullptr, 2, cc, 74)->parameter == ParameterId::vcf_l_res);
    CHECK(midi_map_find(m, nullptr, 1, cc, 71) == nullptr);
    MidiMap other;
    CHECK(other.bind(ccBinding("Kit", 0, 20, ParameterId::vcf_l_freq)));
    CHECK(midi_map_find(other, nullptr, 5, cc, 20)->parameter == ParameterId::vcf_l_freq);
    CHECK(midi_map_find(other, "", 5, cc, 20) == nullptr);  // a known "no device" still filters
  }
  // midi_relative_delta: the three vendor dialects.
  {
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeBinOffset, 64), 0);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeBinOffset, 65), 1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeBinOffset, 63), -1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeBinOffset, 127), 63);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeTwosComplement, 1), 1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeTwosComplement, 127), -1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeTwosComplement, 64), -64);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeSignMagnitude, 1), 1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeSignMagnitude, 65), -1);
    CHECK_EQ(midi_relative_delta(MidiInputMode::relativeSignMagnitude, 0x40), 0);
    CHECK_EQ(midi_relative_delta(MidiInputMode::absolute, 99), 0);  // never called for absolute
  }
  // MidiRelativeDetector: a repeated in-range value means a relative encoder.
  {
    auto run = [](std::initializer_list<int> values) {
      MidiRelativeDetector d;
      MidiInputMode m = MidiInputMode::absolute;
      for (int v : values) {
        m = d.feed(v);
        if (m != MidiInputMode::absolute) break;
      }
      return m;
    };
    // MPK mini IV with KnobM = Rel (owner, 2026-10-04): 1 clockwise, 127 counter-clockwise.
    CHECK(run({1, 1}) == MidiInputMode::relativeTwosComplement);
    CHECK(run({127, 127, 127, 2, 1, 1}) == MidiInputMode::relativeTwosComplement);
    CHECK(run({126, 126}) == MidiInputMode::relativeTwosComplement);
    CHECK(run({65, 65}) == MidiInputMode::relativeBinOffset);
    CHECK(run({63, 63}) == MidiInputMode::relativeBinOffset);
    CHECK(run({1, 65, 65}) == MidiInputMode::relativeSignMagnitude);
    CHECK(run({65, 1, 1}) == MidiInputMode::relativeSignMagnitude);
    // Absolute knobs: a sweep, an end stop repeating 0 / 127, a turn back, never decide.
    CHECK(run({60, 61, 62, 63, 64, 65, 66}) == MidiInputMode::absolute);
    CHECK(run({124, 125, 126, 127, 127, 127}) == MidiInputMode::absolute);
    CHECK(run({3, 2, 1, 0, 0, 0}) == MidiInputMode::absolute);
    CHECK(run({40, 41, 40, 41}) == MidiInputMode::absolute);
    MidiRelativeDetector d;
    for (int i = 0; i < MidiRelativeDetector::kMaxSamples; ++i) (void)d.feed(i % 2 ? 30 : 31);
    CHECK(d.exhausted());
  }
  return test::finish("test_midi_map");
}
