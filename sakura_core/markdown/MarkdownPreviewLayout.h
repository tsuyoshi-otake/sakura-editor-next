/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

namespace markdown {

enum class PreviewPaneMode {
	Hidden,
	Replacement,
	NativeSibling,
};

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

//! A requested preview width of zero means "use the default proportion".
inline constexpr int kPreviewDefaultWidthRequestDip = 0;

/*!
	@name Divider drag limits, in device-independent pixels

	The divider may never be dragged past these, matching VS Code's sash, which
	stops rather than collapsing a group it is not allowed to close.
*/
///@{
inline constexpr int kMinimumEditorWidthDip = 160;
inline constexpr int kMinimumPreviewWidthDip = 180;
///@}

//! Splits a physical-pixel central editor region without consuming minimap or workbench space.
[[nodiscard]] PreviewPaneLayout CalculateMarkdownPreviewLayout(
	int left, int right, unsigned int dpi, bool previewVisible) noexcept;

[[nodiscard]] PreviewPaneLayout CalculateMarkdownPreviewLayout(
	int left, int right, unsigned int dpi, PreviewPaneMode mode) noexcept;

/*!
	@brief Splits the region honoring a user-dragged preview width

	`requestedPreviewWidthDip` is the width the divider drag asked for. It is
	clamped against both minimums rather than rejected, so a drag that runs past
	either edge parks the divider at the limit instead of doing nothing. Pass
	kPreviewDefaultWidthRequestDip to restore the default proportion.
*/
[[nodiscard]] PreviewPaneLayout CalculateMarkdownPreviewLayout(
	int left, int right, unsigned int dpi, PreviewPaneMode mode,
	int requestedPreviewWidthDip) noexcept;

/*!
	@brief Converts a pointer position during a divider drag into a width request

	The pointer holds the divider's left edge, so the preview keeps everything to
	its right. The result is never zero, because zero means "default" rather than
	"as narrow as possible".
*/
[[nodiscard]] int RequestedPreviewWidthDipFromPointer(
	int right, int pointerX, unsigned int dpi) noexcept;

} // namespace markdown
