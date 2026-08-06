/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "window/CWnd.h"

#include <cstdint>
#include <memory>
#include <string>

namespace markdown {
class CMarkdownPreviewWnd;
}

//! Native, non-activating hover popup for extension-provided `Hover.contents`.
//!
//! Hosts the shared native Markdown preview (`markdown::CMarkdownPreviewWnd`)
//! -- the same renderer `CExtensionDetailSurface` uses for Marketplace
//! READMEs (`sakura_core/workbench/editor/CExtensionDetailSurface.cpp`) --
//! inside a bounded `WS_POPUP` window. The window style follows this
//! codebase's existing non-activating popup precedent, the dictionary tip
//! (`CTipWnd`, `sakura_core/window/CTipWnd.h`): `WS_EX_TOOLWINDOW` so the
//! popup never appears in the taskbar or Alt+Tab, plus `WS_EX_NOACTIVATE` and
//! `SW_SHOWNA` so showing it never steals focus or activation from the
//! editor view that triggered it.
//!
//! This class never fetches remote content and never parses Markdown itself.
//! It only queues already-merged Markdown text (see
//! `CExtensionService::HandleHoverResponseWorker` and
//! `CExtensionHoverCenter` in `sakura_core/extension/CExtensionWorkbenchUi.h`)
//! through `QueueDocument` with an empty `documentPath`/`workspaceRoot`, so
//! every resource reference inside hover content resolves as
//! `ResourceDisposition::ExternalBlocked` -- the identical fail-closed rule
//! `CExtensionDetailSurface` applies to a Marketplace README (see
//! "Marketplace README rendering" in
//! `sakura_core/workbench/editor/CLAUDE.md`).
//!
//! **Documented divergence (bounded fixed size, not natural-content size):**
//! `CMarkdownPreviewWnd` has no API to measure a parsed document's natural
//! size ahead of layout -- unlike VS Code's DOM-based hover widget, which
//! grows to its content up to a viewport-relative maximum. This popup is
//! therefore always laid out at a fixed maximum size
//! (`kMaxWidthDip` x `kMaxHeightDip`) and relies on the preview child's own
//! internal scrollbar for content that does not fit, rather than an
//! intrinsic-size measurement pass this repository does not have yet.
class CEditViewHoverPopup final : public CWnd
{
public:
	CEditViewHoverPopup();
	~CEditViewHoverPopup() override;

	CEditViewHoverPopup(const CEditViewHoverPopup&) = delete;
	CEditViewHoverPopup& operator=(const CEditViewHoverPopup&) = delete;

	//! Creates the popup as an owned top-level window of `hwndOwner`. Returns
	//! false when the window class or window creation fails, or when this
	//! popup is already created. `hwndOwner` follows `CTipWnd::Create`'s own
	//! existing convention: pass the owning `CEditView`'s own `GetHwnd()`
	//! (not the outer frame), so the popup is minimized/restored/destroyed
	//! together with the view that owns it.
	[[nodiscard]] bool Create(HINSTANCE hInstance, HWND hwndOwner);
	//! Destroys the popup window and joins the preview child's worker thread.
	//! Safe to call when not created.
	void Destroy() noexcept;

	//! Renders `markdown` and shows the popup near `screenAnchor` (typically
	//! the screen position of the hovered word), clamped to stay fully inside
	//! the anchor's monitor working area. An empty or whitespace-only
	//! `markdown` hides the popup instead of showing an empty box -- the same
	//! "blank content is a real answer, not a failure" rule
	//! `CExtensionDetailSurface::PublishReadme` applies to a blank README.
	//! Every call takes a new internal render generation, so a superseded
	//! `QueueDocument` completion can never paint over a newer request's
	//! content; callers do not need to track that ordering themselves.
	void ShowMarkdown(const std::wstring& markdown, POINT screenAnchor, unsigned int dpi);
	//! Hides the popup without destroying it. Safe to call repeatedly and
	//! when not created.
	void Hide() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept { return CWnd::GetHwnd(); }

protected:
	LRESULT OnSize(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override;

private:
	static constexpr int kMaxWidthDip = 460;
	static constexpr int kMaxHeightDip = 300;
	//! Offset from the anchor point so the popup does not sit directly under
	//! the pointer/word it describes.
	static constexpr int kAnchorOffsetXDip = 12;
	static constexpr int kAnchorOffsetYDip = 20;
	static constexpr std::size_t kMaxMarkdownCharacters = 32 * 1024;

	void LayoutPreview() noexcept;
	[[nodiscard]] static bool IsBlank(const std::wstring& markdown) noexcept;

	std::unique_ptr<markdown::CMarkdownPreviewWnd> m_preview;
	std::uint64_t m_renderGeneration = 0;
	//! DPI captured from the most recent ShowMarkdown call. LayoutPreview uses
	//! this instead of always assuming 96, since this popup is created once
	//! but may be shown on any monitor. It is not live-updated by
	//! WM_DPICHANGED: the popup is transient and is always fully re-laid-out
	//! by the next ShowMarkdown call.
	unsigned int m_dpi = 96;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
};
