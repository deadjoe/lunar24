// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace lunar24::core {

// A pointer to the primary source that justifies an auditable descriptor.
//
// `source` names the reference document (e.g. "solar42N_manual_v15"),
// `lineStart`/`lineEnd` locate the claim within it (a single line has both equal).
struct EvidenceRef {
  std::string_view source;
  std::uint32_t lineStart = 0;
  std::uint32_t lineEnd = 0;
};

}  // namespace lunar24::core
