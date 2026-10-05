// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/plugin_settings_dir.h — the per-user settings folder for the VST3 / AUv2 plugins: the
// same folder the standalone app uses (iPlug_app_host_override.cpp InitState), so both read
// one MIDI controller map. Plugins only.
#pragma once

#include <string>

namespace lunar24::host {

// macOS: ~/Library/Application Support/Lunar24/; Windows: %LOCALAPPDATA%\Lunar24\ (UTF-8, with
// the trailing separator). "" when it cannot be found: the MIDI map then stays at its default.
std::string plugin_settings_dir();

}  // namespace lunar24::host
