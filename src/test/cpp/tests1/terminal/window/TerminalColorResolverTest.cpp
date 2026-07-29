/*! @file */
#include "pch.h"

#include "terminal/window/TerminalColorResolver.h"

namespace {

TEST(TerminalColorResolver, DarkThemeMakesAnsiBlackDistinctFromCanvas)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const auto black = terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(0), palette, palette.primaryText.ToColorRef());
	EXPECT_EQ(RGB(128, 135, 148), black);
	EXPECT_NE(palette.primaryText.ToColorRef(), black);
}

TEST(TerminalColorResolver, DefaultUsesTheSuppliedThemeForeground)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(palette.primaryText.ToColorRef(),
		terminal::ResolveTerminalColor({}, palette, palette.primaryText.ToColorRef()));
}

TEST(TerminalColorResolver, ExtendedIndexedColorsRetainXtermValues)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	EXPECT_EQ(RGB(255, 0, 0),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(196), palette, palette.primaryText.ToColorRef()));
	EXPECT_EQ(RGB(8, 8, 8),
		terminal::ResolveTerminalColor(terminal::TerminalColor::Indexed(232), palette, palette.primaryText.ToColorRef()));
}

} // namespace
