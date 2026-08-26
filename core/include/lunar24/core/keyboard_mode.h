// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-③ keyboard-mode foundation (design/06 §L4, §P4; design/07 §3, §6).
//
// The panel is one physical knob per parameter. KeyboardSettings.pressureBehaviour
// is the GLOBAL single/twin/split selector; the two sides each act on a set of
// parameter values. Per design/00 §2d (msg 695564a7, P5 exit constraint:
// "control ledger 100% has visible controls AND no new controls"):
//
//   ParameterId identifies WHICH parameter (the one knob).
//   Side identifies WHICH value bank is read.
//
// The ParameterId space must NEVER grow for the right side — split is two
// *banks* under one id, never a new `_r` ParameterId. That is the whole point of
// this layer: it makes "which bank" explicit and independent of "which id".
//
// Per @Claude's direction, every per-note behaviour (portamento / vibrato /
// pressure / quantiser) must read a parameter WITH side context, never from the
// global parameters[] directly. This header is the single place that resolves a
// (mode, side, id) triple to a (bank index, id) read; the concrete DeviceState
// per-side value banks are wired in the next slice (live-state per-side storage).

#pragma once

#include <cstdint>
#include <utility>

#include <lunar24/core/id_types.h>

namespace lunar24::core {

// The keyboard mode, i.e. how the two performance sides map onto value banks
// (grounded: design/00 §2c, manual L675-681 — twin shares, split divides).
enum class KeyboardMode : std::uint8_t {
  Single = 0,  // one bank; both sides act on the single bank
  Twin = 1,    // two sides SHARE one bank ("…the two sides… share… same parame-")
  Split = 2,   // each side has an independent bank ("each having separate parameters")
};

// Which performance side of the panel a read comes from.
enum class KeyboardSide : std::uint8_t { Left = 0, Right = 1 };

// Resolve a (mode, side) to the value-bank index it reads. single/twin read bank
// 0 only (they never tell two banks apart); split reads an independent bank per
// side. The bank index is the "which copy" decision — the ParameterId is passed
// through unchanged by caller.
constexpr std::uint8_t side_bank(KeyboardMode mode, KeyboardSide side) noexcept {
  switch (mode) {
    case KeyboardMode::Single: return 0u;
    case KeyboardMode::Twin:   return 0u;
    case KeyboardMode::Split:  return side == KeyboardSide::Left ? 0u : 1u;
  }
  return 0u;
}

// Do both sides resolve to the same bank under this mode? A per-note behaviour's
// output target still applies to the *side that pressed the note*, but the value
// it reads is shared iff this is true. single/twin: true (one bank). split: false.
constexpr bool sides_share_bank(KeyboardMode mode) noexcept {
  return mode == KeyboardMode::Single || mode == KeyboardMode::Twin;
}

// THE side-context scalar choke point. Every per-note behaviour reads a parameter
// THROUGH this, never from global parameters[] directly (design/00 §2d, design/07
// §6). `bank` is the caller-injected value-bank reader — a callable
//   (std::uint8_t bankIndex, IdValue id) -> Value
// supplied so the concrete DeviceState writer (live-state per-side storage) can
// be wired in the next slice without this header knowing its layout. It returns
// the same Value type the bank reader returns.
template <typename BankReader>
auto read_side_scalar(BankReader&& bank, KeyboardMode mode, KeyboardSide side,
                      ParameterId id)
    -> decltype(std::forward<BankReader>(bank)(std::uint8_t{0}, IdValue{0})) {
  return std::forward<BankReader>(bank)(side_bank(mode, side), static_cast<IdValue>(id));
}

// PROVISIONAL decode of the raw pressureBehaviour selector into a KeyboardMode.
// The manual's raw-u8 mapping is NOT evidence-grounded yet (design/00 §3.2
// registry may hold three oct_sel positions; the behaviour enum decode is marked
// PROVISIONAL in device_state.h as well). Do not assert this mapping as design
// truth — unknown values collapse to Single so a rogue selector degrades to the
// conservative single-bank case instead of a phantom split.
constexpr KeyboardMode mode_from_behaviour(std::uint8_t behaviour) noexcept {
  switch (behaviour) {
    case 0: return KeyboardMode::Single;
    case 1: return KeyboardMode::Twin;
    case 2: return KeyboardMode::Split;
    default: return KeyboardMode::Single;
  }
}

}  // namespace lunar24::core
