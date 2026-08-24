/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>
#include <cstdint>

namespace minimap {

enum class Side : std::uint8_t {
	Right,
	Left,
};

enum class Size : std::uint8_t {
	Proportional,
	Fill,
	Fit,
};

enum class ShowSlider : std::uint8_t {
	Always,
	MouseOver,
};

enum class AutoHide : std::uint8_t {
	None,
	MouseOver,
	Scroll,
};

//! Commands exposed by VS Code's minimap context menu. Keeping the command
//! model independent from HMENU lets paint, configuration, and tests share one
//! authoritative state transition.
enum class ContextCommand : std::uint8_t {
	ToggleEnabled,
	ToggleRenderCharacters,
	SizeProportional,
	SizeFill,
	SizeFit,
	SliderMouseOver,
	SliderAlways,
	SideRight,
	SideLeft,
};

//! The supported native projection of VS Code's editor.minimap settings.
struct Options {
	bool enabled = true;
	AutoHide autohide = AutoHide::None;
	Side side = Side::Right;
	Size size = Size::Proportional;
	ShowSlider showSlider = ShowSlider::MouseOver;
	bool renderCharacters = true;
	int maxColumn = 120;
	int scale = 1;

	[[nodiscard]] constexpr bool operator==(const Options&) const noexcept = default;
};

[[nodiscard]] constexpr Options ApplyContextCommand(
	Options options, ContextCommand command) noexcept
{
	switch (command) {
	case ContextCommand::ToggleEnabled:
		options.enabled = !options.enabled;
		break;
	case ContextCommand::ToggleRenderCharacters:
		options.renderCharacters = !options.renderCharacters;
		break;
	case ContextCommand::SizeProportional:
		options.size = Size::Proportional;
		break;
	case ContextCommand::SizeFill:
		options.size = Size::Fill;
		break;
	case ContextCommand::SizeFit:
		options.size = Size::Fit;
		break;
	case ContextCommand::SliderMouseOver:
		options.showSlider = ShowSlider::MouseOver;
		break;
	case ContextCommand::SliderAlways:
		options.showSlider = ShowSlider::Always;
		break;
	case ContextCommand::SideRight:
		options.side = Side::Right;
		break;
	case ContextCommand::SideLeft:
		options.side = Side::Left;
		break;
	}
	return options;
}

//! Only these settings alter the retained overview raster. Slider visibility,
//! side, enabled, and autohide affect composition or workbench layout instead.
[[nodiscard]] constexpr bool HasSameOverviewRendering(
	const Options& left, const Options& right) noexcept
{
	return left.size == right.size
		&& left.renderCharacters == right.renderCharacters
		&& left.maxColumn == right.maxColumn
		&& left.scale == right.scale;
}

[[nodiscard]] constexpr int LineToPixel(std::int64_t line, std::int64_t lineCount, int height) noexcept
{
	if (lineCount <= 0 || height <= 0) return 0;
	line = std::clamp<std::int64_t>(line, 0, lineCount);
	return static_cast<int>((line * height) / lineCount);
}

[[nodiscard]] constexpr std::int64_t PixelToLine(int pixel, std::int64_t lineCount, int height) noexcept
{
	if (lineCount <= 0 || height <= 0) return 0;
	pixel = std::clamp(pixel, 0, height - 1);
	return std::min<std::int64_t>(lineCount - 1,
		(static_cast<std::int64_t>(pixel) * lineCount) / height);
}

struct ViewportPixels {
	int top = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
	[[nodiscard]] constexpr bool operator==(const ViewportPixels&) const noexcept = default;
};

[[nodiscard]] constexpr ViewportPixels ViewportToPixels(std::int64_t topLine, std::int64_t bottomLine,
	std::int64_t lineCount, int height) noexcept
{
	if (lineCount <= 0 || height <= 0) return {};
	int top = LineToPixel(topLine, lineCount, height);
	int bottom = LineToPixel(bottomLine, lineCount, height);
	if (bottom <= top) bottom = std::min(height, top + 1);
	return { top, bottom };
}

struct LayoutInput {
	std::int64_t lineCount = 0;
	std::int64_t editorTopLine = 0;
	std::int64_t editorVisibleLines = 0;
	int height = 0;
};

//! Pure minimap geometry. Paint and pointer handling consume the same mapping.
struct Layout {
	std::int64_t lineCount = 0;
	std::int64_t firstLine = 0;
	std::int64_t visibleLineSpan = 0;
	std::int64_t editorVisibleLines = 0;
	int height = 0;
	int lineHeight = 1;
	bool sampled = false;
	ViewportPixels viewport{};

