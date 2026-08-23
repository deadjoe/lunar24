// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Stable identity types for the canonical machine registry.
//
// These opaque enums are only *declared* here and *completed* by the generated
// header <lunar24/registry_ids.hpp>. The concrete enumerators must never be
// renumbered or reused once serialized — stable ids are frozen by the machine
// definition (spec/machine/lunar24.json).
#pragma once

#include <cstdint>

namespace lunar24::core {

// A fixed underlying type makes each of these a complete type even while only
// forward-declared, so descriptor structs can hold them by value.
enum class ModuleId : std::uint32_t;
enum class ParameterId : std::uint32_t;
enum class JackId : std::uint32_t;
enum class ProgramId : std::uint32_t;
enum class RouteId : std::uint32_t;

// The numeric type shared by every id (the value in the enum).
using IdValue = std::uint32_t;

}  // namespace lunar24::core
