/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "theme/CThemeService.h"
#include "window/CWnd.h"
#include "workbench/IWorkbenchRuntime.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

//! Native Workspace Trust *editor page*, VS Code's `workbench.trust.manage`
//! target rendered as a first-class editor surface instead of a modal dialog.
//!
//! This is a typed native projection of an already-resolved
//! `workbench::WorkspaceTrustPromptModel`. It never calls into
//! `IWorkbenchRuntime` itself: it does not resolve trust, does not decide
//! which resource a grant would name, and does not re-parse `displayUri`. The
//! composition root supplies the model through `ShowPrompt`, receives a grant
//! request through `SetOnGrantRequested`, performs the grant against the
//! runtime, and reports the terminal outcome back through `SetGrantResult` --
//! the same request/report split used by other composition-layer surfaces for
//! install/close, adapted to a policy decision that must never be taken here.
//!
//! This is a native composition-layer surface,
//! not an `EditorInput` and not a second document model. `CEditWnd` may show it
//! only while the native editor has no active document, and must hide it
//! before projecting a document. It uses native GDI painting and native
//! `WC_BUTTONW` children only: no WebView2, no browser engine, no HTML.
class CWorkspaceTrustEditorSurface final : public CWnd
{
public:
	using GrantRequestedCallback = std::function<void(workbench::EWorkspaceTrustGrantScope)>;
	using CloseRequestedCallback = std::function<void()>;

	explicit CWorkspaceTrustEditorSurface();
	~CWorkspaceTrustEditorSurface() override;

	CWorkspaceTrustEditorSurface(const CWorkspaceTrustEditorSurface&) = delete;
	CWorkspaceTrustEditorSurface& operator=(const CWorkspaceTrustEditorSurface&) = delete;

	HWND Open(HINSTANCE hInstance, HWND hwndParent);
	void Destroy() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show() noexcept;
	void Hide() noexcept;
	void Focus() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept { return CWnd::GetHwnd(); }

	//! Project an already-resolved model. The page never re-resolves trust and
	//! never re-parses displayUri to decide anything. Any previously reported
	//! grant result is cleared: a fresh model describes the current state, and
	//! a stale outcome banner from a prior prompt would misdescribe it.
	void ShowPrompt(workbench::WorkspaceTrustPromptModel model);
	void ClearPrompt();
	[[nodiscard]] bool HasPrompt() const noexcept { return m_hasPrompt; }
	//! Report the terminal outcome of a grant the host performed, so the page
	//! shows what actually happened instead of assuming success.
	void SetGrantResult(workbench::WorkspaceTrustGrantResult result);

	void SetOnGrantRequested(GrantRequestedCallback callback);
	void SetOnCloseRequested(CloseRequestedCallback callback);
	//! Returns true when hwndControl is this surface's close-affordance button.
	[[nodiscard]] bool IsCloseButton(HWND hwndControl) const noexcept { return hwndControl != nullptr && hwndControl == m_hwndClose; }

	LRESULT DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override;

private:
	//! One grant button's native window plus the exact model data it renders.
	//! `heading` is the short command-link-style lead line ("Trust the authors
	//! of all files in the parent folder", etc); `option.displayUri` is drawn
	//! beneath it verbatim, because consent must name the resource, not a
	//! category. Keeping both on the struct (rather than re-deriving the
	//! heading from `option.scope`/`option.resourceCount` at paint time) means
	//! the owner-draw handler and the button-creation code can never disagree
	//! about what a given button says.
	struct SGrantButton {
		HWND hwnd = nullptr;
		int id = 0;
		workbench::WorkspaceTrustGrantOption option;
		std::wstring heading;
	};

	//! Grant button IDs start at kGrantButtonBaseId. kCloseButtonId sits inside
	//! that same small ID block, so an index that would collide with it is
	//! pushed one past instead: index 0 is kGrantButtonBaseId (0x5201), index 1
	//! is kGrantButtonBaseId + 2 (0x5203), never kCloseButtonId itself. At most
	//! two grant options are ever offered (CurrentWorkspace, ParentFolder), so
	//! this reservation is sufficient without a wider ID range.
	static constexpr int kGrantButtonBaseId = 0x5201;
	static constexpr int kCloseButtonId = 0x5202;

	[[nodiscard]] unsigned int Dpi() const noexcept;
	[[nodiscard]] int ScaleDip(int dip) const noexcept;
	void EnsureFont();
	void ReleaseFont() noexcept;
	[[nodiscard]] int GrantButtonId(std::size_t index) const noexcept;
	void RebuildGrantButtons();
	void DestroyGrantButtons() noexcept;
	void DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept;
	//! Returns true and fills the button's paint colors when hwndItem matches
	//! one of m_grantButtons; false (nothing drawn) otherwise, mirroring
	//! DrawIconBitmap's typed "did this actually apply" contract.
	[[nodiscard]] bool DrawGrantButton(const DRAWITEMSTRUCT& draw) noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;
	void LayoutChildren();
	//! Lays out the fixed header (title + close button) and returns the y
	//! where the body begins. A null dc measures without drawing, exactly like
	//! the shared header painter, so the paint pass and the
	//! child-layout pass cannot disagree about where the header ends.
	[[nodiscard]] int PaintHeader(HDC dc, const RECT& client);
	//! Lays out and optionally paints the body: main instruction, description,
	//! the explicit empty/unavailable/trusted states, and (when a grant can be
	//! offered) the grant button rectangles. `buttonRects` is always filled
	//! with the current button geometry, independent of whether dc is null, so
	//! LayoutChildren and Paint share one source of truth for where the
	//! buttons live.
	void PaintBody(HDC dc, const RECT& client, int top, std::vector<RECT>& buttonRects);
	void PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold = false);
	void Paint();
	void InvokeGrant(workbench::EWorkspaceTrustGrantScope scope);
	void InvokeClose();
	//! True while a grant can actually be offered: not already Trusted, the
	//! durable list is writable, and there is at least one resource to name.
	[[nodiscard]] bool OffersGrant() const noexcept;

	workbench::WorkspaceTrustPromptModel m_promptModel;
	bool m_hasPrompt = false;
	std::optional<workbench::WorkspaceTrustGrantResult> m_grantResult;
	std::vector<SGrantButton> m_grantButtons;
	GrantRequestedCallback m_onGrantRequested;
	CloseRequestedCallback m_onCloseRequested;
	HWND m_hwndClose = nullptr;
	HFONT m_font = nullptr;
	HFONT m_boldFont = nullptr;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;
	bool m_focused = false;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
};
