// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// REC: the recorder writes valid 24-bit stereo WAVs (WET pair, DRY pair, or both), drops and
// counts frames instead of blocking when its ring is full, and the engine taps the same
// samples it sends to the device (DRY included on a 2-channel device).
#include "mini_test.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <host/standalone_audio_engine.h>
#include <host/wav_recorder.h>

namespace {
std::vector<unsigned char> readAll(const char* path) {
  std::vector<unsigned char> bytes;
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return bytes;
  unsigned char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  std::fclose(f);
  return bytes;
}
std::uint32_t u32(const std::vector<unsigned char>& b, std::size_t at) {
  return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) | (std::uint32_t(b[at + 2]) << 16) |
         (std::uint32_t(b[at + 3]) << 24);
}
std::uint16_t u16(const std::vector<unsigned char>& b, std::size_t at) {
  return static_cast<std::uint16_t>(std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8));
}
std::int32_t s24(const std::vector<unsigned char>& b, std::size_t at) {
  const std::int32_t v = std::int32_t(b[at]) | (std::int32_t(b[at + 1]) << 8) | (std::int32_t(b[at + 2]) << 16);
  return (v & 0x800000) ? v - 0x1000000 : v;
}
// Captures what the engine taps.
struct FakeTap final : lunar24::host::AudioTap {
  std::vector<float> frames;
  bool armed() const override { return true; }
  void push(const float* f, int n) override { frames.insert(frames.end(), f, f + 4 * n); }
};
}  // namespace

int main() {
  using lunar24::host::WavRecorder;
  // Both files: WET (0.5, -0.5) and DRY (0.25, 1.5 -> clipped to full scale).
  {
    WavRecorder rec;
    std::vector<float> frames;
    for (int i = 0; i < 1000; ++i) frames.insert(frames.end(), {0.5f, -0.5f, 0.25f, 1.5f});
    rec.push(frames.data(), 10);  // not recording yet: ignored
    CHECK(!rec.recording());
    CHECK(rec.start(std::fopen("rec_wet.wav", "wb"), std::fopen("rec_dry.wav", "wb"), 48000));
    CHECK(rec.recording());
    CHECK(!rec.start(nullptr, std::fopen("rec_extra.wav", "wb"), 48000));  // already recording
    rec.push(frames.data(), 1000);
    const WavRecorder::Result r = rec.stop();
    CHECK(r.wasRecording);
    CHECK_EQ(r.droppedFrames, 0u);
    CHECK(r.seconds > 0.0208 && r.seconds < 0.0209);  // 1000 / 48000
    CHECK(!rec.recording());
    const auto wet = readAll("rec_wet.wav");
    const auto dry = readAll("rec_dry.wav");
    CHECK_EQ(wet.size(), 44u + 1000u * 6u);
    CHECK_EQ(dry.size(), 44u + 1000u * 6u);
    if (wet.size() == 44u + 6000u && dry.size() == 44u + 6000u) {
      CHECK(std::memcmp(wet.data(), "RIFF", 4) == 0 && std::memcmp(wet.data() + 8, "WAVEfmt ", 8) == 0);
      CHECK_EQ(u32(wet, 4), 36u + 6000u);
      CHECK_EQ(u16(wet, 20), 1u);       // PCM
      CHECK_EQ(u16(wet, 22), 2u);       // stereo
      CHECK_EQ(u32(wet, 24), 48000u);
      CHECK_EQ(u16(wet, 34), 24u);
      CHECK(std::memcmp(wet.data() + 36, "data", 4) == 0);
      CHECK_EQ(u32(wet, 40), 6000u);
      CHECK_EQ(s24(wet, 44), 4194304);    // 0.5 * 8388607, rounded
      CHECK_EQ(s24(wet, 47), -4194304);
      CHECK_EQ(s24(dry, 44), 2097152);    // DRY A = 0.25
      CHECK_EQ(s24(dry, 47), 8388607);    // DRY B 1.5: clipped, not wrapped
      CHECK_EQ(s24(dry, 44 + 999 * 6), 2097152);
    }
    CHECK(!rec.stop().wasRecording);  // stopping twice is harmless
  }
  // DRY only; nothing to write is refused.
  {
    WavRecorder rec;
    CHECK(!rec.start(nullptr, nullptr, 48000));
    CHECK(rec.start(nullptr, std::fopen("rec_dry_only.wav", "wb"), 44100));
    const float frame[4] = {0.f, 0.f, -1.f, 1.f};
    for (int i = 0; i < 10; ++i) rec.push(frame, 1);
    (void)rec.stop();
    const auto dry = readAll("rec_dry_only.wav");
    CHECK_EQ(dry.size(), 44u + 60u);
    if (dry.size() == 104u) {
      CHECK_EQ(u32(dry, 24), 44100u);
      CHECK_EQ(s24(dry, 44), -8388607);
    }
  }
  // A full ring drops (and counts) the frames that do not fit; the audio side never waits.
  {
    WavRecorder rec(64);
    CHECK(rec.start(std::fopen("rec_small.wav", "wb"), nullptr, 48000));
    std::vector<float> frames(4 * 100, 0.1f);
    rec.push(frames.data(), 100);
    const WavRecorder::Result r = rec.stop();
    CHECK_EQ(r.droppedFrames, 36u);
    CHECK_EQ(readAll("rec_small.wav").size(), 44u + 64u * 6u);
  }
  // The engine taps every frame it renders, the DRY pair too on a 2-output device, and the
  // tapped WET equals what reached the device.
  {
    auto engine = std::make_unique<lunar24::host::StandaloneAudioEngine>();
    CHECK(engine->prepare(1, 48000.0, 64, 0, 2));
    // envelope A hold opens VCA A, so DRY A (after the VCA) sounds without a note.
    CHECK(engine->postParameter(lunar24::core::ParameterId::envelope_a_hold, 1.0));
    FakeTap tap;
    engine->setAudioTap(&tap);
    double l[64]{}, r[64]{};
    double* out[] = {l, r};
    for (int i = 0; i < 20; ++i) engine->processBlock(nullptr, out, 0, 2, 64);
    CHECK_EQ(tap.frames.size(), 20u * 64u * 4u);
    bool wetMatches = true, dryHeard = false;
    for (int f = 0; f < 64; ++f) {
      const float* t = &tap.frames[(19 * 64 + f) * 4];
      wetMatches = wetMatches && t[0] == static_cast<float>(l[f]) && t[1] == static_cast<float>(r[f]);
      dryHeard = dryHeard || t[2] != 0.f || t[3] != 0.f;
    }
    CHECK(wetMatches);
    CHECK(dryHeard);  // VCO A through its held VCA, though the device has only 2 outputs
    engine->setAudioTap(nullptr);
    engine->processBlock(nullptr, out, 0, 2, 64);
    CHECK_EQ(tap.frames.size(), 20u * 64u * 4u);
  }
  for (const char* p : {"rec_wet.wav", "rec_dry.wav", "rec_extra.wav", "rec_dry_only.wav", "rec_small.wav"})
    std::remove(p);
  return test::finish("test_wav_recorder");
}
