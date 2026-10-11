// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/rt_log.h — diagnostics from the audio thread, and the output level watch.
//
// The audio thread may not lock, allocate or touch a file, so it never calls the Logger. It
// pushes small fixed-size events into RtLog (a lock-free single-producer ring) and the UI thread
// drains them into the log with the time they happened. A full ring drops events and counts them.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <host/diag_log.h>

namespace lunar24::host::diag {

enum class RtKind : std::uint8_t {
  midi_note_on,       // a channel(0-15), b note, c velocity, d side (0 left, 1 right)
  midi_note_off,      // a channel, b note, d 0 released / 1 held on by the sustain pedal
  midi_note_off_free, // a channel, b note: a release for a note that is not playing
  midi_binding,       // a channel, b note, c velocity, d binding row: a pad bound to an action / knob
  midi_binding_cc,    // a channel, b CC number, c value, d binding row (trace: knobs send many)
  midi_sustain,       // a channel, c 1 down / 0 up, d notes the pedal-up released
  midi_cc,            // a channel, b controller, c value (trace: knobs send many)
  midi_channel_mode,  // a channel, b controller (120-127: all sound / notes off ...), c value
  midi_filtered,      // a channel, b status: ignored by the MIDI channel setting
  midi_input_reset,   // the input was switched or closed: d notes released, c 1 = all-gates-off
  daw_reset,          // a DAW reset (transport stop / bypass): all notes off
  swap_reset,         // the machine was swapped (RESET, preset, project): MIDI notes forgotten
  event_rejected,     // the engine's event queue was full: a ControlEventKind
};

struct RtEvent {
  std::int64_t when = 0;  // steady_clock ticks
  RtKind kind{};
  std::uint8_t a = 0, b = 0, c = 0, e = 0;
  std::int16_t d = 0;
};

class RtLog {
 public:
  static constexpr std::uint32_t kCapacity = 2048;
  // Audio thread only.
  bool push(RtKind kind, unsigned a = 0, unsigned b = 0, unsigned c = 0, int d = 0, unsigned e = 0) noexcept {
    const auto h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= kCapacity) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    RtEvent& ev = slots_[h % kCapacity];
    ev.when = std::chrono::steady_clock::now().time_since_epoch().count();
    ev.kind = kind;
    ev.a = static_cast<std::uint8_t>(a);
    ev.b = static_cast<std::uint8_t>(b);
    ev.c = static_cast<std::uint8_t>(c);
    ev.e = static_cast<std::uint8_t>(e);
    ev.d = static_cast<std::int16_t>(std::clamp(d, -32768, 32767));
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // UI thread only.
  template <class F>
  void drain(F f) {
    auto t = tail_.load(std::memory_order_relaxed);
    const auto end = head_.load(std::memory_order_acquire);
    for (; t != end; ++t) {
      const RtEvent ev = slots_[t % kCapacity];
      tail_.store(t + 1, std::memory_order_release);
      f(ev);
    }
  }
  std::uint32_t takeDropped() noexcept { return dropped_.exchange(0, std::memory_order_relaxed); }

 private:
  std::array<RtEvent, kCapacity> slots_{};
  std::atomic<std::uint32_t> head_{0}, tail_{0}, dropped_{0};
};

inline const char* midiNoteName(unsigned note, char* buf, std::size_t size) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  std::snprintf(buf, size, "%s%d", names[note % 12], static_cast<int>((note & 127u) / 12) - 1);  // 60 = C4
  return buf;
}

