/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/controls/ListBoxOverlayScrolling.h"

#include <algorithm>

namespace workbench::controls {

OverlayScrollbarModel ListBoxOverlayScrollbarModel(HWND list) noexcept
{
	if (list == nullptr) return {};
	const int count = std::max(0,
		static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0)));
	const int itemHeight = std::max(1,
		static_cast<int>(::SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
	RECT client{};
	::GetClientRect(list, &client);
	const int viewport = std::max(1,
		static_cast<int>(client.bottom - client.top) / itemHeight);
	const int top = std::max(0,
		static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
	return { .contentExtent = count, .viewportExtent = viewport, .offset = top };
}

void ScrollListBoxByWheel(HWND list, WPARAM wParam) noexcept
{
	if (list == nullptr) return;
	const int count = static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
	if (count <= 0) return;
	UINT linesPerNotch = 3;
	if (::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0) == FALSE) {
		linesPerNotch = 3;
	}
	// WHEEL_PAGESCROLL asks for a page; the visible row count is the page here.
	int rows = 0;
	const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
	if (linesPerNotch == WHEEL_PAGESCROLL) {
		RECT client{};
		::GetClientRect(list, &client);
		const int rowHeight = static_cast<int>(::SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
		const int page = rowHeight > 0
			? std::max(1, static_cast<int>(client.bottom - client.top) / rowHeight) : 1;
		rows = delta > 0 ? -page : page;
	} else {
		rows = -(delta * static_cast<int>(linesPerNotch)) / WHEEL_DELTA;
	}
	if (rows == 0) return;
	const int top = static_cast<int>(::SendMessageW(list, LB_GETTOPINDEX, 0, 0));
	const int next = std::clamp(top + rows, 0, std::max(0, count - 1));
	if (next == top) return;
	(void)::SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(next), 0);
}

} // namespace workbench::controls
