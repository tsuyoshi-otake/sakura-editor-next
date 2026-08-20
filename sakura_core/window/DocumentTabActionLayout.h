/*! @file
	@brief Pure layout for document-tab actions.
*/
/*
	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_DOCUMENTTABACTIONLAYOUT_H_
#define SAKURA_DOCUMENTTABACTIONLAYOUT_H_
#pragma once

#include <algorithm>
#include <cstdint>

namespace tabbar {

struct ActionRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr bool IsEmpty() const noexcept
	{
		return left >= right || top >= bottom;
	}

	[[nodiscard]] constexpr bool Contains(int x, int y) const noexcept
	{
		return !IsEmpty() && x >= left && x < right && y >= top && y < bottom;
	}

	[[nodiscard]] constexpr int Width() const noexcept
	{
		return (std::max)(0, right - left);
	}

	[[nodiscard]] constexpr int Height() const noexcept
	{
		return (std::max)(0, bottom - top);
	}
};

enum class DocumentTabAction {
	None,
	MarkdownPreview,
	TabList,
	Close,
};

struct DocumentTabActionLayout {
	ActionRect preview;
	ActionRect list;
	ActionRect close;
	int tabControlRight = 0;
	int reservedRight = 0;
};

struct DocumentTabContentLayout {
	ActionRect icon;
	ActionRect text;
	ActionRect close;
};

enum class DocumentTabActionVisualState {
	Normal,
	Hovered,
	Pressed,
};

/*!
	@brief The action the document tab's preview button actually invokes

	This is deliberately not markdown.showPreviewToSide. That upstream command
	opens the preview in a side editor group, and this shell has no second
	EditorGroup: the button toggles the Sakura-owned sibling preview pane
	(F_TOGGLE_MARKDOWN_PREVIEW / MarkdownPreviewCommandState::ToggleNativeSibling).
	Declaring the VS Code id here would alias a different concept, so the action
	carries a sakura.* id until real multiple editor groups exist.
*/
inline constexpr wchar_t kMarkdownPreviewCommandId[] = L"sakura.toggleMarkdownSiblingPreview";
inline constexpr wchar_t kMarkdownPreviewCodiconId[] = L"open-preview";

[[nodiscard]] constexpr int ScaleDocumentTabDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? 96U : dpi;
	const auto scaled = static_cast<std::int64_t>(dip) * effectiveDpi;
	return static_cast<int>((scaled + 48) / 96);
}

//! VS Code's first editor tab starts at the editor-part content edge. Keeping
//! a legacy host inset exposes an unpainted vertical strip before that tab.
[[nodiscard]] constexpr int CalculateDocumentTabControlLeftInset() noexcept
{
	return 0;
}

[[nodiscard]] constexpr int CalculateDocumentTabItemWidth(
	int availableWidth, int itemCount, int configuredMinimum, int configuredMaximum) noexcept
{
	if (itemCount <= 0) return 0;
	const int minimum = (std::max)(1, configuredMinimum);
	const int maximum = (std::max)(minimum, configuredMaximum);
	const int evenlyDistributed = (std::max)(0, availableWidth - 8) / itemCount;
	return (std::clamp)(evenlyDistributed, minimum, maximum);
}

// The native variable-width tab control measures the caption and image, then
// adds this value to both sides. Keep that measurement in lockstep with the
// owner-drawn 8/4-DIP content rhythm so a close affordance never steals space
// from a caption that otherwise fits.
[[nodiscard]] constexpr int CalculateDocumentTabNativeHorizontalPaddingDip(
	bool showIcon, bool reserveClose) noexcept
{
	constexpr int leading = 8;
	constexpr int icon = 16;
	constexpr int iconGap = 4;
	constexpr int trailing = 8;
	constexpr int closeGap = 4;
	constexpr int closeTarget = 24;
	constexpr int closeTrailing = 4;
	const int ownerDrawNonText = leading + (showIcon ? icon + iconGap : 0)
		+ (reserveClose ? closeGap + closeTarget + closeTrailing : trailing);
	const int nativeImageWidth = showIcon ? icon : 0;
	return (ownerDrawNonText - nativeImageWidth + 1) / 2;
}

[[nodiscard]] constexpr DocumentTabActionVisualState ResolveDocumentTabActionVisualState(
	bool highlighted, bool captured, bool captureOwned) noexcept
{
	if (highlighted && captured && captureOwned) return DocumentTabActionVisualState::Pressed;
	if (highlighted) return DocumentTabActionVisualState::Hovered;
	return DocumentTabActionVisualState::Normal;
}

namespace detail {

[[nodiscard]] constexpr ActionRect PlaceDocumentTabAction(
	int clientLeft, int clientTop, int clientBottom, int right, int width, int topInset) noexcept
{
	const int top = clientTop + topInset;
	if (width <= 0 || right - width < clientLeft || top + width > clientBottom) {
		return { clientLeft, clientTop, clientLeft, clientTop };
	}
	return { right - width, top, right, top + width };
}

} // namespace detail