// The log line (category midi, or engine for a rejected event) and its level.
inline Level rtLevel(RtKind k) {
  switch (k) {
    case RtKind::midi_cc:
    case RtKind::midi_binding_cc:
    case RtKind::midi_filtered: return Level::trace;
    case RtKind::midi_input_reset:
    case RtKind::daw_reset:
    case RtKind::swap_reset: return Level::info;
    case RtKind::event_rejected: return Level::warn;
    default: return Level::debug;
  }
}
inline Cat rtCat(RtKind k) { return k == RtKind::event_rejected ? Cat::engine : Cat::midi; }
inline std::string rtText(const RtEvent& ev) {
  char line[160], n[16];
  const unsigned ch = ev.a + 1u;  // MIDI channels are 1-16 to people
  const unsigned b = ev.b, c = ev.c;
  const int d = ev.d;
  switch (ev.kind) {
    case RtKind::midi_note_on:
      std::snprintf(line, sizeof line, "note-on ch=%u note=%u(%s) vel=%u side=%s", ch, b,
                    midiNoteName(b, n, sizeof n), c, d ? "R" : "L");
      break;
    case RtKind::midi_note_off:
      std::snprintf(line, sizeof line, "note-off ch=%u note=%u(%s)%s", ch, b, midiNoteName(b, n, sizeof n),
                    d ? " held-by-sustain" : "");
      break;
    case RtKind::midi_note_off_free:
      std::snprintf(line, sizeof line, "note-off ch=%u note=%u(%s) not-playing (pad release, or forgotten by a reset)",
                    ch, b, midiNoteName(b, n, sizeof n));
      break;
    case RtKind::midi_binding:
      std::snprintf(line, sizeof line, "bound pad ch=%u note=%u(%s) vel=%u row=%d", ch, b,
                    midiNoteName(b, n, sizeof n), c, d);
      break;
    case RtKind::midi_binding_cc:
      std::snprintf(line, sizeof line, "bound cc ch=%u cc=%u value=%u row=%d", ch, b, c, d);
      break;
    case RtKind::midi_sustain:
      if (c) std::snprintf(line, sizeof line, "sustain down ch=%u", ch);
      else std::snprintf(line, sizeof line, "sustain up ch=%u released=%d", ch, d);
      break;
    case RtKind::midi_cc:
      std::snprintf(line, sizeof line, "cc ch=%u cc=%u value=%u", ch, b, c);
      break;
    case RtKind::midi_channel_mode:
      std::snprintf(line, sizeof line, "channel-mode ch=%u cc=%u value=%u (not acted on)", ch, b, c);
      break;
    case RtKind::midi_filtered:
      std::snprintf(line, sizeof line, "filtered ch=%u status=0x%02X (MIDI channel setting)", ch, b);
      break;
    case RtKind::midi_input_reset:
      std::snprintf(line, sizeof line, "input reset: released %d held notes%s", d,
                    c ? ", queue full so all gates off" : "");
      break;
    case RtKind::daw_reset:
      std::snprintf(line, sizeof line, "DAW reset: all notes and the sustain pedal released");
      break;
    case RtKind::swap_reset:
      std::snprintf(line, sizeof line, "machine swapped: MIDI note and pedal ledgers cleared");
      break;
    case RtKind::event_rejected:
      std::snprintf(line, sizeof line, "event queue full: dropped a control event kind=%u from MIDI", unsigned(ev.a));
      break;
  }
  return line;
}

// Which MIDI notes are down, and which only the sustain pedal holds, as the drained events tell
// it (UI thread). For snapshots: "the DSP holds a note" + "no MIDI note is down" = a stuck note.
class MidiHeldMirror {
 public:
  void apply(const RtEvent& e) {
    const unsigned ch = e.a & 15u, note = e.b & 127u;
    switch (e.kind) {
      case RtKind::midi_note_on: state_[ch][note] = 1; break;
      case RtKind::midi_note_off: state_[ch][note] = e.d ? 2 : 0; break;
      case RtKind::midi_sustain:
        if (!e.c) for (auto& s : state_[ch]) if (s == 2) s = 0;
        break;
      case RtKind::midi_input_reset:
      case RtKind::daw_reset:
      case RtKind::swap_reset:
        for (auto& c : state_) for (auto& s : c) s = 0;
        break;
      default: break;
    }
  }
  void lost() { incomplete_ = true; }
  std::string describe() const {
    std::string down, pedal;
    char n[16], item[32];
    for (unsigned ch = 0; ch < 16; ++ch)
      for (unsigned note = 0; note < 128; ++note) {
        if (!state_[ch][note]) continue;
        std::snprintf(item, sizeof item, " ch%u:%s", ch + 1, midiNoteName(note, n, sizeof n));
        (state_[ch][note] == 1 ? down : pedal) += item;
      }
    return "midi notes down:" + (down.empty() ? std::string(" none") : down) +
           "; held by sustain:" + (pedal.empty() ? std::string(" none") : pedal) +
           (incomplete_ ? " (may be incomplete: audio-thread events were dropped)" : "");
  }

 private:
  std::uint8_t state_[16][128] = {};  // 0 up, 1 down, 2 released but held by the pedal
  bool incomplete_ = false;
};

// Drain the audio-thread events into the log, in the order and at the time they happened.
inline void drainRtLog(RtLog& rt, Logger& log, std::uint16_t instance, MidiHeldMirror* held = nullptr) {
  rt.drain([&](const RtEvent& ev) {
    if (held) held->apply(ev);
    if (!log.enabled(rtCat(ev.kind), rtLevel(ev.kind))) return;
    log.write(rtCat(ev.kind), rtLevel(ev.kind), rtText(ev), instance,
              Logger::Clock::time_point(Logger::Clock::duration(ev.when)));
  });
  if (const auto lost = rt.takeDropped()) {
    if (held) held->lost();
    log.writef(Cat::log, Level::warn, instance, "%u audio-thread events dropped (ring full)", lost);
  }
}

// ---- levels ----------------------------------------------------------------------------------
// The loudest sample since the UI last looked. The audio thread raises it, the UI takes it.
class PeakMeter {
 public:
  void raise(float peak) noexcept {
    if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);
  }
  float take() noexcept { return peak_.exchange(0.f, std::memory_order_relaxed); }

 private:
  std::atomic<float> peak_{0.f};
};

inline std::string dbfs(float peak) {
  if (!(peak > 1e-7f)) return "-inf";
  char b[16];
  std::snprintf(b, sizeof b, "%.1f", 20.0 * std::log10(static_cast<double>(peak)));
  return b;
}

