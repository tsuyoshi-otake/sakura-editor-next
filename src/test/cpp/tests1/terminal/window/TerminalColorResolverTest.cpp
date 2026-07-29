/*! @file */
#include "pch.h"

#include "terminal/window/TerminalColorResolver.h"

namespace {

TEST(TerminalColorResolver, DarkThemePreservesAnsiBlackExactly)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto black = terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
		palette.primaryText.ToColorRef(), terminal::TerminalColorRole::Foreground);
	EXPECT_EQ(RGB(0x28, 0x2C, 0x34), black);
}

TEST(TerminalColorResolver, DefaultUsesTheSuppliedThemeForeground)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(palette.primaryText.ToColorRef(),
		terminal::ResolveTerminalColor({}, palette, palette.primaryText.ToColorRef(),
			terminal::TerminalColorRole::Foreground));
}

TEST(TerminalColorResolver, UsesOneHalfProfileDefaultsAndAnsiColors)
{
	const auto dark = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(RGB(0x28, 0x2C, 0x34), terminal::TerminalDefaultBackground(dark));
	EXPECT_EQ(RGB(0xDC, 0xDF, 0xE4), terminal::TerminalDefaultForeground(dark));
	EXPECT_EQ(RGB(0x61, 0xAF, 0xEF),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(4), dark,
			terminal::TerminalDefaultForeground(dark), terminal::TerminalColorRole::Foreground));

	const auto light = theme::CThemeService::PaletteFor(theme::ThemeMode::Light);
	EXPECT_EQ(RGB(0xFA, 0xFA, 0xFA), terminal::TerminalDefaultBackground(light));
	EXPECT_EQ(RGB(0x38, 0x3A, 0x42), terminal::TerminalDefaultForeground(light));
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

TEST(TerminalColorResolver, AnsiBlackBackgroundUsesTheSelectedProfileScheme)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(RGB(0x28, 0x2C, 0x34),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.canvas.ToColorRef(), terminal::TerminalColorRole::Background));
	EXPECT_EQ(
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.primaryText.ToColorRef(), terminal::TerminalColorRole::Foreground),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette,
			palette.canvas.ToColorRef(), terminal::TerminalColorRole::Background));
}

TEST(TerminalColorResolver, TrueColorForegroundRemainsExactEvenAtLowContrast)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto background = RGB(30, 30, 30);
	const auto original = terminal::TerminalColor::Rgb(34, 34, 34);
	const auto resolved = terminal::ResolveTerminalForeground(original, palette, palette.primaryText.ToColorRef(), background);
	EXPECT_EQ(RGB(34, 34, 34), resolved);
}

TEST(TerminalColorResolver, OneHalfAnsiNeutralsRemainExactForTuiRendition)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto resolve = [&](unsigned int index) {
		return terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(index), palette,
			terminal::TerminalDefaultForeground(palette), terminal::TerminalColorRole::Foreground);
	};
	EXPECT_EQ(RGB(0x28, 0x2C, 0x34), resolve(0));
	EXPECT_EQ(RGB(0xDC, 0xDF, 0xE4), resolve(7));
	EXPECT_EQ(RGB(0x5A, 0x63, 0x74), resolve(8));
	EXPECT_EQ(RGB(0xDC, 0xDF, 0xE4), resolve(15));
}

TEST(TerminalColorResolver, LiteralTrueColorBackgroundAndInverseSourceRemainExact)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto literal = terminal::TerminalColor::Rgb(34, 34, 34);
	EXPECT_EQ(RGB(34, 34, 34), terminal::ResolveTerminalColor(literal, palette, palette.canvas.ToColorRef(),
		terminal::TerminalColorRole::Background));
}

} // namespace