	[[nodiscard]] constexpr bool operator==(const Layout&) const noexcept = default;

	[[nodiscard]] constexpr std::int64_t LastLineExclusive() const noexcept
	{
		return std::min(lineCount, firstLine + visibleLineSpan);
	}

	[[nodiscard]] constexpr int LineToY(std::int64_t line) const noexcept
	{
		if (height <= 0 || visibleLineSpan <= 0) return 0;
		line = std::clamp(line, firstLine, LastLineExclusive());
		if (sampled) {
			return static_cast<int>(((line - firstLine) * height) / visibleLineSpan);
		}
		return static_cast<int>((line - firstLine) * lineHeight);
	}

	[[nodiscard]] constexpr std::int64_t YToLine(int y) const noexcept
	{
		if (height <= 0 || visibleLineSpan <= 0) return 0;
		y = std::clamp(y, 0, height - 1);
		const std::int64_t offset = sampled
			? (static_cast<std::int64_t>(y) * visibleLineSpan) / height
			: y / std::max(1, lineHeight);
		return std::min(lineCount - 1, firstLine + offset);
	}

	[[nodiscard]] constexpr std::int64_t CenteredEditorTopForY(int y) const noexcept
	{
		if (lineCount <= 0) return 0;
		const auto centerLine = YToLine(y);
		const auto maximumTop = std::max<std::int64_t>(0, lineCount - editorVisibleLines);
		return std::clamp(centerLine - editorVisibleLines / 2,
			std::int64_t{ 0 }, maximumTop);
	}
};

[[nodiscard]] constexpr Layout CalculateLayout(const Options& options, const LayoutInput& input) noexcept
{
	Layout result;
	result.lineCount = std::max<std::int64_t>(0, input.lineCount);
	result.editorVisibleLines = std::clamp<std::int64_t>(
		input.editorVisibleLines, 0, result.lineCount);
	result.height = std::max(0, input.height);
	if (result.lineCount == 0 || result.height == 0) return result;

	// Configuration values are 1..3, but the view passes a DPI-projected
	// device-space scale. Keep that projection instead of clamping it back to
	// the logical setting range.
	const int scale = std::clamp(options.scale, 1, 64);
	const int naturalLineHeight = (options.renderCharacters ? 2 : 3) * scale;
	const auto naturalHeight = result.lineCount * static_cast<std::int64_t>(naturalLineHeight);

	if (options.size == Size::Proportional) {
		result.lineHeight = naturalLineHeight;
		result.visibleLineSpan = std::min<std::int64_t>(result.lineCount,
			(result.height + naturalLineHeight - 1) / naturalLineHeight);
		const auto maximumFirst = result.lineCount - result.visibleLineSpan;
		const auto maximumEditorTop = std::max<std::int64_t>(
			0, result.lineCount - result.editorVisibleLines);
		const auto editorTop = std::clamp(input.editorTopLine,
			std::int64_t{ 0 }, maximumEditorTop);
		if (maximumFirst > 0 && maximumEditorTop > 0) {
			result.firstLine = (editorTop * maximumFirst + maximumEditorTop / 2)
				/ maximumEditorTop;
		}
	} else if (options.size == Size::Fit && naturalHeight <= result.height) {
		result.lineHeight = naturalLineHeight;
		result.visibleLineSpan = result.lineCount;
	} else if (result.lineCount <= result.height) {
		result.lineHeight = std::max(1, result.height / static_cast<int>(result.lineCount));
		result.visibleLineSpan = result.lineCount;
	} else {
		result.lineHeight = 1;
		result.visibleLineSpan = result.lineCount;
		result.sampled = true;
	}

	const auto editorTop = std::clamp(input.editorTopLine,
		std::int64_t{ 0 }, result.lineCount);
	const auto editorBottom = std::min(result.lineCount,
		editorTop + result.editorVisibleLines);
	result.viewport.top = std::clamp(result.LineToY(editorTop), 0, result.height);
	result.viewport.bottom = std::clamp(result.LineToY(editorBottom), 0, result.height);
	if (result.viewport.bottom <= result.viewport.top) {
		result.viewport.bottom = std::min(result.height, result.viewport.top + 1);
	}
	return result;
}

[[nodiscard]] constexpr int PreferredWidthDip(const Options& options) noexcept
{
	return std::clamp(options.maxColumn, 1, 10000) * std::clamp(options.scale, 1, 3) + 8;
}

} // namespace minimap
