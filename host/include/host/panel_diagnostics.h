// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/panel_diagnostics.h — what the machine is doing, as log text. UI thread
// only: reads the engine's committed state and its published (atomic) readouts, never the
// audio thread's runtime.
//
//   machineSnapshot  the whole panel at one moment: every knob, every cable (by panel name),
//                    the drone keys, what the running keyboard holds, the envelope lights,
//                    the engine's loss counters and the complete device state as hex.
//   PanelWatch       what changed on the panel, at most twice a second (knob drags and MIDI
//                    CC sweeps coalesce to their latest value; this is not an event replay).
//   EngineWatch      the engine's event-loss counters, logged the moment one grows.
#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

#include <host/diag_log.h>
#include <host/standalone_audio_engine.h>
#include <lunar24/core/state_serializer.h>

namespace lunar24::host::diag {

inline std::string jackName(core::JackId id) {
  for (const auto& j : registry::kJacks)
    if (j.id == id) return std::string(j.stable_id);
  return "jack#" + std::to_string(static_cast<unsigned>(id));
}

inline const char* keyboardModeName(int m) {
  return m == 1 ? "arpeggiator" : m == 2 ? "sequencer" : "keyboard";
}

// `extra`: lines the caller knows and the engine does not (what the UI and MIDI hold).
inline std::string machineSnapshot(const StandaloneAudioEngine& e, const std::string& reason,
                                   const std::string& extra = {}) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(10) << "snapshot begin reason=" << reason << " ready=" << e.isReady()
      << " state_version=" << e.stateVersion() << " muted=" << e.muted() << '\n';
  out << "drone_keys=";
  for (int i = 0; i < 6; ++i) out << (e.droneKey(i) ? '1' : '0');
  out << " (1 = open, voices 1-6)\n";
  out << "dsp keyboard: left holds " << e.keyboardHeldNotes(0) << " note(s), mode " << keyboardModeName(e.keyboardMode(0))
      << "; right holds " << e.keyboardHeldNotes(1) << ", mode " << keyboardModeName(e.keyboardMode(1)) << '\n';
  out << "lights (0..1, one block old): drone VCA";
  for (int i = 0; i < 6; ++i) out << ' ' << e.panelLed(StandaloneAudioEngine::kLedDrone1 + i);
  out << "; envelope A " << e.panelLed(StandaloneAudioEngine::kLedEnvA) << ", B "
      << e.panelLed(StandaloneAudioEngine::kLedEnvB) << '\n';
  const auto d = e.eventDiagnostics();
  out << "engine: live_event_drops=" << e.liveEventDropCount() << " critical_overflow=" << d.criticalOverflow
      << " critical_flushed=" << d.criticalFlushed << " continuous_overflow=" << d.continuousOverflow
      << " reconciles=" << d.reconcileCount << " late=" << d.lateCount << " batch_rejected=" << d.batchRejected
      << '\n';
  if (const core::DeviceStateV1* st = e.canonicalState()) {
    unsigned cables = 0;
    for (const auto& j : registry::kJacks) {
      const auto i = static_cast<unsigned>(j.id);
      if (i < core::kDevicePatchCapacity && st->inputCable[i]) {
        out << "cable " << jackName(st->cableSource[i]) << " -> " << j.stable_id << '\n';
        ++cables;
      }
    }
    out << "cables=" << cables << " (unpatched inputs use their normal connections)\n";
    out << "effector programs=" << static_cast<unsigned>(st->leftEffector.program) << ','
        << static_cast<unsigned>(st->rightEffector.program) << '\n';
    for (const auto& p : registry::kParameters) {
      out << "param " << p.stable_id << '=' << st->parameters[static_cast<unsigned>(p.id)] << '\n';
      const auto right = core::keyboard_scalar_index(p.id);
      if (right >= 0) out << "param " << p.stable_id << ".right=" << st->keyboardScalarRight[right] << '\n';
    }
    std::array<std::uint8_t, core::kDeviceStorageSchema.totalBytesHint> wire{};
    std::size_t n = 0;
    if (core::encode_device_state(*st, wire.data(), wire.size(), &n)) {
      static constexpr char hex[] = "0123456789abcdef";
      out << "device_state_hex=";
      for (std::size_t i = 0; i < n; ++i) out << hex[wire[i] >> 4] << hex[wire[i] & 15];
      out << '\n';
    }
  } else {
    out << "no machine state (audio not running)\n";
  }
  if (!extra.empty()) out << extra << '\n';
  out << "snapshot end";
  return out.str();
}

