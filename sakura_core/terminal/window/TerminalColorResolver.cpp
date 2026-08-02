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
	static constexpr std::array<COLORREF, 16> oneHalfDark{
		RGB(0x28, 0x2C, 0x34), RGB(0xE0, 0x6C, 0x75), RGB(0x98, 0xC3, 0x79), RGB(0xE5, 0xC0, 0x7B),
		RGB(0x61, 0xAF, 0xEF), RGB(0xC6, 0x78, 0xDD), RGB(0x56, 0xB6, 0xC2), RGB(0xDC, 0xDF, 0xE4),
		RGB(0x5A, 0x63, 0x74), RGB(0xE0, 0x6C, 0x75), RGB(0x98, 0xC3, 0x79), RGB(0xE5, 0xC0, 0x7B),
		RGB(0x61, 0xAF, 0xEF), RGB(0xC6, 0x78, 0xDD), RGB(0x56, 0xB6, 0xC2), RGB(0xDC, 0xDF, 0xE4),
	};
	static constexpr std::array<COLORREF, 16> oneHalfLight{
		RGB(0x38, 0x3A, 0x42), RGB(0xE4, 0x56, 0x49), RGB(0x50, 0xA1, 0x4F), RGB(0xC1, 0x83, 0x01),
		RGB(0x01, 0x84, 0xBC), RGB(0xA6, 0x26, 0xA4), RGB(0x09, 0x97, 0xB3), RGB(0xFA, 0xFA, 0xFA),
		RGB(0x4F, 0x52, 0x5D), RGB(0xDF, 0x6C, 0x75), RGB(0x98, 0xC3, 0x79), RGB(0xE4, 0xC0, 0x7A),
		RGB(0x61, 0xAF, 0xEF), RGB(0xC5, 0x77, 0xDD), RGB(0x56, 0xB5, 0xC1), RGB(0xFF, 0xFF, 0xFF),
	};
	const auto& base = IsDarkTheme(palette) ? oneHalfDark : oneHalfLight;
	if( index < base.size() ) return base[index];
	if( index < 232 ) {
		const auto cube = index - 16;
		const auto component = [](unsigned int value) { return value == 0 ? 0u : 55u + 40u * value; };
		return RGB(component(cube / 36), component((cube / 6) % 6), component(cube % 6));
	}
	const auto gray = std::min(255u, 8u + 10u * (index - 232));
	return RGB(gray, gray, gray);
}

} // namespace

COLORREF TerminalDefaultBackground( const theme::ThemePalette& palette ) noexcept
{
	return palette.terminalBackground.ToColorRef();
}

COLORREF TerminalDefaultForeground( const theme::ThemePalette& palette ) noexcept
{
	return IsDarkTheme(palette) ? RGB(0xDC, 0xDF, 0xE4) : RGB(0x38, 0x3A, 0x42);
}

TerminalRenderDefaults ResolveTerminalRenderDefaults(
	const theme::ThemePalette& palette,
	bool useTerminalProfileColors
) noexcept
{
	return {
		TerminalDefaultBackground(palette),
		useTerminalProfileColors ? TerminalDefaultForeground(palette) : palette.primaryText.ToColorRef(),
	};
}

COLORREF ResolveTerminalColor( const TerminalColor& color, const theme::ThemePalette& palette, COLORREF defaultColor,
	TerminalColorRole role ) noexcept
{
	static_cast<void>(role);
	switch( color.kind ) {
	case TerminalColorKind::Indexed: return IndexedColor(color.value & 0xff, palette);
	case TerminalColorKind::Rgb: return RGB((color.value >> 16) & 0xff, (color.value >> 8) & 0xff, color.value & 0xff);
	case TerminalColorKind::Default: return defaultColor;
	}
	return defaultColor;
}

COLORREF ResolveTerminalForeground( const TerminalColor& color, const theme::ThemePalette& palette, COLORREF defaultColor,
	COLORREF background ) noexcept
{
	static_cast<void>(background);
	return ResolveTerminalColor(color, palette, defaultColor, TerminalColorRole::Foreground);
}

} // namespace terminal
