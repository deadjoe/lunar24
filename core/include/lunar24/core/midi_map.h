// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// midi_map.h — user MIDI bindings: (device, channel, message) -> panel target.
//
// A binding is part of the user's CONTROLLER RIG, not of the machine's sound state:
// it lives in its own file (the host layer owns the IO), never inside DeviceStateV1,
// so loading a preset or a saved panel can never re-route the user's controller.
//
// The model is fixed-capacity, no-heap, framework-free. The wire format is a
// fixed-record binary with an explicit version; decode is all-or-nothing (a
// malformed file yields zero bindings, never a partial map).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// Which hardware message a binding listens to.
enum class MidiBindingKind : std::uint8_t { cc = 0, note = 1 };

// What the bound message drives.
enum class MidiTargetKind : std::uint8_t { parameter = 0, action = 1 };

// How a continuous target reads incoming values. The absolute/relative decodings are
// per-binding because different controllers ship different encodings.
enum class MidiInputMode : std::uint8_t {
  absolute = 0,               // 0..127; pickup/takeover is the consumer's job
  relativeBinOffset = 1,      // 64 = no turn, 65 = +1, 63 = -1
  relativeTwosComplement = 2, // 1 = +1, 127 = -1
  relativeSignMagnitude = 3,  // bit 6 = direction (1 = down), low 6 bits = amount
};

// Non-parameter targets a binding can drive. Fixed explicit values, stable on disk.
enum class MidiAction : std::uint8_t {
  drone_key_1 = 0,
  drone_key_2 = 1,
  drone_key_3 = 2,
  drone_key_4 = 3,
  drone_key_5 = 4,
  drone_key_6 = 5,
  cartridge_next = 6,
  cartridge_prev = 7,
  preset_load_a = 8,
  preset_load_b = 9,
  preset_load_c = 10,
  preset_load_d = 11,
  master_mute = 12,
};
inline constexpr std::uint8_t kMidiActionCount = 13;

inline constexpr std::size_t kMidiBindingDeviceCapacity = 64;  // UTF-8, NUL-terminated
inline constexpr std::uint32_t kMidiMapCapacity = 128;

// The hardware message a binding listens to. An empty device name means "any input
// device"; channel 0 means "any channel".
struct MidiBindingKey {
  char device[kMidiBindingDeviceCapacity] = {};
  std::uint8_t channel = 0;  // 0 = any, else 1..16
  MidiBindingKind kind = MidiBindingKind::cc;
  std::uint8_t number = 0;   // CC / note number 0..127

  bool equals(const MidiBindingKey& o) const {
    return channel == o.channel && kind == o.kind && number == o.number &&
           std::strncmp(device, o.device, kMidiBindingDeviceCapacity) == 0;
  }
};

struct MidiBinding {
  MidiBindingKey key;
  MidiTargetKind targetKind = MidiTargetKind::parameter;
  MidiInputMode mode = MidiInputMode::absolute;        // continuous targets
  ParameterId parameter = ParameterId{0};              // targetKind == parameter
  MidiAction action = MidiAction::drone_key_1;         // targetKind == action
};

// Field-range validation (wire decode and UI entry both use it). The device name must
// be NUL-terminated within its fixed field.
inline bool midi_binding_key_valid(const MidiBindingKey& k) {
  if (k.channel > 16 || k.number > 127) return false;
  if (k.kind != MidiBindingKind::cc && k.kind != MidiBindingKind::note) return false;
  return std::memchr(k.device, '\0', kMidiBindingDeviceCapacity) != nullptr;
}

inline bool midi_binding_valid(const MidiBinding& b) {
  if (!midi_binding_key_valid(b.key)) return false;
  if (b.targetKind == MidiTargetKind::parameter)
    return static_cast<std::uint32_t>(b.parameter) < kParameterCount &&
           b.mode <= MidiInputMode::relativeSignMagnitude;
  if (b.targetKind == MidiTargetKind::action)
    return static_cast<std::uint8_t>(b.action) < kMidiActionCount &&
           b.mode == MidiInputMode::absolute;  // mode is meaningless for an action: keep files canonical
  return false;
}

// The ordered binding list. Order is insertion order (the UI lists it as-is); unbind
// shifts left so the list never reorders under the user's eyes.
class MidiMap {
 public:
  std::uint32_t count() const { return count_; }
  const MidiBinding& at(std::uint32_t i) const { return bindings_[i]; }

  int find(const MidiBindingKey& key) const {
    for (std::uint32_t i = 0; i < count_; ++i)
      if (bindings_[i].key.equals(key)) return static_cast<int>(i);
    return -1;
  }

  // Insert or REPLACE: a hardware message already bound takes the new target (the
  // list row keeps its position). Returns false when the binding is invalid or the
  // map is full and the key is new.
  bool bind(const MidiBinding& b) {
    if (!midi_binding_valid(b)) return false;
    const int existing = find(b.key);
    if (existing >= 0) {
      bindings_[existing] = b;
      return true;
    }
    if (count_ >= kMidiMapCapacity) return false;
    bindings_[count_++] = b;
    return true;
  }

