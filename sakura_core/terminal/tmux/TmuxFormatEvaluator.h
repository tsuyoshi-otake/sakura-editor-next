/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandTypes.h"

#include <cstddef>
#include <string_view>

namespace terminal::tmux {

struct TmuxFormatLimits final {
	std::size_t maximumInputBytes{ 4096 };
	std::size_t maximumNestingDepth{ 8 };
	std::size_t maximumExpandedBytes{ 512u * 1024u };
};

class TmuxFormatEvaluator final {
public:
	[[nodiscard]] static TmuxFormatResult Evaluate(
		std::string_view format,
		const TmuxFormatContext& context,
		TmuxFormatLimits limits = {}) noexcept;

	[[nodiscard]] static TmuxFilterResult EvaluateFilter(
		std::string_view expression,
		const TmuxFormatContext& context,
		TmuxFormatLimits limits = {}) noexcept;
};

} // namespace terminal::tmux
