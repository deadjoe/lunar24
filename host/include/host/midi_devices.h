// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// midi_devices.h — which MIDI input ports the app listens to (the MIDI panel's DEVICES tab).
//
// Framework-free and UI-thread only: the host lists the ports it sees, this decides which
// are on, which input slot (0-3, the audio side's device index) each open port uses, and what
// the DEVICES tab shows. Ports are known by name: two ports with the same name are one.
// - Up to kMidiInputDevices ports are on at once. A new port starts off; the user's choice is
//   remembered by name, so a port that is on reopens whenever it is plugged in again.
// - First use (nothing ever chosen): the first real port that appears is turned on, so a
//   keyboard plays as soon as it is plugged in.
// - An open port keeps its slot while it stays on and plugged in (closing a slot releases
//   that input's notes, so slots never move under a playing device).

#pragma once

#include <host/midi_input_queue.h>

#include <algorithm>
#include <string>
#include <vector>

namespace lunar24::host {

struct MidiDeviceRow {
  std::string name;
  bool on = false;          // the user turned it on (remembered)
  bool connected = false;   // the port is there now
  bool failed = false;      // on and connected, but the port would not open
  bool isVirtual = false;   // the app's own virtual input (macOS)
  int slot = -1;            // the input slot it is open on, or -1
};

class MidiDeviceSetup {
 public:
  static constexpr unsigned kMaxOn = kMidiInputDevices;

  // The saved choice. `configured` false: nothing was ever chosen (first use).
  void restore(bool configured, const std::vector<std::string>& on) {
    configured_ = configured;
    on_.clear();
    for (const auto& name : on)
      if (!name.empty() && !contains(on_, name) && on_.size() < kMaxOn) on_.push_back(name);
  }
  bool configured() const { return configured_; }
  const std::vector<std::string>& on() const { return on_; }
  bool isOn(const std::string& name) const { return contains(on_, name); }

  // The user turns a port on or off. False (nothing changes) when turning on would exceed
  // kMaxOn. Any change counts as a choice: first use is over.
  bool setOn(const std::string& name, bool on) {
    if (name.empty()) return false;
    if (on == isOn(name)) { configured_ = true; return true; }
    if (on && on_.size() >= kMaxOn) return false;
    if (on) on_.push_back(name);
    else on_.erase(std::find(on_.begin(), on_.end(), name));
    failed_.erase(std::remove(failed_.begin(), failed_.end(), name), failed_.end());
    configured_ = true;
    return true;
  }

  // The ports the host sees now (real ports in its order) and its virtual input's name ("" on
  // a system without one). True when first use turned a port on (save the choice).
  bool setPorts(const std::vector<std::string>& ports, const std::string& virtualName = "") {
    ports_.clear();
    for (const auto& p : ports)
      if (!p.empty() && p != virtualName && !contains(ports_, p)) ports_.push_back(p);
    virtualName_ = virtualName;
    failed_.erase(std::remove_if(failed_.begin(), failed_.end(),
                                 [&](const std::string& n) { return !present(n); }), failed_.end());
    if (configured_ || !on_.empty() || ports_.empty()) return false;
    on_.push_back(ports_.front());
    configured_ = true;
    return true;
  }

  // What the host must do to match the choice: close these slots, then open these ports.
  // Applying the plan is assumed to succeed; a port that would not open is reported back
  // with openFailed(slot).
  struct Change {
    bool open = false;
    unsigned slot = 0;
    std::string name;
  };
  std::vector<Change> plan() {
    std::vector<Change> changes;
    for (unsigned s = 0; s < kMaxOn; ++s)
      if (!slots_[s].empty() && (!isOn(slots_[s]) || !present(slots_[s]))) {
        changes.push_back({false, s, slots_[s]});
        slots_[s].clear();
      }
    for (const auto& name : on_) {
      if (!present(name) || contains(failed_, name) || slotOf(name) >= 0) continue;
      for (unsigned s = 0; s < kMaxOn; ++s)
        if (slots_[s].empty()) {
          slots_[s] = name;
          changes.push_back({true, s, name});
          break;
        }
    }
    return changes;
  }
  // The port planned on `slot` would not open: the slot is free again and the row says so
  // until the port is turned off and on, or unplugged.
  void openFailed(unsigned slot) {
    if (slot >= kMaxOn || slots_[slot].empty()) return;
    if (!contains(failed_, slots_[slot])) failed_.push_back(slots_[slot]);
    slots_[slot].clear();
  }
  // Every port closed (the host shuts down or loses MIDI): the next plan reopens them.
  void closedAll() { for (auto& s : slots_) s.clear(); }

  const std::string& slotName(unsigned slot) const {
    static const std::string none;
    return slot < kMaxOn ? slots_[slot] : none;
  }
  int slotOf(const std::string& name) const {
    for (unsigned s = 0; s < kMaxOn; ++s)
      if (slots_[s] == name) return static_cast<int>(s);
    return -1;
  }
  bool isVirtual(const std::string& name) const { return !virtualName_.empty() && name == virtualName_; }

  // The DEVICES tab: the ports there now (the host's order, then the virtual input), then the
  // ones that are on but unplugged.
  std::vector<MidiDeviceRow> rows() const {
    std::vector<MidiDeviceRow> out;
    auto add = [&](const std::string& name, bool connected) {
      MidiDeviceRow r;
      r.name = name;
      r.on = isOn(name);
      r.connected = connected;
      r.failed = contains(failed_, name);
      r.isVirtual = isVirtual(name);
      r.slot = slotOf(name);
      out.push_back(r);
    };
    for (const auto& p : ports_) add(p, true);
    if (!virtualName_.empty()) add(virtualName_, true);
    for (const auto& name : on_)
      if (!present(name)) add(name, false);
    return out;
  }

 private:
  static bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  }
  bool present(const std::string& name) const { return contains(ports_, name) || isVirtual(name); }

  bool configured_ = false;
  std::vector<std::string> on_, ports_, failed_;
  std::string virtualName_;
  std::string slots_[kMaxOn];
};

}  // namespace lunar24::host
