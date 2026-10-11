// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/audio_log.h>
#include <host/panel_diagnostics.h>
#include <lunar24/core/state_default.h>
#include <fstream>
#include <memory>

using namespace lunar24;

std::string read(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main() {
  // A blocked disk writer cannot hold the producer lock or grow the queue.
  auto log = std::make_unique<host::AsyncAudioLog>();
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, release = false;
  std::string written;
  CHECK(log->start([&](std::string_view text) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_one();
    cv.wait(lock, [&] { return release; });
    written.append(text);
    return true;
  }));
  while (!log->submit("first")) std::this_thread::yield();
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  for (std::size_t i = 0; i < host::AsyncAudioLog::kCapacity; ++i) CHECK(log->submit("queued"));
  CHECK(!log->submit("must drop without waiting for disk"));
  CHECK(!log->submit(std::string(host::AsyncAudioLog::kRecordBytes, 'x')));
  { std::lock_guard<std::mutex> lock(mutex); release = true; }
  cv.notify_one();
  log->stop();
  CHECK(written.find("log records lost=2") != std::string::npos);
  CHECK(written.find("first") < written.find("queued"));
  CHECK(!log->submit("after shutdown"));

  // Rotation keeps three predecessors, whole records, and Unicode paths.
  const auto dir = std::filesystem::temp_directory_path() /
      std::filesystem::u8path("lunar-log-测试-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(dir);
  const auto path = dir / "audio.log";
  host::RotatingAudioLog file(path);
  CHECK(!file(std::string(host::RotatingAudioLog::kFileBytes + 1, 'x')));
  for (char c = 'a'; c <= 'e'; ++c) CHECK(file(std::string(host::RotatingAudioLog::kFileBytes - 10, c)));
  CHECK(read(path).front() == 'e');
  CHECK(read(dir / "audio.log.1").front() == 'd');
  CHECK(read(dir / "audio.log.2").front() == 'c');
  CHECK(read(dir / "audio.log.3").front() == 'b');
  CHECK(!std::filesystem::exists(dir / "audio.log.4"));
  // Reopening/upgrading app appends, without erasing the preceding session.
  CHECK(host::RotatingAudioLog(path)("new start"));
  CHECK(read(path).find("new start") != std::string::npos);
  // An unavailable path is a log failure, not an exception or a growing file.
  CHECK(!host::RotatingAudioLog(dir / "missing" / "audio.log")("cannot write"));
  std::filesystem::remove_all(dir);

  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  CHECK(engine->prepare(1, 48000, 256, 0, 2));
  CHECK(engine->postParameter(core::ParameterId::envelope_a_hold, 1));
  CHECK(engine->postDroneKey(0, true));
  CHECK(engine->postConnect(core::JackId::keyboard_gate_left_main_out, core::JackId::envelope_b_gate_in));
  const auto text = host::diagnosticSnapshot(*engine, "before-reset");
  CHECK(text.size() < host::AsyncAudioLog::kRecordBytes);
  CHECK(text.find("param envelope_a.hold=1") != std::string::npos);
  CHECK(text.find("drone_keys=100000") != std::string::npos);
  CHECK(text.find("cables=1") != std::string::npos);
  // The embedded wire state is complete, decodable, and still PRE-reset.
  const auto start = text.find("device_state_hex=") + std::strlen("device_state_hex=");
  const auto end = text.find('\n', start);
  std::vector<std::uint8_t> wire;
  for (auto i = start; i < end; i += 2) wire.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  core::DeviceStateV1 decoded;
  CHECK(core::decode_device_state(wire.data(), wire.size(), &decoded));
  CHECK(decoded.parameters[static_cast<unsigned>(core::ParameterId::envelope_a_hold)] == 1);
  CHECK(decoded.inputCable[static_cast<unsigned>(core::JackId::envelope_b_gate_in)] == 1);

  host::PanelDiagnostics panel;
  auto now = std::chrono::steady_clock::now();
  std::vector<std::string> records;
  auto emit = [&](const std::string& t) { records.push_back(t); };
  panel.poll(*engine, now, emit);
  CHECK_EQ(records.size(), 1u);
  CHECK(engine->postParameter(core::ParameterId::vco_a_tune, 0.1));
  CHECK(engine->postParameter(core::ParameterId::vco_a_tune, 0.2));
  panel.poll(*engine, now + std::chrono::milliseconds(100), emit);
  CHECK_EQ(records.size(), 1u);
  panel.poll(*engine, now + std::chrono::milliseconds(500), emit);
  CHECK_EQ(records.size(), 2u);
  CHECK(records.back().find("param vco_a.tune=0.2") != std::string::npos);
  panel.poll(*engine, now + std::chrono::seconds(1), emit);
  CHECK_EQ(records.size(), 2u);  // unchanged state emits nothing
  return test::finish("test_audio_log");
}
