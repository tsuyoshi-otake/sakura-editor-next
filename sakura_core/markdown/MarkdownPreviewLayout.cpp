/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "MarkdownPreviewLayout.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace markdown {
namespace {

constexpr int kDefaultDpi = 96;
constexpr int kMinimumEditorWidthDip = 160;
constexpr int kMinimumPreviewWidthDip = 180;
constexpr int kPreferredPreviewWidthDip = 540;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const auto scaled = (static_cast<std::int64_t>(dip) * effectiveDpi + kDefaultDpi / 2) / kDefaultDpi;
	return static_cast<int>(std::min<std::int64_t>(scaled, std::numeric_limits<int>::max()));
}

} // namespace

PreviewPaneLayout CalculateMarkdownPreviewLayout(int left, int right, unsigned int dpi, bool previewVisible) noexcept
{
	left = std::max(0, left);
	right = std::max(left, right);
	if (!previewVisible) {
		return { left, right, right, right, right, right };
	}

	const auto available = right - left;
	const auto dividerWidth = std::min(available, std::max(1, ScaleDip(1, dpi)));
	const auto contentWidth = available - dividerWidth;
	int editorWidth = contentWidth;
	int previewWidth = 0;
	if (contentWidth > 1) {
		const auto minimumEditor = ScaleDip(kMinimumEditorWidthDip, dpi);
		const auto minimumPreview = ScaleDip(kMinimumPreviewWidthDip, dpi);
		const auto preferredPreview = ScaleDip(kPreferredPreviewWidthDip, dpi);
		if (contentWidth >= minimumEditor + minimumPreview) {
			previewWidth = std::clamp(std::max(minimumPreview, contentWidth / 2),
				minimumPreview, std::min(preferredPreview, contentWidth - minimumEditor));
		} else {
			previewWidth = std::max(1, contentWidth / 2);
		}
		editorWidth = contentWidth - previewWidth;
	}

	const auto editorRight = left + editorWidth;
	const auto dividerRight = editorRight + dividerWidth;
	return { left, editorRight, editorRight, dividerRight, dividerRight, right };
}

} // namespace markdown
