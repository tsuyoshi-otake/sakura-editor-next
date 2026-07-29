/*! @file */
#pragma once

#include "terminal/model/TerminalModel.h"
#include "theme/CThemeService.h"

#include <Windows.h>

namespace terminal {

enum class TerminalColorRole : std::uint8_t {
	Foreground,
	Background,
};

//! Resolves VT colors against Sakura's active workbench theme.
[[nodiscard]] COLORREF ResolveTerminalColor(
	const TerminalColor& color,
	const theme::ThemePalette& palette,
	COLORREF defaultColor,
	TerminalColorRole role
) noexcept;

//! Resolves a foreground which will actually be painted over `background`.
//! Explicit RGB foregrounds are adjusted only when they would otherwise be
//! unreadable; VT backgrounds and inverse video deliberately use the raw
//! resolver above so programs retain control of their literal canvas colors.
[[nodiscard]] COLORREF ResolveTerminalForeground(
	const TerminalColor& color,
	const theme::ThemePalette& palette,
	COLORREF defaultColor,
	COLORREF background
) noexcept;

} // namespace terminal
