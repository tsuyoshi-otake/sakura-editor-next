/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "markdown/MarkdownParser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace markdown {

enum class InlineStyleFlag : std::uint8_t {
	Strong = 1U << 0,
	Emphasis = 1U << 1,
	Link = 1U << 2,
	Code = 1U << 3,
	Image = 1U << 4,
	Strikethrough = 1U << 5,
};

[[nodiscard]] constexpr std::uint8_t InlineStyleMask(InlineStyleFlag flag) noexcept
{
	return static_cast<std::uint8_t>(flag);
}

struct InlineStyleRun final {
	std::size_t start = 0;
	std::size_t length = 0;
	std::uint8_t styles = 0;

	[[nodiscard]] constexpr std::size_t End() const noexcept { return start + length; }
	[[nodiscard]] constexpr bool Has(InlineStyleFlag flag) const noexcept
	{
		return (styles & InlineStyleMask(flag)) != 0;
	}
};

struct InlineStyleRunBuildStatistics final {
	std::size_t inputSpanCount = 0;
	std::size_t boundaryEventCount = 0;
	std::size_t boundaryGroupCount = 0;
	std::size_t outputRunCount = 0;
};

struct InlineStyleRunBuildResult final {
	std::vector<InlineStyleRun> runs;
	InlineStyleRunBuildStatistics statistics;
};

namespace detail {

struct InlineBoundaryEvent final {
	std::size_t offset = 0;
	std::uint8_t styleIndex = 0;
	std::int8_t delta = 0;
};

[[nodiscard]] inline std::optional<std::uint8_t> InlineStyleIndex(InlineKind kind) noexcept
{
	switch (kind) {
	case InlineKind::Strong: return static_cast<std::uint8_t>(0);
	case InlineKind::Emphasis: return static_cast<std::uint8_t>(1);
	case InlineKind::Link: return static_cast<std::uint8_t>(2);
	case InlineKind::Autolink: return static_cast<std::uint8_t>(2);
	case InlineKind::Code: return static_cast<std::uint8_t>(3);
	case InlineKind::Math: return static_cast<std::uint8_t>(3);
	case InlineKind::Image: return static_cast<std::uint8_t>(4);
	case InlineKind::Strikethrough: return static_cast<std::uint8_t>(5);
	default:
		// New/unsupported inline nodes retain their literal text until the native
		// renderer defines an explicit style. They never terminate the sweep early.
		return std::nullopt;
	}
}

} // namespace detail

//! Normalizes possibly unsorted, overlapping, and duplicate spans once into
//! non-overlapping half-open style runs. Complexity is O(S log S + R), where S
//! is the span count and R is the number of emitted runs. The result covers the
//! entire text, including undecorated ranges, so paint needs one linear sweep.
[[nodiscard]] inline InlineStyleRunBuildResult BuildInlineStyleRuns(
	std::size_t textLength, const std::vector<InlineSpan>& spans)
{
	InlineStyleRunBuildResult result;
	result.statistics.inputSpanCount = spans.size();
	if (textLength == 0) return result;

	std::vector<detail::InlineBoundaryEvent> events;
	if (spans.size() <= std::numeric_limits<std::size_t>::max() / 2) {
		events.reserve(spans.size() * 2);
	}
	for (const auto& span : spans) {
		const auto styleIndex = detail::InlineStyleIndex(span.kind);
		if (!styleIndex || span.length == 0 || span.start >= textLength) continue;
		const auto remaining = textLength - span.start;
		const auto end = span.length >= remaining ? textLength : span.start + span.length;
		if (end <= span.start) continue;
		events.push_back({ span.start, *styleIndex, 1 });
		events.push_back({ end, *styleIndex, -1 });
	}
	result.statistics.boundaryEventCount = events.size();
	std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
		if (left.offset != right.offset) return left.offset < right.offset;
		if (left.styleIndex != right.styleIndex) return left.styleIndex < right.styleIndex;
		return left.delta < right.delta;
	});

	// size_t cannot overflow before the input event vector itself exhausts the
	// process address space, including pathological duplicate-span inputs.
	std::array<std::size_t, 6> depth{};
	std::uint8_t activeStyles = 0;
	std::size_t position = 0;
	std::size_t eventIndex = 0;
	result.runs.reserve(events.size() + 1);
	while (eventIndex < events.size()) {
		const auto boundary = events[eventIndex].offset;
		if (boundary > position) {
			result.runs.push_back({ position, boundary - position, activeStyles });
			position = boundary;
		}
		++result.statistics.boundaryGroupCount;
		while (eventIndex < events.size() && events[eventIndex].offset == boundary) {
			const auto& event = events[eventIndex++];
			auto& count = depth[event.styleIndex];
			if (event.delta > 0) {
				++count;
			} else if (count != 0) {
				--count;
			}
		}
		activeStyles = 0;
		for (std::size_t index = 0; index < depth.size(); ++index) {
			activeStyles |= depth[index] == 0 ? 0
				: static_cast<std::uint8_t>(1U << index);
		}
	}
	if (position < textLength) result.runs.push_back({ position, textLength - position, activeStyles });
	result.statistics.outputRunCount = result.runs.size();
	return result;
}

//! Clips already normalized runs to one wrapped line. A lower_bound skips every
//! earlier run in O(log R), after which only the runs that paint this line are
//! visited. Returned offsets are relative to lineStart.
[[nodiscard]] inline std::vector<InlineStyleRun> ClipInlineStyleRuns(
	const std::vector<InlineStyleRun>& runs, std::size_t lineStart, std::size_t lineLength)
{
	std::vector<InlineStyleRun> clipped;
	if (lineLength == 0 || runs.empty()) return clipped;
	const auto maximumLength = std::numeric_limits<std::size_t>::max() - lineStart;
	const auto lineEnd = lineStart + std::min(lineLength, maximumLength);
	const auto first = std::lower_bound(runs.begin(), runs.end(), lineStart,
		[](const InlineStyleRun& run, std::size_t start) { return run.End() <= start; });
	for (auto iterator = first; iterator != runs.end() && iterator->start < lineEnd; ++iterator) {
		const auto start = std::max(lineStart, iterator->start);
		const auto end = std::min(lineEnd, iterator->End());
		if (start < end) clipped.push_back({ start - lineStart, end - start, iterator->styles });
	}
	return clipped;
}

} // namespace markdown
