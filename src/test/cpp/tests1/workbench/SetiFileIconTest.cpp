/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/icons/SetiFileIcon.h"

// vs-seti is the file icon theme VS Code selects when the user has chosen none,
// so these expectations are upstream behaviour, not a local preference. Every
// literal below is a value in Microsoft's own
// extensions/theme-seti/icons/vs-seti-icon-theme.json, transcribed by
// tools/generate-seti-icon-theme.py; SETI-ATTRIBUTION.md pins the commit.

using workbench::icons::seti::EIconVariant;
using workbench::icons::seti::ResolveSetiFileIcon;
using workbench::icons::seti::SResolvedIcon;

namespace {

// Named so the comma inside the aggregate stays inside parentheses; a braced
// initializer would be split into two macro arguments by EXPECT_EQ.
constexpr SResolvedIcon Icon(wchar_t character, std::uint32_t color)
{
	return { character, color };
}

constexpr SResolvedIcon Dark(std::wstring_view fileName)
{
	return *ResolveSetiFileIcon(fileName, false, EIconVariant::Dark);
}

constexpr SResolvedIcon Light(std::wstring_view fileName)
{
	return *ResolveSetiFileIcon(fileName, false, EIconVariant::Light);
}

} // namespace

// The theme document has no folder, folderExpanded, rootFolder, or
// rootFolderExpanded key, so upstream never sets hasFolderIcons and a folder row
// under the default theme shows its twistie and its name and nothing else.
// Drawing some other folder glyph here would be a look-alike, not the theme.
TEST(SetiFileIcon, ADirectoryGetsNoIconBecauseTheThemeContributesNone)
{
	static_assert(!workbench::icons::seti::kHasFolderIcons);
	EXPECT_FALSE(ResolveSetiFileIcon(L"src", true, EIconVariant::Dark).has_value());
	EXPECT_FALSE(ResolveSetiFileIcon(L"node_modules", true, EIconVariant::Light).has_value());
	// A directory whose name would otherwise match a file association gets none either.
	EXPECT_FALSE(ResolveSetiFileIcon(L"LICENSE", true, EIconVariant::Dark).has_value());
}

// A fileNames rule carries the name class, every suffix class, and two markers,
// so it outranks any fileExtensions rule for the same file.
TEST(SetiFileIcon, AWholeFileNameBeatsItsExtension)
{
	EXPECT_EQ(Icon(L'\uE04D', 0x519ABAu), Dark(L"readme.md"));
	EXPECT_EQ(Icon(L'\uE060', 0x519ABAu), Dark(L"notes.md"));
	EXPECT_EQ(Icon(L'\uE05A', 0xCBCB41u), Dark(L"license"));
	EXPECT_EQ(Icon(L'\uE05F', 0xE37933u), Dark(L"makefile"));
	EXPECT_EQ(Icon(L'\uE025', 0x519ABAu), Dark(L"dockerfile"));
}

// Upstream emits one -ext-file-icon class per dotted suffix, so the rule built
// from the longest suffix carries the most classes and wins.
TEST(SetiFileIcon, TheLongestDottedSuffixWins)
{
	EXPECT_EQ(Icon(L'\uE099', 0xE37933u), Dark(L"parser.spec.ts"));
	EXPECT_EQ(Icon(L'\uE099', 0x519ABAu), Dark(L"parser.ts"));
	// A suffix that is not an association falls through to the next dot.
	EXPECT_EQ(Icon(L'\uE099', 0x519ABAu), Dark(L"parser.d.ts"));
}

// Upstream lowercases the file name before it builds the class list.
TEST(SetiFileIcon, MatchingIsCaseInsensitive)
{
	EXPECT_EQ(Dark(L"readme.md"), Dark(L"README.MD"));
	EXPECT_EQ(Dark(L"license"), Dark(L"LICENSE"));
	EXPECT_EQ(Dark(L"main.cpp"), Dark(L"Main.CPP"));
}

// A name whose only dot is the first character still has an extension: upstream
// splits on "." and keeps every segment from the second on, so ".gitignore"
// yields "gitignore".
TEST(SetiFileIcon, ALeadingDotStillProducesAnExtension)
{
	EXPECT_EQ(Icon(L'\uE034', 0x41535Bu), Dark(L".gitignore"));
	EXPECT_EQ(Icon(L'\uE034', 0x41535Bu), Dark(L".gitattributes"));
}

