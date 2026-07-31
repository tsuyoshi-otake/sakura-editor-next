/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include <functional>
#include <memory>
#include <optional>

namespace workbench::viewcontainer {

/*!
	@brief One side-bar Part that renders whichever ViewContainer currently lives in it.

	VS Code's Primary Side Bar and Secondary Side Bar are two instances of the same
	composite-bar concept: each shows the single active ViewContainer assigned to it, and
	a container moves between them without being recreated. This host is therefore
	instantiated once per side bar and renders a page borrowed from the shared pool. It
	holds no layout authority: the owner decides which container it shows.
*/
class CViewContainerHost final : public IWorkbenchTool {
public:
	//! Commits a user-originated Outline expansion request to the owning model.
	//! Return false to veto. The callback carries only the requested value, so it stays
	//! independent of HWND and layout/model types.
	using OutlineExpandedCallback = std::function<bool(bool expanded)>;

	explicit CViewContainerHost(std::shared_ptr<CViewContainerPages> pages,
		OutlineExpandedCallback outlineExpanded = {});
	~CViewContainerHost() override;
	CViewContainerHost(const CViewContainerHost&) = delete;
	CViewContainerHost& operator=(const CViewContainerHost&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	//! Destroys this host window only. The shared pages outlive it and must already have
	//! been closed, or detached, by their owner.
	void Close() override;

	void SetPalette(const theme::ThemePalette& palette);

	//! Applies already-committed model state. This never calls the callback.
	void SetOutlineExpanded(bool expanded);
	//! Sends a user request to the owner before changing local/native state.
	[[nodiscard]] bool RequestOutlineExpanded(bool expanded) noexcept;
	void FocusOutline();

	//! Renders one ViewContainer, or the empty state when there is none. This applies an
	//! already-decided model fact and borrows the page from the shared pool.
	void ShowPage(std::optional<ViewContainerPage> page);
	[[nodiscard]] std::optional<ViewContainerPage> ActivePage() const noexcept { return m_page; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept;

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }
	[[nodiscard]] static int OutlineHeaderHeightPixels(unsigned int dpi) noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void LayoutChildren();
	//! Splits the Extensions ViewContainer between its Marketplace and any contributed views.
	void LayoutExtensionsPage(const RECT& client);
	void Paint();
	[[nodiscard]] bool IsOutlineHeaderPoint(POINT point) const noexcept;
	//! True when this host, and not the other side bar, currently owns the page window.
	[[nodiscard]] bool OwnsPage(ViewContainerPage page) const noexcept;

	std::shared_ptr<CViewContainerPages> m_pages;
	OutlineExpandedCallback m_outlineExpandedCallback;
	HWND m_window = nullptr;
	HINSTANCE m_instance = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	RECT m_bounds{};
	RECT m_outlineHeader{};
	unsigned int m_dpi = 96;
	std::optional<ViewContainerPage> m_page;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer
