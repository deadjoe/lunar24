// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/plugin_state_chunk.h — what a VST3 / AU plugin stores in the DAW project.
// A small header and tagged sections, so a later build can add a section (the MIDI map, say)
// and an older build skips the ones it does not know. All integers little-endian:
//
//   "LN24"  u32 format  u32 payloadBytes  { u32 tag  u32 bytes  data[bytes] }...
//
//   'STAT'  the machine-state record (the app's state file holds the same record)
//   'KEYS'  the six DRONE VOICES keys, one byte each (0 closed, 1 open); not part of the
//           machine state, but a project should reopen sounding the way it was saved
//
// The DAW may store more after the chunk (VST3 appends the bypass flag), so the reader takes
// the chunk's length from its header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <host/app_state_store.h>
#include <lunar24/core/state_serializer.h>

namespace lunar24::host {

inline constexpr std::uint32_t kPluginChunkFormat = 1;
inline constexpr std::size_t kPluginChunkHeaderBytes = 12;
inline constexpr std::size_t kPluginDroneKeys = 6;

// What a project chunk carries.
struct PluginProjectState {
  DeviceStateV1 machine;
  bool hasDroneKeys = false;
  bool droneKeys[kPluginDroneKeys] = {};
};

enum class PluginChunkRead : std::uint8_t {
  Ok,
  NotLunar24,   // no "LN24" header: not ours
  Truncated,    // shorter than its header or a section says
  NewerFormat,  // written by a build with an incompatible newer format
  NoState,      // no machine-state section
  BadState,     // the machine-state section was refused (see `state`)
};

struct PluginChunkResult {
  PluginChunkRead read = PluginChunkRead::NotLunar24;
  StateLoadOutcome state = StateLoadOutcome::NotAttempted;  // the machine-state section's verdict
  std::size_t bytes = 0;  // the chunk's whole length per its header (0 if the header is unusable)
};

namespace plugin_chunk_detail {
inline std::uint32_t tag(const char (&t)[5]) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(t[0])) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(t[1])) << 8 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(t[2])) << 16 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(t[3])) << 24;
}
inline void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
inline std::uint32_t getU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
inline void putSection(std::vector<std::uint8_t>& out, std::uint32_t t, const std::uint8_t* data,
                       std::size_t n) {
  putU32(out, t);
  putU32(out, static_cast<std::uint32_t>(n));
  out.insert(out.end(), data, data + n);
}
}  // namespace plugin_chunk_detail

// Append the chunk for `project` to `out`. False (and `out` unchanged) if the state cannot be
// encoded.
inline bool write_plugin_chunk(const PluginProjectState& project, std::vector<std::uint8_t>& out) {
  namespace d = plugin_chunk_detail;
  std::vector<std::uint8_t> machine(kAppStateWireBytes);
  if (!lunar24::core::encode_device_state(project.machine, machine.data(), machine.size())) return false;
  std::vector<std::uint8_t> payload;
  d::putSection(payload, d::tag("STAT"), machine.data(), machine.size());
  if (project.hasDroneKeys) {
    std::uint8_t keys[kPluginDroneKeys];
    for (std::size_t i = 0; i < kPluginDroneKeys; ++i) keys[i] = project.droneKeys[i] ? 1u : 0u;
    d::putSection(payload, d::tag("KEYS"), keys, kPluginDroneKeys);
  }
  const char magic[4] = {'L', 'N', '2', '4'};
  out.insert(out.end(), magic, magic + 4);
  d::putU32(out, kPluginChunkFormat);
  d::putU32(out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

// Read a chunk from `data` (`size` bytes, possibly followed by the DAW's own bytes). On Ok,
// `out` holds the validated machine state (through restore_saved_state) and the drone keys if
// the chunk had them; otherwise `out` is untouched.
inline PluginChunkResult read_plugin_chunk(const std::uint8_t* data, std::size_t size,
                                           PluginProjectState* out) {
  namespace d = plugin_chunk_detail;
  PluginChunkResult r;
  if (data == nullptr || size < kPluginChunkHeaderBytes || std::memcmp(data, "LN24", 4) != 0) return r;
  const std::uint32_t format = d::getU32(data + 4);
  const std::size_t payload = d::getU32(data + 8);
  if (payload > size - kPluginChunkHeaderBytes) {
    r.read = PluginChunkRead::Truncated;
    return r;
  }
  r.bytes = kPluginChunkHeaderBytes + payload;
  if (format > kPluginChunkFormat) {
    r.read = PluginChunkRead::NewerFormat;
    return r;
  }

  const std::uint8_t* machine = nullptr;
  std::size_t machineBytes = 0;
  const std::uint8_t* keys = nullptr;
  for (std::size_t at = kPluginChunkHeaderBytes; at < r.bytes;) {
    if (r.bytes - at < 8) {
      r.read = PluginChunkRead::Truncated;
      return r;
    }
    const std::uint32_t t = d::getU32(data + at);
    const std::size_t n = d::getU32(data + at + 4);
    at += 8;
    if (n > r.bytes - at) {
      r.read = PluginChunkRead::Truncated;
      return r;
    }
    if (t == d::tag("STAT")) {
      machine = data + at;
      machineBytes = n;
    } else if (t == d::tag("KEYS") && n == kPluginDroneKeys) {
      keys = data + at;
    }  // anything else: a section from a later build, skipped
    at += n;
  }
  if (machine == nullptr) {
    r.read = PluginChunkRead::NoState;
    return r;
  }
  PluginProjectState project;
  r.state = restore_saved_state(machine, machineBytes, &project.machine);
  if (r.state != StateLoadOutcome::Ok) {
    r.read = PluginChunkRead::BadState;
    return r;
  }
  if (keys != nullptr) {
    project.hasDroneKeys = true;
    for (std::size_t i = 0; i < kPluginDroneKeys; ++i) project.droneKeys[i] = keys[i] != 0;
  }
  *out = project;
  r.read = PluginChunkRead::Ok;
  return r;
}

}  // namespace lunar24::host
