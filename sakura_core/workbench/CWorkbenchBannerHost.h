/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/WorkbenchBannerLayout.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench {

//! What clicking an action does.  The two cases are genuinely different routes,
//! so they are named rather than encoded in whether a command ID happens to be
//! empty: a dismissal is not a command upstream either -- VS Code's banner
//! closes itself and writes `security.workspace.trust.banner` -- and a surface
//! that guessed the route from an empty string would silently swallow a
//! mistyped command ID as "dismiss".
enum class EWorkbenchBannerActionKind : std::uint8_t {
	//! Executes `commandId` through the workbench command spine, the same route
	//! the status-bar item uses.  There is no second dispatch.
	Command,
	//! Dismisses this banner through the host's dismiss callback.
	Dismiss,
};

//! One inline action link at the right end of the banner.
struct WorkbenchBannerAction {
	std::wstring label;
	EWorkbenchBannerActionKind kind = EWorkbenchBannerActionKind::Command;
	//! Stable VS Code command ID.  Required for `Command`, ignored for `Dismiss`.
	std::string commandId;
};

/*!
	@brief Owns the one CEditWnd-child window that paints `workbench.parts.banner`

	This is a projection, not an authority.  It never decides whether the banner
	should be visible and never reads workspace trust: `CWorkbenchRuntime` owns
	that decision and pushes it through the layout state service, and `CEditWnd`
	projects the resulting Part state onto this window.  The host's whole job is
	to measure its own height, draw the content it was handed, and report clicks.

	A `Command` action is only ever handed to this host by a caller that can
	actually execute it.  The host draws exactly the actions it is given, so an
	unperformable action must be withheld by the caller rather than drawn dead.
*/
class CWorkbenchBannerHost final {
public:
	//! Executes a stable command ID.  Always `CEditWnd`'s single workbench
	//! dispatch, never a second route.
	using ExecuteCommandCallback = std::function<void(std::string_view commandId)>;
	//! The user asked for the banner to stop appearing.  The host does not act on
	//! this itself: whether a dismissal can be recorded, and therefore whether the
	//! banner may actually go away, is the runtime's decision.
	using DismissCallback = std::function<void()>;

	CWorkbenchBannerHost() = default;
	~CWorkbenchBannerHost();
	CWorkbenchBannerHost(const CWorkbenchBannerHost&) = delete;
	CWorkbenchBannerHost& operator=(const CWorkbenchBannerHost&) = delete;

	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance);
	void SetExecuteCommandCallback(ExecuteCommandCallback callback) { m_executeCommand = std::move(callback); }
	void SetDismissCallback(DismissCallback callback) { m_dismiss = std::move(callback); }
	//! Replaces message and actions together, because a message describing one
	//! state next to the previous state's actions is worse than either alone.
	void SetContent(std::wstring message, std::vector<WorkbenchBannerAction> actions);
	void SetPalette(const theme::ThemePalette& palette);
	//! Physical height this strip needs at `dpi`, for `CEditWnd` to put into
	//! `WorkbenchLayoutRequest::bannerHeightPixels` before the layout is computed.
	//! Zero when there is no content, so an empty banner reserves nothing.
	[[nodiscard]] int PreferredHeightPixels(unsigned int dpi);
	//! Applies the rectangle the workbench layout produced.  The host never
	//! derives its own position from a sibling window.
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show();
	void Hide();
	void Close();

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }
	[[nodiscard]] bool IsVisible() const noexcept { return m_visible; }
	//! Exposed for the paint-level verification described in the root CLAUDE.md,
	//! which needs a real property of the window to prove the gesture happened.
	[[nodiscard]] const WorkbenchBannerLayout& CurrentLayout() const noexcept { return m_layout; }

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void Paint();
	//! Recomputes `m_layout` from the current bounds, DPI, and measured text.
	void RecalculateLayout();
	void EnsureFont();
	//! Invokes action `index`.  Out-of-range indices are ignored rather than
	//! asserted, because a stale click can outlive a content replacement.
	void InvokeAction(int index);
	void SetHotAction(int index);

	HWND m_window = nullptr;
	bool m_visible = false;
	bool m_closed = false;
	unsigned int m_dpi = 96;
	RECT m_bounds{};
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	std::wstring m_message;
	std::vector<WorkbenchBannerAction> m_actions;
	WorkbenchBannerLayout m_layout;
	//! The action the pointer is over, or -1.  Drawn underlined, like a link.
	int m_hotAction = -1;
	//! The action the primary button went down on, so a press that travels off
	//! the link before release does not fire it.
	int m_pressedAction = -1;
	bool m_trackingMouseLeave = false;
	ExecuteCommandCallback m_executeCommand;
	DismissCallback m_dismiss;
};

} // namespace workbench
