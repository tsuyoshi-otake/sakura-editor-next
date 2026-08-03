/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::statusbar {

class StatusbarVisibilityMementoCodec final {
public:
	[[nodiscard]] static std::optional<std::string> Encode(const std::vector<std::string>& hiddenIds) noexcept;
	[[nodiscard]] static std::optional<std::vector<std::string>> Decode(std::string_view payload) noexcept;
};

} // namespace workbench::statusbar
