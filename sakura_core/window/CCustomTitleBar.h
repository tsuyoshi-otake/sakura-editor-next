/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include "theme/CThemeService.h"

struct CustomFrameLayout;

//! Returns a physical icon size constrained to 16--20 DIP for the supplied title area.
[[nodiscard]] int CalculateCustomTitleBarIconSize(int titleHeight, UINT dpi) noexcept;
//! Selects the glyph color for a caption button, including its highlighted close-button state.
[[nodiscard]] theme::ThemeColor CustomTitleBarGlyphColor(
	const theme::ThemePalette& palette,
	bool active,
	LRESULT hit,
	LRESULT hotHit,
	LRESULT pressedHit
) noexcept;

//! GDI painter for Sakura-owned opaque title chrome.
class CCustomTitleBar final {
public:
	void Paint(
		HWND owner,
		HDC dc,
		const CustomFrameLayout& layout,
		const theme::ThemePalette& palette,
		HFONT font,
		bool active,
		LRESULT hotHit,
		LRESULT pressedHit
	) const noexcept;
};
