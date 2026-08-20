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
constexpr int kPreferredPreviewWidthDip = 540;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const auto scaled = (static_cast<std::int64_t>(dip) * effectiveDpi + kDefaultDpi / 2) / kDefaultDpi;
	return static_cast<int>(std::min<std::int64_t>(scaled, std::numeric_limits<int>::max()));
}

[[nodiscard]] int DipFromScaled(int pixels, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const auto dip = (static_cast<std::int64_t>(pixels) * kDefaultDpi
		+ static_cast<std::int64_t>(effectiveDpi) / 2) / effectiveDpi;
	return static_cast<int>(std::min<std::int64_t>(dip, std::numeric_limits<int>::max()));
}

} // namespace

int RequestedPreviewWidthDipFromPointer(int right, int pointerX, unsigned int dpi) noexcept
{
	return std::max(1, DipFromScaled(std::max(0, right - pointerX), dpi));
}

PreviewPaneLayout CalculateMarkdownPreviewLayout(int left, int right, unsigned int dpi, bool previewVisible) noexcept
{
	return CalculateMarkdownPreviewLayout(left, right, dpi,
		previewVisible ? PreviewPaneMode::NativeSibling : PreviewPaneMode::Hidden);
}

PreviewPaneLayout CalculateMarkdownPreviewLayout(int left, int right, unsigned int dpi, PreviewPaneMode mode) noexcept
{
	return CalculateMarkdownPreviewLayout(left, right, dpi, mode, kPreviewDefaultWidthRequestDip);
}

PreviewPaneLayout CalculateMarkdownPreviewLayout(int left, int right, unsigned int dpi,
	PreviewPaneMode mode, int requestedPreviewWidthDip) noexcept
{
	left = std::max(0, left);
	right = std::max(left, right);
	if (mode == PreviewPaneMode::Hidden) {
		return { left, right, right, right, right, right };
	}
	if (mode == PreviewPaneMode::Replacement) {
		return { left, left, left, left, left, right };
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
			// A dragged width is honored up to both minimums; only the untouched
			// default is additionally capped at the preferred width, so the user
			// can make the preview wider than the default but never wide enough
			// to squeeze the editor below its minimum.
			const auto defaultPreview = std::clamp(std::max(minimumPreview, contentWidth / 2),
				minimumPreview, std::min(preferredPreview, contentWidth - minimumEditor));
			previewWidth = requestedPreviewWidthDip <= kPreviewDefaultWidthRequestDip
				? defaultPreview
				: std::clamp(ScaleDip(requestedPreviewWidthDip, dpi),
					minimumPreview, contentWidth - minimumEditor);
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