  bool unbind(const MidiBindingKey& key) {
    const int i = find(key);
    if (i < 0) return false;
    for (std::uint32_t j = static_cast<std::uint32_t>(i) + 1; j < count_; ++j)
      bindings_[j - 1] = bindings_[j];
    --count_;
    return true;
  }

  void clear() { count_ = 0; }

 private:
  MidiBinding bindings_[kMidiMapCapacity]{};
  std::uint32_t count_ = 0;
};

// ---- wire format --------------------------------------------------------------
//
//   offset 0:  magic "L24M" (4 bytes)
//   offset 4:  version u32 LE (1)
//   offset 8:  binding count u32 LE
//   offset 12: count records, each 80 bytes:
//     device[64] | channel u8 | kind u8 | number u8 | mode u8 | targetKind u8 |
//     action u8 | reserved u16 | parameter u32 LE | reserved u32
//
// Everything multi-byte is little-endian; reserved bytes are zero on write and must
// be zero on read (a file from a format we do not know is rejected, not guessed at).

inline constexpr std::uint32_t kMidiMapWireVersion = 1;
inline constexpr std::size_t kMidiMapWireHeaderBytes = 12;
inline constexpr std::size_t kMidiBindingWireBytes = 80;
inline constexpr char kMidiMapWireMagic[4] = {'L', '2', '4', 'M'};

inline constexpr std::size_t midi_map_wire_bytes(std::uint32_t count) {
  return kMidiMapWireHeaderBytes + static_cast<std::size_t>(count) * kMidiBindingWireBytes;
}

inline void put_u32le(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xffu);
  p[1] = static_cast<std::uint8_t>((v >> 8u) & 0xffu);
  p[2] = static_cast<std::uint8_t>((v >> 16u) & 0xffu);
  p[3] = static_cast<std::uint8_t>((v >> 24u) & 0xffu);
}

inline std::uint32_t get_u32le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
         (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

// Encode the whole map. Returns the byte count, or 0 when the output buffer is too
// small (nothing is written then: the size check comes first).
inline std::size_t midi_map_encode(const MidiMap& map, std::uint8_t* out, std::size_t capacity) {
  const std::size_t need = midi_map_wire_bytes(map.count());
  if (capacity < need) return 0;
  std::memcpy(out, kMidiMapWireMagic, 4);
  put_u32le(out + 4, kMidiMapWireVersion);
  put_u32le(out + 8, map.count());
  for (std::uint32_t i = 0; i < map.count(); ++i) {
    const MidiBinding& b = map.at(i);
    std::uint8_t* r = out + kMidiMapWireHeaderBytes + static_cast<std::size_t>(i) * kMidiBindingWireBytes;
    std::memset(r, 0, kMidiBindingWireBytes);
    std::memcpy(r, b.key.device, kMidiBindingDeviceCapacity);
    r[64] = b.key.channel;
    r[65] = static_cast<std::uint8_t>(b.key.kind);
    r[66] = b.key.number;
    r[67] = static_cast<std::uint8_t>(b.mode);
    r[68] = static_cast<std::uint8_t>(b.targetKind);
    r[69] = static_cast<std::uint8_t>(b.action);
    put_u32le(r + 72, static_cast<std::uint32_t>(b.parameter));
  }
  return need;
}

// Decode into `out`, all-or-nothing: returns false and writes NOTHING unless the
// header, the exact length and every record's fields all validate.
inline bool midi_map_decode(const std::uint8_t* in, std::size_t n, MidiMap* out) {
  if (in == nullptr || out == nullptr) return false;
  if (n < kMidiMapWireHeaderBytes) return false;
  if (std::memcmp(in, kMidiMapWireMagic, 4) != 0) return false;
  if (get_u32le(in + 4) != kMidiMapWireVersion) return false;
  const std::uint32_t count = get_u32le(in + 8);
  if (count > kMidiMapCapacity) return false;
  if (n != midi_map_wire_bytes(count)) return false;  // truncated OR grown
  MidiBinding parsed[kMidiMapCapacity];
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* r = in + kMidiMapWireHeaderBytes + static_cast<std::size_t>(i) * kMidiBindingWireBytes;
    MidiBinding& b = parsed[i];
    b = MidiBinding{};
    std::memcpy(b.key.device, r, kMidiBindingDeviceCapacity);
    b.key.channel = r[64];
    b.key.kind = static_cast<MidiBindingKind>(r[65]);
    b.key.number = r[66];
    b.mode = static_cast<MidiInputMode>(r[67]);
    b.targetKind = static_cast<MidiTargetKind>(r[68]);
    b.action = static_cast<MidiAction>(r[69]);
    b.parameter = static_cast<ParameterId>(get_u32le(r + 72));
    if (r[70] != 0 || r[71] != 0 || get_u32le(r + 76) != 0) return false;  // reserved
    if (!midi_binding_valid(b)) return false;
  }
  out->clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    // Duplicate keys in one file collapse to the last one (same rule as bind()).
    if (!out->bind(parsed[i])) return false;
  }
  return true;
}

}  // namespace lunar24::core
