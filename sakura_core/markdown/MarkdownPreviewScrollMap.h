/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>

namespace markdown {

//! Maps a zero-based source line to the first rendered line at or after it,
//! clamping trailing blank/source-only lines to the final rendered row. The
//! layout range must be in document/source order.
template <class RenderLines>
[[nodiscard]] std::optional<int> PreviewTopForSourceLine(
	const RenderLines& lines, std::size_t sourceLine)
{
	if (lines.empty()) return std::nullopt;
	const auto found = std::lower_bound(lines.begin(), lines.end(), sourceLine,
		[](const auto& line, std::size_t requested) { return line.sourceLine < requested; });
	if (found == lines.end()) return std::prev(lines.end())->top;
	return found->top;
}

//! Maps the native preview scroll position to the source line owning the top
//! rendered row. This does not imply a caret move.
template <class RenderLines>
[[nodiscard]] std::optional<std::size_t> SourceLineForPreviewScroll(
	const RenderLines& lines, int scrollPosition)
{
	if (lines.empty()) return std::nullopt;
	const auto after = std::upper_bound(lines.begin(), lines.end(), scrollPosition,
		[](int position, const auto& line) { return position < line.top; });
	const auto visible = after == lines.begin() ? lines.begin() : std::prev(after);
	return visible->sourceLine;
}

} // namespace markdown
