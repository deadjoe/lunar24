// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The plugin's DAW-project chunk: a saved project reopens the same machine and drone keys, the
// DAW's own trailing bytes are left alone, a later build's extra sections are skipped, and
// anything damaged or foreign is refused without touching the result.
#include "mini_test.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <host/plugin_state_chunk.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>
#include <lunar24/core/state_serializer.h>

namespace {
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::host::PluginChunkRead;
using lunar24::host::PluginProjectState;
using lunar24::host::StateLoadOutcome;
using lunar24::host::read_plugin_chunk;
using lunar24::host::write_plugin_chunk;

std::vector<std::uint8_t> encodeMachine(const DeviceStateV1& st) {
  std::vector<std::uint8_t> b(lunar24::host::kAppStateWireBytes);
  CHECK(lunar24::core::encode_device_state(st, b.data(), b.size()));
  return b;
}

PluginProjectState sampleProject() {
  PluginProjectState p;
  p.machine = lunar24::core::make_default_device_state(1);
  CHECK(lunar24::core::state_set_param(p.machine, ParameterId::effector_master, 0.37));
  CHECK(lunar24::core::state_set_param(p.machine, ParameterId::vcf_l_freq, 0.61));
  p.hasDroneKeys = true;
  p.droneKeys[0] = true;
  p.droneKeys[3] = true;
  return p;
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// A chunk built by hand: header + the given sections.
std::vector<std::uint8_t> chunkOf(std::uint32_t format,
                                  const std::vector<std::pair<const char*, std::vector<std::uint8_t>>>& sections) {
  std::vector<std::uint8_t> payload;
  for (const auto& s : sections) {
    for (int i = 0; i < 4; ++i) payload.push_back(static_cast<std::uint8_t>(s.first[i]));
    putU32(payload, static_cast<std::uint32_t>(s.second.size()));
    payload.insert(payload.end(), s.second.begin(), s.second.end());
  }
  std::vector<std::uint8_t> c = {'L', 'N', '2', '4'};
  putU32(c, format);
  putU32(c, static_cast<std::uint32_t>(payload.size()));
  c.insert(c.end(), payload.begin(), payload.end());
  return c;
}
}  // namespace

int main() {
  const PluginProjectState saved = sampleProject();

  // Round trip, with the DAW's bypass flag after the chunk.
  {
    std::vector<std::uint8_t> c;
    CHECK(write_plugin_chunk(saved, c));
    const std::size_t chunkBytes = c.size();
    c.insert(c.end(), {1, 0, 0, 0});
    PluginProjectState back;
    const auto r = read_plugin_chunk(c.data(), c.size(), &back);
    CHECK(r.read == PluginChunkRead::Ok);
    CHECK(r.state == StateLoadOutcome::Ok);
    CHECK_EQ(r.bytes, chunkBytes);
    CHECK(encodeMachine(back.machine) == encodeMachine(saved.machine));
    CHECK(back.hasDroneKeys);
    for (int i = 0; i < 6; ++i) CHECK_EQ(back.droneKeys[i], saved.droneKeys[i]);
  }
  // Without drone keys (not written) the reader reports none.
  {
    PluginProjectState noKeys = saved;
    noKeys.hasDroneKeys = false;
    std::vector<std::uint8_t> c;
    CHECK(write_plugin_chunk(noKeys, c));
    PluginProjectState back;
    CHECK(read_plugin_chunk(c.data(), c.size(), &back).read == PluginChunkRead::Ok);
    CHECK(!back.hasDroneKeys);
  }
  // A later build's unknown section, before and after ours, is skipped.
  {
    const auto c = chunkOf(1, {{"MIDI", {9, 9, 9}}, {"STAT", encodeMachine(saved.machine)}, {"XTRA", {}}});
    PluginProjectState back;
    const auto r = read_plugin_chunk(c.data(), c.size(), &back);
    CHECK(r.read == PluginChunkRead::Ok);
    CHECK_EQ(r.bytes, c.size());
    CHECK(encodeMachine(back.machine) == encodeMachine(saved.machine));
  }
  // Refusals leave the result untouched.
  const auto untouched = [](const std::vector<std::uint8_t>& c, PluginChunkRead expected) {
    PluginProjectState back;
    back.machine.parameters[0] = 123.0;  // a marker
    const auto r = read_plugin_chunk(c.data(), c.size(), &back);
    CHECK(r.read == expected);
    CHECK(back.machine.parameters[0] == 123.0);
    CHECK(!back.hasDroneKeys);
    return r;
  };
  std::vector<std::uint8_t> good;
  CHECK(write_plugin_chunk(saved, good));
  {
    std::vector<std::uint8_t> foreign = good;
    foreign[0] = 'X';
    untouched(foreign, PluginChunkRead::NotLunar24);
    untouched({}, PluginChunkRead::NotLunar24);
  }
  // Every truncation of a good chunk is refused, never read past its end (ASan checks).
  for (std::size_t n = 0; n < good.size(); ++n) {
    PluginProjectState back;
    const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK(read_plugin_chunk(cut.data(), cut.size(), &back).read != PluginChunkRead::Ok);
  }
  {
    auto c = chunkOf(1, {{"STAT", encodeMachine(saved.machine)}});
    c[12 + 4] = 0xFF;  // the section claims more bytes than the chunk holds
    c[12 + 5] = 0xFF;
    untouched(c, PluginChunkRead::Truncated);
  }
  untouched(chunkOf(2, {{"STAT", encodeMachine(saved.machine)}}), PluginChunkRead::NewerFormat);
  untouched(chunkOf(1, {{"KEYS", {1, 1, 1, 1, 1, 1}}}), PluginChunkRead::NoState);
  {
    const auto r = untouched(chunkOf(1, {{"STAT", {1, 2, 3}}}), PluginChunkRead::BadState);
    CHECK(r.state == StateLoadOutcome::LengthMismatch);
  }
  {
    DeviceStateV1 bad = saved.machine;
    bad.parameters[static_cast<std::size_t>(ParameterId::effector_master)] = std::nan("");
    const auto r = untouched(chunkOf(1, {{"STAT", encodeMachine(bad)}}), PluginChunkRead::BadState);
    CHECK(r.state == StateLoadOutcome::InvalidState);
  }
  // A record of the size saved before the patch bank grew is recognised and upgraded before it is
  // judged (the upgrade itself is covered by test_app_state_store).
  {
    const auto c = chunkOf(1, {{"STAT", std::vector<std::uint8_t>(lunar24::core::legacy_patch67_wire_bytes(), 0u)}});
    PluginProjectState back;
    CHECK(read_plugin_chunk(c.data(), c.size(), &back).state != StateLoadOutcome::LengthMismatch);
  }
  return test::finish("plugin_state_chunk");
}
