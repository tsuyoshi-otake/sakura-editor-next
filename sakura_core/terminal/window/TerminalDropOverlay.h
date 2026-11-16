/*! @file
 * @brief Translucent drop-feedback window of the terminal panel's drag and drop.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "theme/CThemeService.h"

#include <Windows.h>

namespace terminal {

//! One click-through, non-activating popup that tints a screen rectangle with a
//! translucent colour (#415 Phase 6). The terminal panes are child HWNDs that the
//! tool window cannot paint over, so drop feedback is a separate layered window
//! owned by the top-level frame: it stays above the frame, never takes focus or
//! mouse input, and hiding it leaves nothing behind in the panes it covered.
class TerminalDropOverlay final {
public:
	TerminalDropOverlay() = default;
	~TerminalDropOverlay();
	TerminalDropOverlay( const TerminalDropOverlay& ) = delete;
	TerminalDropOverlay& operator=( const TerminalDropOverlay& ) = delete;

	//! Shows the overlay over `screenRect` in `color`, honouring its alpha (an
	//! alpha of 0 shows nothing). The window is created on first use, owned by
	//! `anchor`'s top-level window. An empty rectangle hides the overlay.
	//! Returns false when the window could not be created.
	bool Show( HWND anchor, HINSTANCE instance, const RECT& screenRect, theme::ThemeColor color );
	void Hide() noexcept;
	void Destroy() noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;
	//! The rectangle last shown, in screen coordinates; empty while hidden.
	[[nodiscard]] RECT ShownRect() const noexcept;

private:
	HWND m_window{};
	RECT m_shown{};
	COLORREF m_color{};
	BYTE m_alpha{};
};

} // namespace terminal
