// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// P1 slice-1 disposable probe (spike/). See FINDINGS.md. Not part of the product.
#include "RtAudio.h"
#include <cstdio>
int main() {
  printf("RtAudio version: %s\n", RtAudio::getVersion().c_str());
  RtAudio rta;
  auto ids = rta.getDeviceIds();
  printf("getDeviceIds() count = %zu\n", ids.size());
  for (auto id : ids) {
    try {
      RtAudio::DeviceInfo info = rta.getDeviceInfo(id);
      printf("  id=%-4u name=\"%s\"  in=%u  out=%u  defaultOut=%s defaultIn=%s\n",
             id, info.name.c_str(), info.inputChannels, info.outputChannels,
             info.isDefaultOutput?"y":"n", info.isDefaultInput?"y":"n");
    } catch (const std::exception& e) {
      printf("  id=%u  getDeviceInfo(realId) FAILED: %s\n", id, e.what());
    }
  }
  return 0;
}
