/*! @file
	@brief VS Code互換のCommand Palette/Quick Pick用Quick Inputオーバーレイ
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"

#include <Windows.h>

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
	std::wstring detail;
	bool enabled = true;
};

//! Localized strings supplied by the currently active Quick Input surface.
struct QuickInputStrings {
	std::wstring placeholder;
	std::wstring noResults;
};

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
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	void Layout(int width, int height) noexcept;
	void PopulateList(std::wstring_view preferredSelectionId = {}) noexcept;
	void UpdateSearch() noexcept;
	void MoveSelection(int direction) noexcept;
	void NotifySelectionChanged() noexcept;
	[[nodiscard]] std::wstring SelectedItemId() const;
	void Accept() noexcept;
	void RestoreFocus() noexcept;
	void Paint(HDC dc, const RECT& bounds) noexcept;
	void DrawItem(const DRAWITEMSTRUCT& draw) noexcept;
	void DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept;
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
