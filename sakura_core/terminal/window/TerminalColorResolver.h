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

//! Default profile canvas/foreground matching Windows Terminal's included
//! One Half Dark / One Half Light schemes for the active workbench mode.
[[nodiscard]] COLORREF TerminalDefaultBackground(const theme::ThemePalette& palette) noexcept;
[[nodiscard]] COLORREF TerminalDefaultForeground(const theme::ThemePalette& palette) noexcept;

//! Resolves VT colors against Sakura's active workbench theme.
[[nodiscard]] COLORREF ResolveTerminalColor(
	const TerminalColor& color,
	const theme::ThemePalette& palette,
	COLORREF defaultColor,
	TerminalColorRole role
) noexcept;

//! Resolves a foreground without changing the VT program's requested color.
//! `background` is retained for the existing renderer boundary; normal terminal
//! rendering deliberately preserves ANSI and true-color fidelity.
[[nodiscard]] COLORREF ResolveTerminalForeground(
	const TerminalColor& color,
	const theme::ThemePalette& palette,
	COLORREF defaultColor,
	COLORREF background
) noexcept;

} // namespace terminal
