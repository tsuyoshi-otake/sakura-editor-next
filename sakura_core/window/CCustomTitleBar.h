/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include "theme/CThemeService.h"

struct CustomFrameLayout;

//! Sakura-owned compact title-bar controls placed immediately before the native caption buttons.
//! `Update` is VS Code's `workbench.actions.updateIndicator`
//! (`contrib/update/browser/updateTitleBarEntry.ts`, `MenuId.TitleBarUpdate` order 0).
//! It is a labelled button rather than a glyph, and it exists only while the update
//! state is actionable, so its rectangle is empty in every other state.
enum class CustomFrameControl : unsigned char {
	None,
	Layout,
	PrimarySidebar,
	BottomPanel,
	SecondarySidebar,
	Update,
	Account,
	Manage,
};

[[nodiscard]] const wchar_t* CustomFrameControlName(CustomFrameControl control) noexcept;
[[nodiscard]] const wchar_t* CustomFrameControlAutomationId(CustomFrameControl control) noexcept;

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
	CCustomTitleBar() noexcept = default;
	~CCustomTitleBar() noexcept;
	CCustomTitleBar(const CCustomTitleBar&) = delete;
	CCustomTitleBar& operator=(const CCustomTitleBar&) = delete;
	CCustomTitleBar(CCustomTitleBar&&) = delete;
	CCustomTitleBar& operator=(CCustomTitleBar&&) = delete;

	void Paint(
		HWND owner,
		HDC dc,
		const CustomFrameLayout& layout,
		const theme::ThemePalette& palette,
		HFONT font,
		bool active,
		LRESULT hotHit,
		LRESULT pressedHit,
		CustomFrameControl hotControl,
		CustomFrameControl pressedControl,
		CustomFrameControl focusedControl
	) const noexcept;

private:
	[[nodiscard]] HFONT AcquireCodiconFont(int height) const noexcept;
	void ReleaseCodiconFont() const noexcept;

	mutable HFONT m_codiconFont = nullptr;
	mutable int m_codiconFontHeight = 0;
};
