// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <iomanip>
#include <locale>
#include <sstream>
#include <host/standalone_audio_engine.h>
#include <lunar24/core/state_serializer.h>

namespace lunar24::host {
// UI thread only. Never dereference engine.runtime(): it belongs to audio.
// The wire record preserves sequences, tuning, presets and selectors as well as
// the human-readable knobs/cables. It is device state, NOT a DSP memory dump.
inline std::string diagnosticSnapshot(const StandaloneAudioEngine& e, const char* reason) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << "snapshot begin reason=" << reason
      << " version=" << e.stateVersion() << " ready=" << e.isReady()
      << " muted=" << e.muted() << " live_event_drops=" << e.liveEventDropCount() << '\n';
  const auto* st = e.canonicalState();
  if (st) {
    for (const auto& p : registry::kParameters) {
      out << "param " << p.stable_id << '=' << st->parameters[static_cast<unsigned>(p.id)] << '\n';
      const auto right = core::keyboard_scalar_index(p.id);
      if (right >= 0) out << "right " << p.stable_id << '=' << st->keyboardScalarRight[right] << '\n';
    }
    unsigned cables = 0;
    for (const auto& j : registry::kJacks) {
      const auto i = static_cast<unsigned>(j.id);
      if (i < core::kDevicePatchCapacity && st->inputCable[i]) {
        out << "cable " << static_cast<unsigned>(st->cableSource[i]) << " -> " << j.stable_id << '\n';
        ++cables;
      }
    }
    out << "cables=" << cables << " (unpatched inputs use normal connections)\n";
    std::array<std::uint8_t, core::kDeviceStorageSchema.totalBytesHint> wire{};
    std::size_t n = 0;
    if (core::encode_device_state(*st, wire.data(), wire.size(), &n)) {
      constexpr char hex[] = "0123456789abcdef";
      out << "device_state_hex=";
      for (std::size_t i = 0; i < n; ++i) out << hex[wire[i] >> 4] << hex[wire[i] & 15];
      out << '\n';
    }
  }
  out << "drone_keys=";
  for (int i = 0; i < 6; ++i) out << (e.droneKey(i) ? '1' : '0');
  out << "\nlast_published_envelopes (asynchronous, not a simultaneous DSP snapshot):";
  for (int i = 0; i <= StandaloneAudioEngine::kLedEnvB; ++i) out << ' ' << e.panelLed(i);
  out << "\nsnapshot end";
  return out.str();
}

// At most two UI-state comparisons per second; no work per audio sample. Knob
// drags/MIDI edits coalesce to the most recent value. This is not an event replay.
class PanelDiagnostics {
 public:
  template<class Emit>
  void poll(const StandaloneAudioEngine& e, std::chrono::steady_clock::time_point now, Emit emit) {
    if (now < next_) return;
    next_ = now + std::chrono::milliseconds(500);
    const auto* st = e.canonicalState();
    if (!st) return;
    if (!have_ || version_ != e.stateVersion()) {
      emit(diagnosticSnapshot(e, "state-installed"));
    } else {
      std::ostringstream out;
      out.imbue(std::locale::classic());
      out << std::setprecision(17);
      for (const auto& p : registry::kParameters) {
        const auto i = static_cast<unsigned>(p.id);
        if (st->parameters[i] != previous_.parameters[i])
          out << "param " << p.stable_id << '=' << st->parameters[i] << '\n';
        const auto r = core::keyboard_scalar_index(p.id);
        if (r >= 0 && st->keyboardScalarRight[r] != previous_.keyboardScalarRight[r])
          out << "right " << p.stable_id << '=' << st->keyboardScalarRight[r] << '\n';
      }
      for (unsigned i = 0; i < core::kDevicePatchCapacity; ++i)
        if (st->inputCable[i] != previous_.inputCable[i] ||
            (st->inputCable[i] && st->cableSource[i] != previous_.cableSource[i]))
          out << "cable sink=" << i << " source="
              << (st->inputCable[i] ? static_cast<unsigned>(st->cableSource[i]) : 0) << '\n';
      for (int i = 0; i < 6; ++i)
        if (e.droneKey(i) != keys_[i]) out << "drone " << i + 1 << '=' << e.droneKey(i) << '\n';
      if (st->leftEffector.program != previous_.leftEffector.program ||
          st->rightEffector.program != previous_.rightEffector.program)
        out << "effector programs=" << static_cast<unsigned>(st->leftEffector.program)
            << ',' << static_cast<unsigned>(st->rightEffector.program) << '\n';
      if (e.muted() != muted_) out << "muted=" << e.muted() << '\n';
      const auto text = out.str();
      if (!text.empty()) emit("panel changes (coalesced 500ms)\n" + text);
    }
    previous_ = *st;
    for (int i = 0; i < 6; ++i) keys_[i] = e.droneKey(i);
    muted_ = e.muted(); version_ = e.stateVersion(); have_ = true;
  }
 private:
  core::DeviceStateV1 previous_{};
  std::chrono::steady_clock::time_point next_{};
  std::uint64_t version_ = 0;
  bool have_ = false, muted_ = false, keys_[6]{};
};
}  // namespace lunar24::host