// The theme's own "file" association, which is what an unmatched row draws.
TEST(SetiFileIcon, AnUnmatchedNameFallsBackToTheThemeDefault)
{
	constexpr SResolvedIcon defaultIcon{ L'\uE023', 0xD4D7D6u };
	EXPECT_EQ(defaultIcon, Dark(L"mystery.qqq"));
	EXPECT_EQ(defaultIcon, Dark(L"noextension"));
	EXPECT_EQ(defaultIcon, Dark(L"trailingdot."));
	EXPECT_EQ(defaultIcon, Dark(L""));
}

// languageIds is the weakest of the three layers and matches only through VS
// Code's language registry, which this product does not have, so the generator
// folds each Seti-known language into the extensions and names that language
// claims. Without that fold none of this repository's own file types would
// resolve: the theme's fileExtensions section names none of them.
TEST(SetiFileIcon, TheFoldedLanguageLayerCoversThisRepositoryOwnFileTypes)
{
	EXPECT_EQ(Icon(L'\uE01A', 0x519ABAu), Dark(L"CExplorerTool.cpp"));
	EXPECT_EQ(Icon(L'\uE00C', 0xA074C4u), Dark(L"CExplorerTool.h"));
	EXPECT_EQ(Icon(L'\uE00C', 0x519ABAu), Dark(L"legacy.c"));
	EXPECT_EQ(Icon(L'\uE07B', 0x519ABAu), Dark(L"generate-seti-icon-theme.py"));
	EXPECT_EQ(Icon(L'\uE074', 0x519ABAu), Dark(L"check-encoding.ps1"));
	EXPECT_EQ(Icon(L'\uE0A2', 0x519ABAu), Dark(L"build-all.bat"));
	EXPECT_EQ(Icon(L'\uE0A5', 0xE37933u), Dark(L"sakura.vcxproj"));
	EXPECT_EQ(Icon(L'\uE0A7', 0xA074C4u), Dark(L"build-sakura.yml"));
	EXPECT_EQ(Icon(L'\uE055', 0xCBCB41u), Dark(L"settings.json"));
}

// The light section is emitted under the .vs body class alone. Seti overrides
// only colours there, never a glyph, so a light row is the same picture in
// darker ink.
TEST(SetiFileIcon, TheLightSectionChangesOnlyTheColour)
{
	EXPECT_EQ(Icon(L'\uE01A', 0x498BA7u), Light(L"CExplorerTool.cpp"));
	EXPECT_EQ(Icon(L'\uE04D', 0x498BA7u), Light(L"readme.md"));
	EXPECT_EQ(Icon(L'\uE023', 0xBFC2C1u), Light(L"mystery.qqq"));
	for (const auto* name : { L"readme.md", L"main.cpp", L"settings.json", L"mystery.qqq" }) {
		EXPECT_EQ(Dark(name).character, Light(name).character);
	}
}

// Upstream's _todo definition carries no fontColor at all, so its rows inherit
// the surrounding text colour instead of naming one. It is the only such icon,
// and it is also the only association the light section does not override.
TEST(SetiFileIcon, AnIconWithNoFontColorInheritsTheRowColour)
{
	constexpr auto inherit = workbench::icons::seti::kInheritColor;
	EXPECT_EQ(Icon(L'\uE096', inherit), Dark(L"todo"));
	EXPECT_EQ(Icon(L'\uE096', inherit), Dark(L"todo.md"));
	EXPECT_EQ(Icon(L'\uE096', inherit), Light(L"todo.md"));
}

// The resolver is pure and constant-evaluable, which is what lets these
// expectations be checked without a window, a device context, or a font.
TEST(SetiFileIcon, ResolutionIsAConstantExpression)
{
	static_assert(Dark(L"readme.md") == Icon(L'\uE04D', 0x519ABAu));
	static_assert(!ResolveSetiFileIcon(L"src", true, EIconVariant::Dark).has_value());
	SUCCEED();
}
