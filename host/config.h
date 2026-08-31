// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/config.h — iPlug2 standalone build configuration for the Lunar 24 host
// standalone editor. This is the APP_API config (the desktop app that opens a
// real macOS window), NOT a DAW plugin. The window itself is sized by the
// geometry choke point in host/include/host/window_layout.h (P5-①), so the
// PLUG_WIDTH/PLUG_HEIGHT here are only the initial editor size that the host
// then overrides via SetEditorSize() inside the mMakeGraphicsFunc lambda.
//
// ⚠️ Do NOT #define BUNDLE_ID or APP_GROUP_ID: IPlug_include_in_plug_hdr.h
// derives them as
//   BUNDLE_ID = BUNDLE_DOMAIN "." BUNDLE_MFR "." API_EXT "." BUNDLE_NAME
// (API_EXT == "app" for APP_API). Defining them here is a redefinition warning.
// Provide only the three bundle pieces and let the header compose the rest.

#ifndef HOST_CONFIG_H
#define HOST_CONFIG_H

#define PLUG_NAME "Lunar24Host"
#define PLUG_MFR "Lunar24"
#define PLUG_VERSION_HEX 0x00000001
#define PLUG_VERSION_STR "0.0.1"
#define PLUG_UNIQUE_ID 'Lu24'
#define PLUG_MFR_ID 'Lua2'
#define PLUG_URL_STR "https://github.com/deadjoe/lunar24"
#define PLUG_EMAIL_STR "lunar24@example.invalid"
#define PLUG_COPYRIGHT_STR "Copyright 2026 Lunar 24 contributors"
#define PLUG_CLASS_NAME LunarHostPlugin

// macOS bundle id pieces (BUNDLE_ID is derived by the iPlug header).
#define BUNDLE_NAME "Lunar24Host"
#define BUNDLE_MFR "Lunar24"
#define BUNDLE_DOMAIN "com"

// GH#4 8B3 (task#73): PLUG_CHANNEL_IO is an EXACT set of legal I/O configs, not a "max capability"
// string. The APP branch declares the six legal combos the standalone host may open; iPlug2's
// ParseChannelIOStr takes max over them, so MaxNChannels(input) = 2 and MaxNChannels(output) = 4
// (the channel DATA can hold up to 4 outputs). The ACTUAL number this stream opens is negotiated
// from the device capability (host/include/host/stream_plan.h) and installed via
// LunarHostPlugin::setActualChannelPlan() BEFORE OnReset — a 2-out device opens 2, never a forced
// 4. Every VALID negotiated plan must be admitted by iPlug2's AUTHORITATIVE parsed-config check
// IPlugProcessor::LegalIO(in,out) (see host/plugin.cpp setActualChannelPlan); stream_plan.h
// ::is_legal_io is only the framework-free STREAM-POLICY invariant that agrees with it.
#ifdef APP_API
#define PLUG_CHANNEL_IO "0-2 1-2 2-2 0-4 1-4 2-4"
#else
#define PLUG_CHANNEL_IO "1-1 2-2"
#endif

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1

// Initial editor size (design space). The host overrides these with the fit
// logical size at open time; these are just the "default editor" the library
// seeds before the geometry choke point runs.
#define PLUG_WIDTH 2400
#define PLUG_HEIGHT 1551
// PLUG_MIN/MAX_WIDTH/HEIGHT are left undefined: IPlug_include_in_plug_hdr.h
// derives them from PLUG_WIDTH/HEIGHT.

#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#endif  // HOST_CONFIG_H
