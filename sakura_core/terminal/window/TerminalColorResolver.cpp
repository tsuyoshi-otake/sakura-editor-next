/*! @file */
#include "StdAfx.h"
#include "terminal/window/TerminalColorResolver.h"

#include <algorithm>
#include <array>

namespace terminal {
namespace {

bool IsDarkTheme( const theme::ThemePalette& palette ) noexcept
{
	const auto& canvas = palette.canvas;
	return static_cast<unsigned int>(canvas.red) * 299 + static_cast<unsigned int>(canvas.green) * 587 +
		static_cast<unsigned int>(canvas.blue) * 114 < 128000;
}

COLORREF IndexedColor( unsigned int index, const theme::ThemePalette& palette ) noexcept
{
	static constexpr std::array<COLORREF, 16> base{
		RGB(0, 0, 0), RGB(205, 49, 49), RGB(13, 188, 121), RGB(229, 229, 16),
		RGB(36, 114, 200), RGB(188, 63, 188), RGB(17, 168, 205), RGB(229, 229, 229),
		RGB(102, 102, 102), RGB(241, 76, 76), RGB(35, 209, 139), RGB(245, 245, 67),
		RGB(59, 142, 234), RGB(214, 112, 214), RGB(41, 184, 219), RGB(255, 255, 255),
	};
	if( index < base.size() ) {
		// ConsoleColor.Black is frequently emitted as SGR 30 by PowerShell.
		// A literal black foreground disappears against the workbench canvas, so
		// bind the neutral ANSI colors to the active theme's readable neutrals.
		// #808794 is still the darkest neutral in the dark terminal palette while
		// retaining a WCAG contrast ratio above 4.5:1 against Sakura's #181A1F canvas.
		if( index == 0 ) return IsDarkTheme(palette) ? RGB(128, 135, 148) : RGB(31, 35, 41);
		if( index == 7 ) return palette.primaryText.ToColorRef();
		if( index == 8 ) return palette.secondaryText.ToColorRef();
		if( index == 15 ) return palette.highlightText.ToColorRef();
		return base[index];
	}
	if( index < 232 ) {
		const auto cube = index - 16;
		const auto component = [](unsigned int value) { return value == 0 ? 0u : 55u + 40u * value; };
		return RGB(component(cube / 36), component((cube / 6) % 6), component(cube % 6));
	}
	const auto gray = std::min(255u, 8u + 10u * (index - 232));
	return RGB(gray, gray, gray);
}

} // namespace

COLORREF ResolveTerminalColor( const TerminalColor& color, const theme::ThemePalette& palette, COLORREF defaultColor ) noexcept
{
	switch( color.kind ) {
	case TerminalColorKind::Indexed: return IndexedColor(color.value & 0xff, palette);
	case TerminalColorKind::Rgb: return RGB((color.value >> 16) & 0xff, (color.value >> 8) & 0xff, color.value & 0xff);
	case TerminalColorKind::Default: return defaultColor;
	}
	return defaultColor;
}

} // namespace terminal
