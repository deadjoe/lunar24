// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Which MIDI input ports the app listens to: first use, the four-device limit, stable slots
// across hot-plug, failed opens and the DEVICES tab rows.
#include "mini_test.h"

#include <host/midi_devices.h>

#include <string>
#include <vector>

using lunar24::host::MidiDeviceSetup;
using Ports = std::vector<std::string>;

static std::string planText(MidiDeviceSetup& d) {
  std::string out;
  for (const auto& c : d.plan())
    out += (out.empty() ? "" : " ") + std::string(c.open ? "open " : "close ") + std::to_string(c.slot) + ":" + c.name;
  return out;
}

int main() {
  // First use: nothing ever chosen, so the first real port turns on (once).
  {
    MidiDeviceSetup d;
    d.restore(false, {});
    CHECK(!d.setPorts({}, "To Lunar 24"));                  // only the virtual input: nothing turns on
    CHECK(!d.configured());
    CHECK(d.setPorts({"MPK mini IV", "iPad"}, "To Lunar 24"));
    CHECK(d.configured());
    CHECK(d.on() == Ports{"MPK mini IV"});
    CHECK_EQ(planText(d), "open 0:MPK mini IV");
    CHECK_EQ(planText(d), "");                               // already open
    CHECK(!d.setPorts({"MPK mini IV", "iPad"}, "To Lunar 24"));  // first use happens once
    CHECK(d.setOn("MPK mini IV", false));
    CHECK_EQ(planText(d), "close 0:MPK mini IV");
    CHECK(!d.setPorts({"Another"}, ""));                     // turned everything off: stays off
    CHECK(d.on().empty());
  }
  // A saved choice: new ports start off; at most four on.
  {
    MidiDeviceSetup d;
    d.restore(true, {"iPad", "iPad", ""});                  // duplicates and blanks dropped
    CHECK(d.on() == Ports{"iPad"});
    CHECK(!d.setPorts({"MPK mini IV", "iPad", "IAC Bus 1", "IAC Bus 2", "Launch Control"}));
    CHECK(!d.isOn("MPK mini IV"));
    CHECK_EQ(planText(d), "open 0:iPad");
    CHECK(d.setOn("MPK mini IV", true));
    CHECK(d.setOn("IAC Bus 1", true));
    CHECK(d.setOn("IAC Bus 2", true));
    CHECK(!d.setOn("Launch Control", true));                 // a fifth: refused
    CHECK(!d.isOn("Launch Control"));
    CHECK(d.setOn("iPad", true));                            // already on: fine
    CHECK_EQ(planText(d), "open 1:MPK mini IV open 2:IAC Bus 1 open 3:IAC Bus 2");
    // Unplug the iPad: only its slot closes; the others keep theirs. It stays on, unplugged.
    d.setPorts({"MPK mini IV", "IAC Bus 1", "IAC Bus 2", "Launch Control"});
    CHECK_EQ(planText(d), "close 0:iPad");
    const auto rows = d.rows();
    CHECK_EQ(rows.size(), 5u);
    CHECK_EQ(rows.back().name, "iPad");
    CHECK(rows.back().on && !rows.back().connected && rows.back().slot == -1);
    CHECK_EQ(rows[0].name, "MPK mini IV");
    CHECK_EQ(rows[0].slot, 1);
    CHECK(!rows[3].on && rows[3].connected);                // Launch Control: off
    // Turn IAC Bus 1 off: its slot frees; plugging the iPad back takes the lowest free slot.
    CHECK(d.setOn("IAC Bus 1", false));
    CHECK_EQ(planText(d), "close 2:IAC Bus 1");
    d.setPorts({"iPad", "MPK mini IV", "IAC Bus 1", "IAC Bus 2"});
    CHECK_EQ(planText(d), "open 0:iPad");
    CHECK_EQ(d.slotName(0), "iPad");
    CHECK_EQ(d.slotName(1), "MPK mini IV");
    CHECK_EQ(d.slotName(2), "");
    CHECK_EQ(d.slotName(9), "");
  }
  // A port that will not open: its slot frees, the row says so until it is unplugged or
  // turned off and on.
  {
    MidiDeviceSetup d;
    d.restore(true, {"Busy Port", "iPad"});
    d.setPorts({"Busy Port", "iPad"});
    CHECK_EQ(planText(d), "open 0:Busy Port open 1:iPad");
    d.openFailed(0);
    CHECK_EQ(d.slotOf("Busy Port"), -1);
    CHECK_EQ(planText(d), "");                               // not retried by itself
    CHECK(d.rows()[0].failed);
    CHECK(d.setOn("Busy Port", false));
    CHECK(d.setOn("Busy Port", true));
    CHECK(!d.rows()[0].failed);
    CHECK_EQ(planText(d), "open 0:Busy Port");
    d.openFailed(0);
    d.setPorts({"iPad"});                                    // unplugged: forgotten
    d.setPorts({"Busy Port", "iPad"});
    CHECK_EQ(planText(d), "open 0:Busy Port");
    d.closedAll();                                           // every port closed: all reopen
    CHECK_EQ(planText(d), "open 0:iPad open 1:Busy Port");  // in the order they were turned on
  }
  // The virtual input (macOS) is a row of its own after the ports; ports named like it are
  // the same row, and a duplicate port name is one row.
  {
    MidiDeviceSetup d;
    d.restore(true, {"To Lunar 24"});
    d.setPorts({"MPK mini IV", "MPK mini IV", "To Lunar 24"}, "To Lunar 24");
    const auto rows = d.rows();
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[1].isVirtual && rows[1].on && rows[1].connected);
    CHECK(!rows[0].isVirtual);
    CHECK_EQ(planText(d), "open 0:To Lunar 24");
    d.setPorts({"MPK mini IV"}, "");                         // no virtual input any more
    CHECK_EQ(planText(d), "close 0:To Lunar 24");
  }
  return test::finish("test_midi_devices");
}
