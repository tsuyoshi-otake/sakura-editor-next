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

//! Logical viewport position preserved across width-dependent reflow.
struct PreviewScrollAnchor {
	std::size_t sourceLine = 0;
	std::size_t sourceLineOrdinal = 0;
	int intraLineOffset = 0;
};

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

//! Captures the rendered row and pixel offset currently owning the viewport top.
template <class RenderLines>
[[nodiscard]] std::optional<PreviewScrollAnchor> CapturePreviewScrollAnchor(
	const RenderLines& lines, int scrollPosition)
{
	if (lines.empty()) return std::nullopt;
	const auto after = std::upper_bound(lines.begin(), lines.end(), scrollPosition,
		[](int position, const auto& line) { return position < line.top; });
	const auto visible = after == lines.begin() ? lines.begin() : std::prev(after);
	const auto firstForSource = std::lower_bound(lines.begin(), visible, visible->sourceLine,
		[](const auto& line, std::size_t sourceLine) { return line.sourceLine < sourceLine; });
	return PreviewScrollAnchor{
		visible->sourceLine,
		static_cast<std::size_t>(std::distance(firstForSource, visible)),
		std::max(0, scrollPosition - visible->top),
	};
}

//! Restores an anchor against a new wrapping. Missing rows fall forward to the
//! next source line, while a shortened set of rows clamps to the last peer.
template <class RenderLines>
[[nodiscard]] std::optional<int> RestorePreviewScrollAnchor(
	const RenderLines& lines, const PreviewScrollAnchor& anchor)
{
	if (lines.empty()) return std::nullopt;
	auto found = std::lower_bound(lines.begin(), lines.end(), anchor.sourceLine,
		[](const auto& line, std::size_t sourceLine) { return line.sourceLine < sourceLine; });
	if (found == lines.end()) found = std::prev(lines.end());
	if (found->sourceLine == anchor.sourceLine) {
		auto selected = found;
		for (std::size_t ordinal = 0; ordinal < anchor.sourceLineOrdinal; ++ordinal) {
			const auto next = std::next(selected);
			if (next == lines.end() || next->sourceLine != anchor.sourceLine) break;
			selected = next;
		}
		found = selected;
	}
	return std::max(0, found->top + anchor.intraLineOffset);
}

} // namespace markdown
