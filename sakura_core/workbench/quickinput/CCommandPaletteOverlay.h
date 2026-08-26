/*! @file
	@brief VS Code互換のCommand Palette/Quick Pick用Quick Inputオーバーレイ
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/controls/CInputBoxGeometry.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "theme/CThemeService.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::quickinput {

//! Presentation data for one item shown by a workbench-local Quick Input surface.
struct CommandPaletteItem {
	std::wstring id;
	std::wstring label;
	std::wstring description;
	std::wstring detail;
	bool enabled = true;
	//! A visible group heading, never a legal answer.
	bool separator = false;
};

//! Localized strings supplied by the currently active Quick Input surface.
struct QuickInputStrings {
	std::wstring placeholder;
	std::wstring noResults;
};

//! The pixel geometry contract shared by the native Quick Input projection and
//! its pure tests.  `listContentHeight` is already DPI-scaled because variable
//! owner-draw rows are measured by USER in physical pixels.
struct QuickInputLayoutMetrics {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	int headerHeight = 0;
	int listTop = 0;
	int listHeight = 0;
};

//! The painted input frame and the native single-line EDIT hosted inside it.
//! USER does not vertically center a single-line EDIT's caret when the HWND is
//! stretched to the full chrome row, so the two rectangles are deliberately
//! separate just like VS Code's input widget and its editor content.
struct QuickInputRowGeometry {
	RECT frame{};
	RECT editor{};
};

//! Converts a positive DIP token using the same nearest-pixel rule as
//! `MulDiv(value, dpi, 96)` without requiring a USER/GDI handle.
[[nodiscard]] constexpr int ScaleQuickInputDip(int value, int dpi) noexcept
{
	const int effectiveDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
	return (value * effectiveDpi + USER_DEFAULT_SCREEN_DPI / 2)
		/ USER_DEFAULT_SCREEN_DPI;
}

//! `lineHeight` is the measured `TEXTMETRICW::tmHeight` of the input font; pass
//! zero before the font has been measured and the CSS padding is used instead.
//! The centering itself belongs to `workbench::controls::CenterSingleLineEditor`,
//! which the Search widget shares, mirroring the single upstream `InputBox`.
[[nodiscard]] constexpr QuickInputRowGeometry ComputeQuickInputRowGeometry(
	int x,
	int y,
	int width,
	int dpi,
	int lineHeight) noexcept
{
	const int frameWidth = (std::max)(0, width);
	const int frameHeight = ScaleQuickInputDip(26, dpi);
	const RECT frame{ x, y, x + frameWidth, y + frameHeight };
	return {
		.frame = frame,
		.editor = controls::CenterSingleLineEditor(frame, lineHeight,
			ScaleQuickInputDip(1, dpi), frameHeight - 2 * ScaleQuickInputDip(3, dpi)),
	};
}

//! Mouse presses outside Quick Input end the non-modal session.  The message is
//! deliberately not consumed: the workbench surface the user clicked must still
//! receive the click after the palette has closed, matching VS Code's blur model.
[[nodiscard]] constexpr bool IsQuickInputDismissMouseMessage(UINT message) noexcept
{
	switch (message) {
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_NCLBUTTONDOWN:
	case WM_NCRBUTTONDOWN:
	case WM_NCMBUTTONDOWN:
	case WM_NCXBUTTONDOWN:
		return true;
	default:
		return false;
	}
}

//! Pure command-palette provider parsing.  The provider marker is part of the
//! EDIT value, while filtering receives only the user query after it.
[[nodiscard]] constexpr std::wstring_view StripCommandPaletteProviderPrefix(
	std::wstring_view value) noexcept
{
	return !value.empty() && value.front() == L'>' ? value.substr(1) : value;
}

//! Computes the bounded overlay geometry for one host/DPI combination.  Keeping
//! this arithmetic outside HWND code makes narrow clients and non-96-DPI
//! rounding behavior directly testable.
[[nodiscard]] constexpr QuickInputLayoutMetrics ComputeQuickInputLayout(
	int parentWidth,
	int parentHeight,
	int dpi,
	int listContentHeight,
	bool inputMode) noexcept
{
	const int edgeMargin = ScaleQuickInputDip(6, dpi);
	const int availableWidth = (std::max)(0, parentWidth - edgeMargin * 2);
	const int goldenWidth = parentWidth * 62 / 100;
	const int width = (std::min)(availableWidth,
		(std::min)(ScaleQuickInputDip(600, dpi),
			(std::max)(ScaleQuickInputDip(320, dpi), goldenWidth)));
	const int headerHeight = ScaleQuickInputDip(6, dpi)
		+ ScaleQuickInputDip(26, dpi) + ScaleQuickInputDip(4, dpi);
	const int listBottomPadding = inputMode ? 0 : ScaleQuickInputDip(7, dpi);
	const int availableHeight = (std::max)(0,
		parentHeight - edgeMargin - headerHeight);
	const int rowBand = (std::max)(1, ScaleQuickInputDip(44, dpi));
	const int rawMaximum = parentHeight * 40 / 100;
	const int alignedMaximum = rawMaximum / rowBand * rowBand + ScaleQuickInputDip(6, dpi);
	const int contentMaximum = (std::max)(0, availableHeight - listBottomPadding);
	const int boundedContent = (std::min)((std::max)(0, listContentHeight),
		(std::min)(alignedMaximum, contentMaximum));
	const int listHeight = inputMode ? 0 : boundedContent + listBottomPadding;
	const int height = headerHeight + listHeight;
	return {
		.x = (std::max)(0, (parentWidth - width) / 2),
		.y = (std::min)(edgeMargin, (std::max)(0, parentHeight - height)),
		.width = width,
		.height = height,
		.headerHeight = headerHeight,
		.listTop = headerHeight,
		.listHeight = listHeight,
	};
}

//! Borderless, non-modal Quick Input surface used by Ctrl+Shift+P and theme picking.
class CCommandPaletteOverlay final {
public:
	using SearchCallback = std::function<std::vector<CommandPaletteItem>(std::wstring_view)>;
	using SelectionCallback = std::function<void(std::wstring)>;
	using AcceptCallback = std::function<void(std::wstring)>;
	using CancelCallback = std::function<void()>;
	using StringsCallback = std::function<QuickInputStrings()>;

	CCommandPaletteOverlay() noexcept = default;
	~CCommandPaletteOverlay() noexcept;
	CCommandPaletteOverlay(const CCommandPaletteOverlay&) = delete;
	CCommandPaletteOverlay& operator=(const CCommandPaletteOverlay&) = delete;

	[[nodiscard]] bool Create(HWND parent) noexcept;
	void Destroy() noexcept;

	//! Shows the palette without disabling or entering a nested message loop for the owner.
	[[nodiscard]] bool Show(
		std::vector<CommandPaletteItem> items,
		std::wstring_view initiallySelectedId = {});
	//! Shows the same chromeless surface as an input box. Enter accepts the
	//! current text and Escape/close cancels it; no modal caption or dialog
	//! buttons are created.
	[[nodiscard]] bool ShowInput(
		std::wstring_view prompt,
		std::wstring_view placeholder,
		std::wstring_view value = {});
	void Hide() noexcept;
	//! Closes the palette through its cancel terminal, if it is visible.
	void Cancel() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;

	//! Gives the editor message loop first chance to handle palette keyboard input.
	[[nodiscard]] bool PreTranslateMessage(MSG& message) noexcept;
	void Layout() noexcept;
	//! Refreshes language-dependent control text without replacing the current query/results.
	void RefreshStrings() noexcept;

	void SetPalette(const theme::ThemePalette& palette) noexcept;
	void SetStringsCallback(StringsCallback callback);
	void SetSearchCallback(SearchCallback callback);
	void SetSelectionCallback(SelectionCallback callback);
	void SetAcceptCallback(AcceptCallback callback);
	void SetCancelCallback(CancelCallback callback);

private:
	static constexpr int kInputControl = 100;
	static constexpr int kListControl = 101;
	static constexpr int kCloseControl = 102;
	static constexpr int kEmptyControl = 103;

	static ATOM RegisterWindowClass() noexcept;
	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
	static LRESULT CALLBACK ListSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data) noexcept;
	static LRESULT CALLBACK InputSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data) noexcept;
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	void Layout(int width, int height) noexcept;
	void PopulateList(std::wstring_view preferredSelectionId = {}) noexcept;
	void UpdateSearch() noexcept;
	void NormalizeCommandPaletteInput() noexcept;
	void PinCommandPaletteCaret() noexcept;
	void MeasureInputLineHeight() noexcept;
	void MoveSelection(int direction) noexcept;
	void NotifySelectionChanged() noexcept;
	[[nodiscard]] std::wstring SelectedItemId() const;
	void EnsureSelectableSelection() noexcept;
	void Accept() noexcept;
	void RestoreFocus() noexcept;
	void Paint(HDC dc, const RECT& bounds) noexcept;
	void DrawItem(const DRAWITEMSTRUCT& draw) noexcept;
	void DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept;
	void ScrollListByWheel(WPARAM wParam) noexcept;
	void ScrollListToPixelOffset(int pixelOffset) noexcept;
	void RebuildRowPixelOffsets() noexcept;
	void EnsureRowPixelOffsets() noexcept;
	[[nodiscard]] controls::OverlayScrollbarModel ListScrollModel() noexcept;
	void UpdateOverlayScrollbar() noexcept;
	void RebuildBrushes() noexcept;
	void ResetBrushes() noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;

	[[nodiscard]] int Scale(int value) const noexcept;
	[[nodiscard]] static bool IsPaletteTarget(HWND palette, HWND target) noexcept;
	[[nodiscard]] static std::wstring ReadWindowText(HWND window);
	[[nodiscard]] static HFONT ControlFont(HFONT fallback) noexcept;

	HWND m_parent = nullptr;
	HWND m_window = nullptr;
	HWND m_prompt = nullptr;
	HWND m_input = nullptr;
	HWND m_list = nullptr;
	HWND m_close = nullptr;
	HWND m_empty = nullptr;
	HWND m_previousFocus = nullptr;
	RECT m_inputFrame{};
	//! Measured TEXTMETRICW::tmHeight of the input font, cached per DPI.
	int m_inputLineHeight = 0;
	UINT m_inputLineHeightDpi = 0;
	controls::COverlayScrollbar m_overlayScrollbar;
	std::vector<int> m_rowPixelOffsets;
	UINT m_rowPixelOffsetsDpi = 0;
	int m_wheelDeltaRemainder = 0;
	bool m_inputMode = false;
	bool m_suppressInputChange = false;
	std::wstring m_inputPrompt;
	std::wstring m_inputPlaceholder;
	int m_lastSelectableIndex = -1;
	bool m_repairingSelection = false;
	bool m_terminalCallbackInProgress = false;

	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	HBRUSH m_panelBrush = nullptr;
	HBRUSH m_inputBrush = nullptr;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;

	std::vector<CommandPaletteItem> m_items;
	std::wstring m_lastNotifiedSelectionId;
	bool m_selectionNotificationsEnabled = true;
	StringsCallback m_stringsCallback;
	SearchCallback m_searchCallback;
	SelectionCallback m_selectionCallback;
	AcceptCallback m_acceptCallback;
	CancelCallback m_cancelCallback;
};

} // namespace workbench::quickinput
