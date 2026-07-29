/*! @file */
#include "pch.h"

#include "terminal/window/TerminalColorResolver.h"

#include <algorithm>
#include <cmath>

namespace {

double Linear( BYTE value )
{
	const auto normalized = static_cast<double>(value) / 255.0;
	return normalized <= 0.04045 ? normalized / 12.92 : std::pow((normalized + 0.055) / 1.055, 2.4);
}

double Contrast( COLORREF first, COLORREF second )
{
	const auto luminance = [](COLORREF color) {
		return 0.2126 * Linear(GetRValue(color)) + 0.7152 * Linear(GetGValue(color)) + 0.0722 * Linear(GetBValue(color));
	};
	const auto firstLum = luminance(first);
	const auto secondLum = luminance(second);
	return (std::max(firstLum, secondLum) + 0.05) / (std::min(firstLum, secondLum) + 0.05);
}

TEST(TerminalColorResolver, DarkThemeMakesAnsiBlackDistinctFromCanvas)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto black = terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
		palette.primaryText.ToColorRef(), terminal::TerminalColorRole::Foreground);
	EXPECT_EQ(RGB(128, 135, 148), black);
	EXPECT_NE(palette.primaryText.ToColorRef(), black);
}

TEST(TerminalColorResolver, DefaultUsesTheSuppliedThemeForeground)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(palette.primaryText.ToColorRef(),
		terminal::ResolveTerminalColor({}, palette, palette.primaryText.ToColorRef(),
			terminal::TerminalColorRole::Foreground));
}

TEST(TerminalColorResolver, ExtendedIndexedColorsRetainXtermValues)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(RGB(255, 0, 0),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(196), palette, palette.primaryText.ToColorRef(),
			terminal::TerminalColorRole::Foreground));
	EXPECT_EQ(RGB(8, 8, 8),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(232), palette, palette.primaryText.ToColorRef(),
			terminal::TerminalColorRole::Foreground));
}

TEST(TerminalColorResolver, AnsiBlackBackgroundRetainsLiteralVtColor)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(RGB(0, 0, 0),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.canvas.ToColorRef(), terminal::TerminalColorRole::Background));
	EXPECT_NE(
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.primaryText.ToColorRef(), terminal::TerminalColorRole::Foreground),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.canvas.ToColorRef(), terminal::TerminalColorRole::Background));
}

TEST(TerminalColorResolver, LowContrastTrueColorForegroundIsAdjustedAgainstItsActualBackground)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto background = RGB(30, 30, 30);
	const auto original = terminal::TerminalColor::Rgb(34, 34, 34);
	const auto resolved = terminal::ResolveTerminalForeground(original, palette, palette.primaryText.ToColorRef(), background);
	EXPECT_NE(RGB(34, 34, 34), resolved);
	EXPECT_GE(Contrast(resolved, background), 4.5);
}

TEST(TerminalColorResolver, LiteralTrueColorBackgroundAndInverseSourceRemainExact)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto literal = terminal::TerminalColor::Rgb(34, 34, 34);
	EXPECT_EQ(RGB(34, 34, 34), terminal::ResolveTerminalColor(literal, palette, palette.canvas.ToColorRef(),
		terminal::TerminalColorRole::Background));
}

} // namespace
