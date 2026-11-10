/*! @file
	@brief Binds an owner-drawn LISTBOX to the shared overlay scrollbar

	A workbench list keeps WS_VSCROLL so the control still owns its scroll
	position, while COverlayScrollbar hides the platform bar and draws VS
	Code's thin overlay instead. Two facts follow for every such list, and they
	live here so each view does not re-derive them:

	- The overlay model is read from the list's row contract, not from its
	  SCROLLINFO, which can publish a new page one paint late after a resize.
	- A list box whose scroll bar is hidden ignores WM_MOUSEWHEEL, so the
	  owner scrolls the rows itself.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/controls/COverlayScrollbar.h"

#include <Windows.h>

namespace workbench::controls {

//! The list's overlay model in rows: item count, whole visible rows, and the
//! top index. An empty or missing list yields an empty model.
[[nodiscard]] OverlayScrollbarModel ListBoxOverlayScrollbarModel(HWND list) noexcept;

//! Moves the list's top index by one WM_MOUSEWHEEL notch, honouring
//! SPI_GETWHEELSCROLLLINES including WHEEL_PAGESCROLL (one visible page).
void ScrollListBoxByWheel(HWND list, WPARAM wParam) noexcept;

} // namespace workbench::controls
