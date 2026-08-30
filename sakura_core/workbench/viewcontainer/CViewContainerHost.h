/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::viewcontainer {

enum class EViewContainerHostPageStatus : std::uint8_t {
	Applied,
	AlreadyApplied,
	Cleared,
	UnknownContainer,
	InvalidHost,
	DetachFailed,
	AttachFailed,
	CompensationFailed,
};

enum class EViewContainerHostCloseStatus : std::uint8_t {
	Open,
	Closed,
	DetachedPagePreserved,
	DetachedPagePreservationFailed,
};

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
	//! Runs only after a retained Outline View has been laid out, shown, and painted.
	//! Parsing is deliberately outside the expansion-model callback so cached content can
	//! appear synchronously before document-version refresh work starts.
	using OutlineRevealCallback = std::function<void()>;

	explicit CViewContainerHost(std::shared_ptr<CViewContainerPages> pages,
		std::string logicalHostId,
		OutlineExpandedCallback outlineExpanded = {}, OutlineRevealCallback outlineRevealed = {});
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

	//! Renders one ViewContainer, or the empty state when `containerId` is empty. Unknown
	//! containers and transition failures leave the prior projection unchanged. This applies
	//! an already-decided model fact and borrows the page from the shared pool.
	EViewContainerHostPageStatus ShowPage(std::string_view containerId);
	//! The ViewContainer this Part renders, or empty for the empty state. The view points at
	//! this host's own storage and stays valid until the next `ShowPage`.
	[[nodiscard]] std::string_view ActivePage() const noexcept { return m_page; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept;
	[[nodiscard]] EViewContainerHostCloseStatus CloseStatus() const noexcept
	{
		return m_closeStatus;
	}

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }
	[[nodiscard]] static int OutlineHeaderHeightPixels(unsigned int dpi) noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void LayoutChildren();
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;
	void Paint();
	//! Draws an inset, vertically-centered, word-wrapped status message over the whole
	//! client area, in the secondary text color.
	void DrawCenteredMessage(HDC dc, std::wstring_view message) const;
	void NotifyOutlineRevealed() noexcept;
	[[nodiscard]] bool IsOutlineHeaderPoint(POINT point) const noexcept;
	//! True when this host, and not the other side bar, currently owns the page window.
	[[nodiscard]] bool OwnsPage(std::string_view containerId) const noexcept;
	[[nodiscard]] std::optional<ViewContainerPageHost> PageHost() const noexcept;
	[[nodiscard]] bool FinalStateOwnsHost(
		const std::optional<ViewContainerPageState>& state) const noexcept;
	void ProjectActualPage(std::string& desired, std::string& previous,
		bool desiredAttached, bool previousAttached) noexcept;

	std::shared_ptr<CViewContainerPages> m_pages;
	std::string m_logicalHostId;
	OutlineExpandedCallback m_outlineExpandedCallback;
	OutlineRevealCallback m_outlineRevealCallback;
	HWND m_window = nullptr;
	HINSTANCE m_instance = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	RECT m_bounds{};
	RECT m_outlineHeader{};
	unsigned int m_dpi = 96;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;
	rendering::CGdiBackBuffer m_backBuffer;
	//! The rendered ViewContainer's ID, empty for none.
	std::string m_page;
	EViewContainerHostCloseStatus m_closeStatus{ EViewContainerHostCloseStatus::Open };
	bool m_closed = false;
};

} // namespace workbench::viewcontainer
