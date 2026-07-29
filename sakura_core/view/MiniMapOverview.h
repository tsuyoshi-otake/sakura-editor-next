/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>
#include <cstdint>

namespace minimap {

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

} // namespace minimap
