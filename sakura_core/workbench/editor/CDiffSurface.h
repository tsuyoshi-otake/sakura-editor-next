/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "window/CWnd.h"

#include <functional>
#include <string>
#include <vector>

//! One rendered row of a side-by-side diff.
//!
//! This mirrors `workbench::scm::GitDiffViewRow`, but the surface keeps its own
//! presentation-neutral copy so that the editor subtree never depends on the SCM
//! subtree. The composition root translates one into the other.
struct SDiffSurfaceRow final {
	//! True when the row belongs to a changed region on either side.
	bool changed = false;
	//! 1-based original line number, or 0 when the original side has no line here.
	int originalLineNumber = 0;
	//! 1-based modified line number, or 0 when the modified side has no line here.
	int modifiedLineNumber = 0;
};

//! Everything the surface needs to render one comparison.
struct SDiffSurfaceContent final {
	//! Editor title, built the same way VS Code builds it: `name (left ↔ right)`.
	std::wstring title;
	//! Column captions, e.g. `HEAD` and `Working Tree`.
	std::wstring originalLabel;
	std::wstring modifiedLabel;
	std::vector<std::wstring> originalLines;
	std::vector<std::wstring> modifiedLines;
	std::vector<SDiffSurfaceRow> rows;
	//! True when the line-diff computation stopped at its bound, so the rendered
	//! alignment is a truncated approximation rather than the complete diff.
	bool truncated = false;
};

//! Typed, native side-by-side diff surface.
//!
//! The surface owns a **row** selection, not a text selection. VS Code's diff
//! editor selects characters inside the modified side; a plain GDI surface has
//! no caret and no text measurement per glyph, and staging can only ever act on
//! whole lines anyway (upstream widens every selection to whole lines in
//! `toLineRanges` before it stages). The selection is therefore a contiguous
//! range of rendered rows, spanning both columns because the rows are aligned.
//! The surface reports only which rows are selected; translating those rows into
//! line numbers on either side belongs to the composition root, which is the
//! only place allowed to know what a line means to git.
//!
//! This is a native composition-layer projection in exactly the same sense as
//! `CExtensionDetailSurface`: it is not an `EditorInput`, it owns no document
//! model, and `CEditWnd` may show it only while the native editor has no active
//! document. It renders content handed to it and never reads files or runs git.
class CDiffSurface final : public CWnd
{
public:
	using CloseRequestedCallback = std::function<void()>;

	explicit CDiffSurface();
	~CDiffSurface() override;

	CDiffSurface(const CDiffSurface&) = delete;
	CDiffSurface& operator=(const CDiffSurface&) = delete;

	HWND Open(HINSTANCE hInstance, HWND hwndParent);
	void Destroy() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show() noexcept;
	void Hide() noexcept;
	void Focus() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept { return CWnd::GetHwnd(); }

	void ShowDiff(SDiffSurfaceContent content);
	void ClearDiff();
	[[nodiscard]] bool HasDiff() const noexcept { return m_hasContent; }
	//! True when at least one row is selected.
	[[nodiscard]] bool HasSelection() const noexcept;
	//!
	//! @brief The selected rows, as inclusive indices into `SDiffSurfaceContent::rows`.
	//!
	//! False when nothing is selected, and then neither output is written.
	//!
	[[nodiscard]] bool SelectedRowRange(int& firstRow, int& lastRow) const noexcept;
	void ClearSelection() noexcept;
	//!
	//! @brief The comparison currently rendered.
	//!
	//! The rows are what a selection indexes into, so the composition root needs
	//! them to answer which lines a selection names. Exposing the content it was
	//! given is not a second authority: the surface is the only holder, and a
	//! caller that kept its own copy could describe a comparison the screen has
	//! already replaced.
	//!
	[[nodiscard]] const SDiffSurfaceContent& Content() const noexcept { return m_content; }
	[[nodiscard]] const std::wstring& Title() const noexcept { return m_content.title; }
	void SetOnCloseRequested(CloseRequestedCallback callback);
	//! Returns true when hwndControl is this surface's close-affordance button.
	[[nodiscard]] bool IsCloseButton(HWND hwndControl) const noexcept
	{
		return hwndControl != nullptr && hwndControl == m_hwndClose;
	}

	LRESULT DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override;

private:
	static constexpr int kCloseButtonId = 0x5201;

	[[nodiscard]] unsigned int Dpi() const noexcept;
	[[nodiscard]] int ScaleDip(int dip) const noexcept;
	void LayoutChildren();
	void EnsureFont();
	void ReleaseFont() noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;
	void DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept;
	void Paint();
	void PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold = false);
	//! Paints one side of the comparison inside `column`, clipped to the viewport.
	void PaintSide(HDC dc, const RECT& column, int headerBottom, bool original);
	[[nodiscard]] int RowHeight() const noexcept;
	[[nodiscard]] int ContentTop() const noexcept;
	[[nodiscard]] int GutterWidth(HDC dc) const;
	void UpdateScrollRange();
	void ScrollTo(int offset) noexcept;
	void InvokeClose();
	//! The row under a client-area y coordinate, or -1 when the point is outside the rows.
	[[nodiscard]] int RowAtPoint(int y) const noexcept;
	[[nodiscard]] bool IsRowSelected(int row) const noexcept;
	//! Moves the selection's focus row, extending from the anchor when `extend`.
	void MoveSelectionTo(int row, bool extend);
	void EnsureRowVisible(int row);

	SDiffSurfaceContent m_content;
	CloseRequestedCallback m_onCloseRequested;
	HWND m_hwndClose = nullptr;
	HFONT m_font = nullptr;
	HFONT m_boldFont = nullptr;
	//! Fixed-pitch text uses the `Editor` font role, exactly as the diff editor does upstream.
	theme::CThemeFont m_codeFont;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;
	int m_rowHeight = 0;
	bool m_hasContent = false;
	bool m_focused = false;
	int m_scrollOffset = 0;
	int m_contentHeight = 0;
	int m_maxScrollOffset = 0;
	//! Both -1 when nothing is selected. The anchor is where the gesture began.
	int m_selectionAnchorRow = -1;
	int m_selectionFocusRow = -1;
	bool m_selecting = false;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
};
