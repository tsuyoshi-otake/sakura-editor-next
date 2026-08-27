/*! @file
 * @brief Decoder for the copied Rust Output provider snapshot ABI.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/output/OutputServiceTypes.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace workbench::output {

inline constexpr std::string_view kOutputServiceRustSnapshotMagicV1(
	"SAKURA_OUTPUT_MODEL_V1\0", 23);

//! Decodes one complete copied V1 snapshot. Malformed, truncated, oversized,
//! or trailing input is rejected without retaining the input storage.
[[nodiscard]] std::optional<OutputServiceSnapshot> DecodeOutputServiceRustSnapshotV1(
	const std::vector<std::uint8_t>& bytes);

} // namespace workbench::output
