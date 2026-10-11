// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/include/host/computer_keys.h — which computer-keyboard plate keys are held down (the
// panel's A W S E D ... keys). UI thread only. A key-up is matched by its character, and when
// the character changed while the key was down (Shift, an input method), by the key's code.
// When the window loses the keyboard the OS sends no key-ups at all, so the host releases
// everything still held (releaseAll); otherwise the note would sound forever.
#pragma once

#include <map>

namespace lunar24::host {

class ComputerKeys {
 public:
  struct Held {
    int sounding = 0;  // the semitone the note-on played (octave shift at press time)
    int vk = 0;        // the key's virtual-key code
  };

  // A note key went down. False for an auto-repeat of a key already held (no new note).
  bool press(int semi, int vk, int sounding) {
    if (held_.count(semi) != 0) return false;
    held_[semi] = Held{sounding, vk};
    return true;
  }
  // A key went up. `semi` is the plate key its character names, or -1 when the character is
  // not a plate key. Returns the released key's semi, or -1 if it matches no held key.
  int release(int semi, int vk, Held* out = nullptr) {
    auto it = semi >= 0 ? held_.find(semi) : held_.end();
    if (it == held_.end())
      for (it = held_.begin(); it != held_.end() && it->second.vk != vk; ++it) {}
    if (it == held_.end()) return -1;
    const int released = it->first;
    if (out != nullptr) *out = it->second;
    held_.erase(it);
    return released;
  }
  // Release every held key: f(semi, Held) for each, then forget them. Returns how many.
  template <class F>
  int releaseAll(F f) {
    const int n = static_cast<int>(held_.size());
    for (const auto& [semi, h] : held_) f(semi, h);
    held_.clear();
    return n;
  }
  const std::map<int, Held>& held() const { return held_; }
  bool empty() const { return held_.empty(); }

 private:
  std::map<int, Held> held_;  // plate key (0..16, the index in "awsedftgyhujkolp;") -> its note
};

}  // namespace lunar24::host
