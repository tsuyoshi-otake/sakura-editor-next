/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/icons/CSetiFont.h"
#include "workbench/icons/SetiFileIcon.h"

#include <string>

// vs-seti addresses its artwork by code point in one font, so a resolved glyph
// only means something if that exact face is registered and really owns the code
// point. If the table and the embedded bytes are different versions of the theme,
// every name still resolves and every row draws .notdef -- the least visible way
// this can break.
//
// tests1.exe links the sakura .res, so the same SETIFONT resource is present
// here; declaring it again in tests1_rc.rc would fail the link with CVT1100.

using namespace workbench::icons;

namespace {

//! Look one code point up in the registered face and report whether it is a real glyph.
[[nodiscard]] bool HasRealGlyph(std::wstring_view faceName, wchar_t codePoint)
{
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) return false;
	LOGFONTW logFont{};
	logFont.lfHeight = -16;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	::wcsncpy_s(logFont.lfFaceName, std::wstring(faceName).c_str(), _TRUNCATE);

	const HFONT font = ::CreateFontIndirectW(&logFont);
	if (font == nullptr) return false;
	const HDC dc = ::CreateCompatibleDC(nullptr);
	if (dc == nullptr) {
		::DeleteObject(font);
		return false;
	}
	const HGDIOBJ previous = ::SelectObject(dc, font);

	// GDI silently substitutes another face when the name does not resolve, so
	// check the realized face first. Without that, a substituted font reports
	// real glyphs for code points seti.ttf may not even have.
	wchar_t actualFace[LF_FACESIZE]{};
	const int actualLength = ::GetTextFaceW(dc, LF_FACESIZE, actualFace);
	const bool sameFace = actualLength > 0 && faceName == std::wstring_view(actualFace);

	WORD glyphIndex = 0;
	const DWORD converted = ::GetGlyphIndicesW(dc, &codePoint, 1, &glyphIndex, GGI_MARK_NONEXISTING_GLYPHS);

	::SelectObject(dc, previous);
	::DeleteDC(dc);
	::DeleteObject(font);

	return sameFace && converted != GDI_ERROR && glyphIndex != 0xFFFF && glyphIndex != 0;
}

} // namespace

// If this fails the Explorer falls back to the first-party Codicon associations,
// which is a different picture from VS Code's default rather than a broken one.
TEST(CSetiFont, EmbeddedFontRegistersUnderTheFamilyNameTheTableIsWrittenAgainst)
{
	const auto& font = CSetiFont::Instance();
	ASSERT_TRUE(font.IsAvailable())
		<< "the SETIFONT RCDATA embedding or AddFontMemResourceEx failed";
	EXPECT_EQ(seti::kFontFamily, font.FaceName());
}

TEST(CSetiFont, InstanceIsASingleProcessWideRegistration)
{
	EXPECT_EQ(&CSetiFont::Instance(), &CSetiFont::Instance());
}

// Every style the associations can resolve to, against the bytes that actually
// shipped. One .notdef means the embedded seti.ttf is not the version
// SetiIconThemeTable.h was generated from.
TEST(CSetiFont, EveryStyleInTheThemeTableHasARealGlyphInTheRegisteredFont)
{
	const auto& font = CSetiFont::Instance();
	ASSERT_TRUE(font.IsAvailable());
	std::size_t missing = 0;
	for (const auto& style : seti::kIconStyles) {
		if (!HasRealGlyph(font.FaceName(), style.character)) ++missing;
	}
	EXPECT_EQ(0u, missing) << missing << " of " << std::size(seti::kIconStyles)
		<< " Seti styles resolve to .notdef in the embedded font";
}

// The two bundled faces are separate registrations under separate names. Seti
// code points mean nothing in codicon.ttf and the reverse, so a mix-up would
// draw unrelated artwork instead of failing.
TEST(CSetiFont, TheSetiFaceIsDistinctFromTheCodiconFace)
{
	const auto& font = CSetiFont::Instance();
	ASSERT_TRUE(font.IsAvailable());
	EXPECT_NE(std::wstring_view(L"codicon"), font.FaceName());
}
