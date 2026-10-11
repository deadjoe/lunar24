// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// A fresh engine starts with every DRONE VOICES key closed, and a closed drone is silent from
// its first sample. Tests that need a sounding machine open the six keys before the engine
// installs its machine (prepare / applyDeviceState).
#pragma once

#include <host/standalone_audio_engine.h>

inline void openDrones(lunar24::host::StandaloneAudioEngine& e) {
  const bool all[6] = {true, true, true, true, true, true};
  e.setDroneKeys(all);
}
