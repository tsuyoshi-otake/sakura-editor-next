/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

namespace markdown {

//! The editor and preview bounds within the existing central editor region.
struct PreviewPaneLayout {
	int editorLeft = 0;
	int editorRight = 0;
	int dividerLeft = 0;
	int dividerRight = 0;
	int previewLeft = 0;
	int previewRight = 0;

	[[nodiscard]] constexpr int EditorWidth() const noexcept { return editorRight - editorLeft; }
	[[nodiscard]] constexpr int PreviewWidth() const noexcept { return previewRight - previewLeft; }
};

//! Splits a physical-pixel central editor region without consuming minimap or workbench space.
[[nodiscard]] PreviewPaneLayout CalculateMarkdownPreviewLayout(
	int left, int right, unsigned int dpi, bool previewVisible) noexcept;

} // namespace markdown