class PanelWatch {
 public:
  static constexpr auto kEvery = std::chrono::milliseconds(500);
  // Emit(const std::string&): one coalesced record, or a full snapshot when a whole new state
  // was installed (startup, RESET, preset, DAW project).
  template <class Emit>
  void poll(const StandaloneAudioEngine& e, std::chrono::steady_clock::time_point now, Emit emit) {
    if (now < next_) return;
    next_ = now + kEvery;
    const core::DeviceStateV1* st = e.canonicalState();
    if (!st) return;
    if (!have_ || version_ != e.stateVersion()) {
      emit(machineSnapshot(e, have_ ? "state installed" : "first state"));
    } else {
      std::ostringstream out;
      out.imbue(std::locale::classic());
      out << std::setprecision(10);
      for (const auto& p : registry::kParameters) {
        const auto i = static_cast<unsigned>(p.id);
        if (st->parameters[i] != prev_.parameters[i]) out << "\nparam " << p.stable_id << '=' << st->parameters[i];
        const auto r = core::keyboard_scalar_index(p.id);
        if (r >= 0 && st->keyboardScalarRight[r] != prev_.keyboardScalarRight[r])
          out << "\nparam " << p.stable_id << ".right=" << st->keyboardScalarRight[r];
      }
      for (unsigned i = 0; i < core::kDevicePatchCapacity; ++i) {
        const bool was = prev_.inputCable[i] != 0, is = st->inputCable[i] != 0;
        const auto sink = jackName(static_cast<core::JackId>(i));
        if (was && (!is || prev_.cableSource[i] != st->cableSource[i]))
          out << "\ncable removed " << jackName(prev_.cableSource[i]) << " -> " << sink;
        if (is && (!was || prev_.cableSource[i] != st->cableSource[i]))
          out << "\ncable added " << jackName(st->cableSource[i]) << " -> " << sink;
      }
      for (int i = 0; i < 6; ++i)
        if (e.droneKey(i) != keys_[i]) out << "\ndrone key " << i + 1 << (e.droneKey(i) ? " open" : " closed");
      if (st->leftEffector.program != prev_.leftEffector.program ||
          st->rightEffector.program != prev_.rightEffector.program)
        out << "\neffector programs=" << static_cast<unsigned>(st->leftEffector.program) << ','
            << static_cast<unsigned>(st->rightEffector.program);
      if (e.muted() != muted_) out << "\nmute " << (e.muted() ? "on" : "off");
      const std::uint32_t held[2] = {e.keyboardHeldNotes(0), e.keyboardHeldNotes(1)};
      if (held[0] != held_[0] || held[1] != held_[1])
        out << "\ndsp keyboard holds left=" << held[0] << " right=" << held[1];
      const std::string text = out.str();
      if (!text.empty()) emit("panel" + text);
    }
    prev_ = *st;
    for (int i = 0; i < 6; ++i) keys_[i] = e.droneKey(i);
    muted_ = e.muted();
    held_[0] = e.keyboardHeldNotes(0);
    held_[1] = e.keyboardHeldNotes(1);
    version_ = e.stateVersion();
    have_ = true;
  }

 private:
  core::DeviceStateV1 prev_{};
  std::chrono::steady_clock::time_point next_{};
  std::uint64_t version_ = 0;
  bool have_ = false, muted_ = false, keys_[6]{};
  std::uint32_t held_[2]{};
};

class EngineWatch {
 public:
  // Emit(Level, const std::string&) when a loss counter grew since the last poll.
  template <class Emit>
  void poll(const StandaloneAudioEngine& e, Emit emit) {
    const auto d = e.eventDiagnostics();
    const std::uint32_t now[6] = {e.liveEventDropCount(), d.criticalOverflow, d.criticalFlushed,
                                  d.continuousOverflow, d.reconcileCount, d.batchRejected};
    static const char* names[6] = {"live events dropped (UI queue full)",
                                   "note/gate events overflowed (all gates reset)",
                                   "pending note/gate events flushed",
                                   "parameter events overflowed",
                                   "all-gates-off reconciles",
                                   "event batches rejected"};
    std::string text;
    for (int i = 0; i < 6; ++i) {
      // A new machine starts its counters again: only growth is news.
      if (have_ && now[i] > last_[i])
        text += (text.empty() ? "" : "; ") + std::string(names[i]) + " +" + std::to_string(now[i] - last_[i]);
      last_[i] = now[i];
    }
    have_ = true;
    if (!text.empty()) emit(Level::warn, text);
  }

 private:
  std::uint32_t last_[6]{};
  bool have_ = false;
};

}  // namespace lunar24::host::diag
