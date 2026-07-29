/*! @file */
#pragma once

#include "terminal/model/TerminalModel.h"
#include "theme/CThemeService.h"

#include <Windows.h>

namespace terminal {

//! Resolves VT colors against Sakura's active workbench theme.
[[nodiscard]] COLORREF ResolveTerminalColor(
	const TerminalColor& color,
	const theme::ThemePalette& palette,
	COLORREF defaultColor
) noexcept;

} // namespace terminal
