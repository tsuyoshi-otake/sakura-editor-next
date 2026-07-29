/*! @file */
#include "StdAfx.h"
#include "terminal/window/TerminalColorResolver.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace terminal {
namespace {

bool IsDarkTheme( const theme::ThemePalette& palette ) noexcept
{
	const auto& canvas = palette.canvas;
	return static_cast<unsigned int>(canvas.red) * 299 + static_cast<unsigned int>(canvas.green) * 587 +
		static_cast<unsigned int>(canvas.blue) * 114 < 128000;
}

double LinearComponent( unsigned int component ) noexcept
{
	const auto normalized = static_cast<double>(component) / 255.0;
	return normalized <= 0.04045 ? normalized / 12.92 : std::pow((normalized + 0.055) / 1.055, 2.4);
}

double RelativeLuminance( COLORREF color ) noexcept
{
	return 0.2126 * LinearComponent(GetRValue(color)) + 0.7152 * LinearComponent(GetGValue(color)) + 0.0722 * LinearComponent(GetBValue(color));
}

double ContrastRatio( COLORREF first, COLORREF second ) noexcept
{
	const auto firstLuminance = RelativeLuminance(first);
	const auto secondLuminance = RelativeLuminance(second);
	return (std::max(firstLuminance, secondLuminance) + 0.05) / (std::min(firstLuminance, secondLuminance) + 0.05);
}

COLORREF Blend( COLORREF from, COLORREF to, double amount ) noexcept
{
	const auto component = [amount](BYTE left, BYTE right) {
		return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(left + (right - left) * amount)), 0, 255));
	};
	return RGB(component(GetRValue(from), GetRValue(to)), component(GetGValue(from), GetGValue(to)), component(GetBValue(from), GetBValue(to)));
}

COLORREF EnsureReadableRgbForeground( COLORREF foreground, COLORREF background ) noexcept
{
	constexpr double kMinimumContrast = 4.5;
	if( ContrastRatio(foreground, background) >= kMinimumContrast ) return foreground;
	const COLORREF target = RelativeLuminance(background) < 0.5 ? RGB(255, 255, 255) : RGB(0, 0, 0);
	// Binary search preserves as much of the program-selected hue as possible
	// while making the text legible. The target always provides enough contrast
	// against the chosen side of the luminance range.
	double low = 0.0;
	double high = 1.0;
	for( int iteration = 0; iteration < 12; ++iteration ) {
		const auto middle = (low + high) / 2.0;
		if( ContrastRatio(Blend(foreground, target, middle), background) >= kMinimumContrast ) high = middle;
		else low = middle;
	}
	return Blend(foreground, target, high);
}

COLORREF IndexedColor( unsigned int index, const theme::ThemePalette& palette, TerminalColorRole role ) noexcept
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
		// retaining a WCAG contrast ratio above 4.5:1 against Sakura's #1E1E1E canvas.
		// Readability remapping applies only to glyphs. Background colors are VT
		// canvas data and must retain their literal ANSI values (notably SGR 40).
		if( role == TerminalColorRole::Foreground ) {
			if( index == 0 ) return IsDarkTheme(palette) ? RGB(128, 135, 148) : RGB(31, 35, 41);
			if( index == 7 ) return palette.primaryText.ToColorRef();
			if( index == 8 ) return palette.secondaryText.ToColorRef();
			if( index == 15 ) return palette.highlightText.ToColorRef();
		}
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

COLORREF ResolveTerminalColor( const TerminalColor& color, const theme::ThemePalette& palette, COLORREF defaultColor,
	TerminalColorRole role ) noexcept
{
	switch( color.kind ) {
	case TerminalColorKind::Indexed: return IndexedColor(color.value & 0xff, palette, role);
	case TerminalColorKind::Rgb: return RGB((color.value >> 16) & 0xff, (color.value >> 8) & 0xff, color.value & 0xff);
	case TerminalColorKind::Default: return defaultColor;
	}
	return defaultColor;
}

COLORREF ResolveTerminalForeground( const TerminalColor& color, const theme::ThemePalette& palette, COLORREF defaultColor,
	COLORREF background ) noexcept
{
	const auto resolved = ResolveTerminalColor(color, palette, defaultColor, TerminalColorRole::Foreground);
	return color.kind == TerminalColorKind::Rgb ? EnsureReadableRgbForeground(resolved, background) : resolved;
}

} // namespace terminal
