/*! @file
	@brief One owner-drawn row list of the Projects ViewContainer (#411)

	The Projects ViewContainer shows two Views, Projects and Sessions, each as
	a pane with its own row list. This class owns everything such a list does
	the same way in both panes: the LISTBOX itself, the shared overlay
	scrollbar (#407), wheel scrolling, hover text through one tooltip tool,
	the single-click gesture (#412), and the message an empty list shows.
	The page owns what the rows mean, paints them through WM_DRAWITEM, and
	receives LBN_SELCHANGE / LBN_DBLCLK as usual.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/controls/COverlayScrollbar.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace workbench::projects {

//! What a list asks its page about its rows. Every callback is optional.
struct ProjectsListPaneCallbacks final {
	//! Hover text for a row; empty means the row shows no tooltip.
	std::function<std::wstring(std::size_t row)> tooltip;
	//! Whether a single click opens the row (#412).
	std::function<bool(std::size_t row)> opensOnSingleClick;
	//! A release on the row the press began on, once the list selected it.
	std::function<void(std::size_t row)> clicked;
	//! WM_CONTEXTMENU, with its screen position or -1 for the keyboard.
	std::function<void(LPARAM position)> contextMenu;
	//! A key the page handles itself; true keeps it from the list.
	std::function<bool(WPARAM key)> keyDown;
};

class CProjectsListPane final {
public:
	CProjectsListPane() = default;
	~CProjectsListPane();
	CProjectsListPane(const CProjectsListPane&) = delete;
	CProjectsListPane& operator=(const CProjectsListPane&) = delete;

	//! Creates the hidden LISTBOX child of `parent` with `controlId`, its
	//! overlay scrollbar, and one TTF_IDISHWND tool on `tooltip`. A null
	//! tooltip or a failed overlay leaves a list without that feature.
	[[nodiscard]] bool Create(HWND parent, int controlId, HINSTANCE instance, HWND tooltip,
		ProjectsListPaneCallbacks callbacks) noexcept;
	//! Removes the tooltip tool, the overlay, and the list. Safe to repeat.
	void Destroy() noexcept;

	[[nodiscard]] HWND Handle() const noexcept { return m_list; }

	//! Replaces every row with its accessible label and selects `selected`.
	//! A press in progress is dropped, because its row index may now name
	//! another row.
	void SetRows(std::span<const std::wstring> labels, std::optional<std::size_t> selected) noexcept;
	[[nodiscard]] std::optional<std::size_t> Selection() const noexcept;
	void Select(std::size_t row) noexcept;

	//! Moves the list within its parent's client area without painting.
	void SetBounds(const RECT& bounds) noexcept;
	void SetVisible(bool visible) noexcept;
	void ApplyDpi(unsigned int dpi, HFONT font, int rowHeight) noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	//! Text an empty list paints instead of rows, such as "Loading...".
	void SetEmptyMessage(std::wstring message) noexcept;

	//! Re-reads the scroll model and places the overlay over the list.
	void UpdateScrollbar() noexcept;
	//! Re-resolves the hovered row after the rows or the scroll position moved.
	void HoverRowUnderCursor() noexcept;
	void Invalidate() noexcept;

private:
	static LRESULT CALLBACK SubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	[[nodiscard]] TOOLINFOW Tool() const noexcept;
	//! The row whose painted rectangle contains `point`, in list coordinates.
	//! LB_ITEMFROMPOINT alone answers the nearest row below the last one.
	[[nodiscard]] std::optional<std::size_t> RowAt(POINT point) const noexcept;
	[[nodiscard]] std::optional<std::size_t> SingleClickRowAt(POINT point) const noexcept;
	void HoverRowAt(POINT point) noexcept;
	void SetTooltipText(const std::wstring& text) noexcept;
	[[nodiscard]] bool PaintEmptyMessage() noexcept;

	ProjectsListPaneCallbacks m_callbacks;
	controls::COverlayScrollbar m_scrollbar;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	std::wstring m_emptyMessage;
	std::optional<std::size_t> m_hoveredRow;
	//! The row a primary press began on, when a single click opens it (#412).
	std::optional<std::size_t> m_pressedRow;
	HWND m_parent = nullptr;
	HWND m_list = nullptr;
	HWND m_tooltip = nullptr;
	HFONT m_font = nullptr;
	unsigned int m_dpi = 96;
	bool m_toolAdded = false;
};

} // namespace workbench::projects