// Actions use 24-DIP hit targets around 16-DIP glyphs, with a 4-DIP rhythm.
[[nodiscard]] constexpr DocumentTabActionLayout CalculateDocumentTabActionLayout(
	int clientLeft, int clientTop, int clientRight, int clientBottom,
	unsigned int dpi, bool showMarkdownPreview, bool showCloseAll, int sizeBoxWidth = 0) noexcept
{
	clientRight = (std::max)(clientLeft, clientRight);
	clientBottom = (std::max)(clientTop, clientBottom);
	sizeBoxWidth = (std::max)(0, sizeBoxWidth);

	const int button = ScaleDocumentTabDip(24, dpi);
	const int gap = ScaleDocumentTabDip(4, dpi);
	const int outerRight = ScaleDocumentTabDip(4, dpi);
	const int topInset = ScaleDocumentTabDip(4, dpi);
	const int actionCount = 1 + (showMarkdownPreview ? 1 : 0) + (showCloseAll ? 1 : 0);
	const int margin = outerRight * 2 + actionCount * button + (actionCount - 1) * gap;
	const int actionRight = (std::max)(clientLeft, clientRight - sizeBoxWidth);

	DocumentTabActionLayout layout{};
	layout.tabControlRight = (std::clamp)(actionRight - margin, clientLeft, clientRight);
	layout.reservedRight = clientRight - layout.tabControlRight;

	int nextRight = actionRight - outerRight;
	if (showCloseAll) {
		layout.close = detail::PlaceDocumentTabAction(
			clientLeft, clientTop, clientBottom, nextRight, button, topInset);
		nextRight -= button + gap;
	} else {
		layout.close = { clientLeft, clientTop, clientLeft, clientTop };
	}
	const int listRight = nextRight;
	layout.list = detail::PlaceDocumentTabAction(
		clientLeft, clientTop, clientBottom, listRight, button, topInset);
	if (showMarkdownPreview) {
		const int previewRight = listRight - button - gap;
		layout.preview = detail::PlaceDocumentTabAction(
			clientLeft, clientTop, clientBottom, previewRight, button, topInset);
	} else {
		layout.preview = { clientLeft, clientTop, clientLeft, clientTop };
	}
	return layout;
}

[[nodiscard]] constexpr ActionRect CalculateDocumentTabActionGlyphBounds(
	const ActionRect& action, unsigned int dpi) noexcept
{
	if (action.IsEmpty()) return { action.left, action.top, action.left, action.top };
	const int glyph = ScaleDocumentTabDip(16, dpi);
	const int width = (std::min)(glyph, action.Width());
	const int height = (std::min)(glyph, action.Height());
	const int left = action.left + (action.Width() - width) / 2;
	const int top = action.top + (action.Height() - height) / 2;
	return { left, top, left + width, top + height };
}

[[nodiscard]] constexpr DocumentTabContentLayout CalculateDocumentTabContentLayout(
	const ActionRect& tab, unsigned int dpi, bool showIcon, bool showClose) noexcept
{
	if (tab.IsEmpty()) {
		const ActionRect empty{ tab.left, tab.top, tab.left, tab.top };
		return { empty, empty, empty };
	}

	const int leading = ScaleDocumentTabDip(8, dpi);
	const int trailing = ScaleDocumentTabDip(8, dpi);
	const int gap = ScaleDocumentTabDip(4, dpi);
	const int iconSize = ScaleDocumentTabDip(16, dpi);
	const int closeSize = ScaleDocumentTabDip(24, dpi);
	const int closeTrailing = ScaleDocumentTabDip(4, dpi);

	ActionRect close{ tab.right, tab.top, tab.right, tab.top };
	int textRight = (std::max)(tab.left, tab.right - trailing);
	if (showClose) {
		const int closeRight = (std::max)(tab.left, tab.right - closeTrailing);
		const int closeLeft = (std::max)(tab.left, closeRight - closeSize);
		const int closeHeight = (std::min)(closeSize, tab.Height());
		const int closeTop = tab.top + (tab.Height() - closeHeight) / 2;
		close = { closeLeft, closeTop, closeRight, closeTop + closeHeight };
		textRight = (std::max)(tab.left, closeLeft - gap);
	}

	const int contentLeft = (std::min)(textRight, tab.left + leading);
	ActionRect icon{ contentLeft, tab.top, contentLeft, tab.top };
	int textLeft = contentLeft;
	if (showIcon && contentLeft + iconSize + gap <= textRight) {
		const int iconTop = tab.top + (tab.Height() - (std::min)(iconSize, tab.Height())) / 2;
		icon = { contentLeft, iconTop, contentLeft + iconSize,
			iconTop + (std::min)(iconSize, tab.Height()) };
		textLeft = icon.right + gap;
	}
	return { icon, { (std::min)(textLeft, textRight), tab.top, textRight, tab.bottom }, close };
}

[[nodiscard]] constexpr DocumentTabAction HitTestDocumentTabAction(
	const DocumentTabActionLayout& layout, int x, int y) noexcept
{
	if (layout.preview.Contains(x, y)) return DocumentTabAction::MarkdownPreview;
	if (layout.list.Contains(x, y)) return DocumentTabAction::TabList;
	if (layout.close.Contains(x, y)) return DocumentTabAction::Close;
	return DocumentTabAction::None;
}

} // namespace tabbar

#endif // SAKURA_DOCUMENTTABACTIONLAYOUT_H_
