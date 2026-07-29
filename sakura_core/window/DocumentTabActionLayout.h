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

[[nodiscard]] constexpr int ScaleDocumentTabDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? 96U : dpi;
	const auto scaled = static_cast<std::int64_t>(dip) * effectiveDpi;
	return static_cast<int>((scaled + 48) / 96);
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

// The 47-DIP legacy margin contains the list and close actions. Markdown adds
// one 16-DIP action and its 7-DIP inter-action gap, producing a 70-DIP margin.
[[nodiscard]] constexpr DocumentTabActionLayout CalculateDocumentTabActionLayout(
	int clientLeft, int clientTop, int clientRight, int clientBottom,
	unsigned int dpi, bool showMarkdownPreview, int sizeBoxWidth = 0) noexcept
{
	clientRight = (std::max)(clientLeft, clientRight);
	clientBottom = (std::max)(clientTop, clientBottom);
	sizeBoxWidth = (std::max)(0, sizeBoxWidth);

	const int button = ScaleDocumentTabDip(16, dpi);
	const int gap = ScaleDocumentTabDip(7, dpi);
	const int outerRight = ScaleDocumentTabDip(4, dpi);
	const int topInset = ScaleDocumentTabDip(2, dpi);
	const int margin = ScaleDocumentTabDip(showMarkdownPreview ? 70 : 47, dpi);
	const int actionRight = (std::max)(clientLeft, clientRight - sizeBoxWidth);

	DocumentTabActionLayout layout{};
	layout.tabControlRight = (std::clamp)(actionRight - margin, clientLeft, clientRight);
	layout.reservedRight = clientRight - layout.tabControlRight;

	const int closeRight = actionRight - outerRight;
	layout.close = detail::PlaceDocumentTabAction(
		clientLeft, clientTop, clientBottom, closeRight, button, topInset);
	const int listRight = closeRight - button - gap;
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
