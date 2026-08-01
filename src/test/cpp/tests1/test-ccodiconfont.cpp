/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"

#include <string>

// 実 VS Code は codicon.ttf を丸ごと同梱し、`$(name)` をその 1 書体のグリフとして描く。
// この製品も同じ経路にした（sakura_rc.rc2 の `CODICONFONT RCDATA`）。
//
// ここで確かめるのは「表に載っている」ことではなく「実際に GDI へ登録できて、その書体で
// そのコードポイントが実グリフとして引ける」ことである。表とフォントが食い違えば、
// 名前は解決できているのに描かれるのは .notdef、という一番わかりにくい壊れ方になる。
//
// tests1.exe は sakura.vcxproj のオブジェクトと一緒に sakura の .res も取り込むので、
// 同じ `CODICONFONT` が tests1.exe 側にも入る（tests1_rc.rc へ二重に書くと
// CVT1100「重複するリソース」でリンクが落ちる）。埋め込みが抜ければこのテストが落ちる。

using namespace workbench::icons;

namespace {

//! 登録済みの書体でコードポイントを 1 つ引き、実グリフ（.notdef 以外）かを見る。
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

	// 代替書体で描かれていないことを先に確かめる。GDI はフォント名が解決できないと
	// 黙って別書体へ差し替えるので、これを見ないと「別の字が出ている」を見逃す。
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

// 同梱フォントが登録できていること。落ちたら組み込みアイコンは全て取り込み済み
// ベクター（約 2 ダース）へ縮退しており、それ以外の名前は代替の点になる。
TEST(CCodiconFont, EmbeddedFontRegistersAndReportsItsOwnFamilyName)
{
	const auto& font = CCodiconFont::Instance();
	ASSERT_TRUE(font.IsAvailable())
		<< "CODICONFONT RCDATA の埋め込み、または AddFontMemResourceEx が失敗している";
	// 書体名は決め打ちせずフォント自身の name テーブルから取る。実測値は L"codicon"。
	EXPECT_EQ(L"codicon", font.FaceName());
}

TEST(CCodiconFont, InstanceIsASingleProcessWideRegistration)
{
	EXPECT_EQ(&CCodiconFont::Instance(), &CCodiconFont::Instance());
}

// 表の各コードポイントがフォント側に実在すること。生成時に cmap を検証してあるが、
// 埋め込まれたバイト列が表と同じ版であることは実機でしか確かめられない。
TEST(CCodiconFont, EverySpotCheckedNameHasARealGlyphInTheRegisteredFont)
{
	const auto& font = CCodiconFont::Instance();
	ASSERT_TRUE(font.IsAvailable());
	for (const auto* name : { L"gear", L"check", L"warning", L"rocket", L"zap", L"copy",
			L"edit", L"circle-slash", L"dashboard", L"loading", L"extensions", L"source-control" }) {
		const auto glyph = FindCodiconGlyph(name);
		ASSERT_TRUE(glyph.has_value()) << "a spot-checked name is missing from CodiconGlyphTable.h";
		EXPECT_TRUE(HasRealGlyph(font.FaceName(), *glyph)) << "no glyph for one of the spot-checked names";
	}
}

// 表全体。1 件でも .notdef なら、埋め込んだ codicon.ttf が表の生成元と別版である。
TEST(CCodiconFont, EveryTableEntryHasARealGlyphInTheRegisteredFont)
{
	const auto& font = CCodiconFont::Instance();
	ASSERT_TRUE(font.IsAvailable());
	std::size_t missing = 0;
	for (const auto& entry : kCodiconGlyphs) {
		if (!HasRealGlyph(font.FaceName(), entry.character)) ++missing;
	}
	EXPECT_EQ(0u, missing) << missing << " of " << std::size(kCodiconGlyphs)
		<< " codicon names resolve to .notdef in the embedded font";
}
