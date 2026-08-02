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

struct TerminalRenderDefaults final {
	COLORREF background{};
	COLORREF foreground{};
};

//! Default terminal viewport background uses the projected VS Code
//! `terminal.background` role, which falls back to Panel background when absent.
//! The foreground keeps the Windows Terminal One Half profile behavior.
[[nodiscard]] COLORREF TerminalDefaultBackground(const theme::ThemePalette& palette) noexcept;
[[nodiscard]] COLORREF TerminalDefaultForeground(const theme::ThemePalette& palette) noexcept;

//! Selects the colors used to fill a terminal viewport and render default text.
//! High Contrast disables the One Half foreground profile, but it must not bypass
//! the projected `terminal.background` role for the viewport fill.
[[nodiscard]] TerminalRenderDefaults ResolveTerminalRenderDefaults(
	const theme::ThemePalette& palette,
	bool useTerminalProfileColors
) noexcept;

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