// When the output starts sounding and when it falls silent, plus a level summary while it
// sounds. It also times how long nothing has entered the effector: when the output falls
// silent, or in a snapshot, "effector input silent for 20 s" means the last 20 s were only the
// effector's tail (reverb, delay feedback). A gating LFO cuts the input every cycle, so this
// is reported as a duration, never as on / off events. UI thread; fed the peaks taken since
// the previous call (every idle tick).
class LevelWatch {
 public:
  static constexpr float kSoundOn = 0.0003f;   // -70 dBFS
  static constexpr float kSoundOff = 0.0001f;  // -80 dBFS for kSilentFor
  static constexpr auto kSilentFor = std::chrono::milliseconds(500);
  static constexpr auto kSummaryEvery = std::chrono::seconds(10);
  using Clock = std::chrono::steady_clock;

  // wet / dry: the outputs; fxIn: the signal entering the effector; in: the audio input.
  template <class Emit>  // Emit(Level, const std::string&)
  void update(Clock::time_point now, float wet, float dry, float fxIn, float in, bool running, Emit emit) {
    if (!running) {
      if (sounding_) emit(Level::info, "output stopped (no audio stream)");
      sounding_ = false;
      quietSince_ = {};
      fxLoudAt_ = wetLoudAt_ = {};
      return;
    }
    if (fxLoudAt_ == Clock::time_point{}) fxLoudAt_ = wetLoudAt_ = now;  // stream start
    if (fxIn >= kSoundOff) fxLoudAt_ = now;
    if (wet >= kSoundOff) wetLoudAt_ = now;
    const float out = std::max(wet, dry);
    maxWet_ = std::max(maxWet_, wet);
    maxDry_ = std::max(maxDry_, dry);
    maxFx_ = std::max(maxFx_, fxIn);
    maxIn_ = std::max(maxIn_, in);
    if (!sounding_ && out > kSoundOn) {
      sounding_ = true;
      quietSince_ = {};
      summaryAt_ = now + kSummaryEvery;  // the summary covers this sounding stretch
      maxWet_ = wet;
      maxDry_ = dry;
      maxFx_ = fxIn;
      maxIn_ = in;
      emit(Level::info, "output sounding: peak " + dbfs(out) + " dBFS");
    } else if (sounding_) {
      if (out >= kSoundOff) {
        quietSince_ = {};
      } else if (quietSince_ == Clock::time_point{}) {
        quietSince_ = now;
      } else if (now - quietSince_ >= kSilentFor) {
        sounding_ = false;
        // How long WET rang on after nothing entered the effector any more.
        // Only when WET is what fell silent just now (not a DRY-only stretch after it).
        const bool wetEnded = quietSince_ - wetLoudAt_ < std::chrono::seconds(1);
        const double tail = wetEnded ? std::chrono::duration<double>(wetLoudAt_ - fxLoudAt_).count() : 0.0;
        emit(Level::info, tail >= 1.0 ? "output silent; the last " + seconds_(tail) +
                                            " of WET were only the effector tail (nothing entered it)"
                                      : std::string("output silent (below -80 dBFS for 0.5 s)"));
      }
    }
    if (now >= summaryAt_) {
      if (sounding_)
        emit(Level::debug, "levels (10 s max dBFS): wet " + dbfs(maxWet_) + ", dry " + dbfs(maxDry_) +
                               ", effector in " + dbfs(maxFx_) + ", audio input " + dbfs(maxIn_));
      maxWet_ = maxDry_ = maxFx_ = maxIn_ = 0.f;
      summaryAt_ = now + kSummaryEvery;
    }
  }
  bool sounding() const { return sounding_; }
  // Seconds since anything entered the effector, as of `now` (0 while something does).
  double fxSilentSeconds(Clock::time_point now) const {
    if (fxLoudAt_ == Clock::time_point{} || now <= fxLoudAt_) return 0.0;
    return std::chrono::duration<double>(now - fxLoudAt_).count();
  }
  // For a snapshot: "output sounding; effector input silent for 12.3 s (what sounds on WET is
  // only the effector tail)".
  std::string describe(Clock::time_point now) const {
    const double fx = fxSilentSeconds(now);
    std::string s = std::string("output ") + (sounding_ ? "sounding" : "silent") + "; effector input ";
    if (fx < 0.5) return s + "sounding";
    s += "silent for " + seconds_(fx);
    if (sounding_ && now - wetLoudAt_ < kSilentFor) s += " (what sounds on WET is only the effector tail)";
    return s;
  }

 private:
  static std::string seconds_(double s) {
    char b[24];
    std::snprintf(b, sizeof b, "%.1f s", s);
    return b;
  }
  bool sounding_ = false;
  Clock::time_point quietSince_{}, summaryAt_{}, fxLoudAt_{}, wetLoudAt_{};
  float maxWet_ = 0.f, maxDry_ = 0.f, maxFx_ = 0.f, maxIn_ = 0.f;
};

}  // namespace lunar24::host::diag
