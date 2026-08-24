/*! @file */
/*
	Copyright (C) 2021-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "view/CEditView_Paint.h"
#include "view/CTextMetrics.h"
#include "view/figures/CFigureStrategy.h"
#include <vector>
#include <Windows.h>

namespace {

TEST(IndentDecorationLayout, ProjectsVisualColumnsIntoEditorLayoutUnits)
{
	using view::indent_decoration::LayoutRange;
	using view::indent_decoration::ProjectVisualColumns;

	const LayoutRange firstLevel{ 0, 32 };
	const LayoutRange secondLevel{ 32, 64 };
	EXPECT_EQ(ProjectVisualColumns(0, 4, 8), firstLevel);
	EXPECT_EQ(ProjectVisualColumns(4, 4, 8), secondLevel);
	EXPECT_FALSE(ProjectVisualColumns(0, 0, 8).has_value());
	EXPECT_FALSE(ProjectVisualColumns(UINT32_MAX, 1, 8).has_value());
}

TEST(IndentGuideLayout, MeasuresLeadingSpacesAndTabsInVisualColumns)
{
	using view::indent_guide::LeadingVisualColumns;

	EXPECT_EQ(8U, LeadingVisualColumns(L"        value", 4));
	EXPECT_EQ(8U, LeadingVisualColumns(L"\t    value", 4));
	EXPECT_EQ(4U, LeadingVisualColumns(L" \tvalue", 4));
	EXPECT_EQ(0U, LeadingVisualColumns(L"value", 4));
	EXPECT_EQ(0U, LeadingVisualColumns(L"    value", 0));
}

class GdiTextSurface final {
public:
	GdiTextSurface(HDC referenceDc, HFONT font, int width, int height)
	{
		m_dc = CreateCompatibleDC(referenceDc);
		if (nullptr == m_dc) {
			return;
		}
		m_bitmap = CreateCompatibleBitmap(referenceDc, width, height);
		if (nullptr == m_bitmap) {
			return;
		}
		m_oldBitmap = SelectObject(m_dc, m_bitmap);
		m_oldFont = SelectObject(m_dc, font);
		if (!IsValid()) {
			return;
		}
		SetTextColor(m_dc, RGB(0, 0, 0));
		SetBkColor(m_dc, RGB(255, 255, 255));
		SetBkMode(m_dc, OPAQUE);
		PatBlt(m_dc, 0, 0, width, height, WHITENESS);
	}

	~GdiTextSurface()
	{
		if (nullptr != m_dc && nullptr != m_oldFont && HGDI_ERROR != m_oldFont) {
			SelectObject(m_dc, m_oldFont);
		}
		if (nullptr != m_dc && nullptr != m_oldBitmap && HGDI_ERROR != m_oldBitmap) {
			SelectObject(m_dc, m_oldBitmap);
		}
		if (nullptr != m_bitmap) {
			DeleteObject(m_bitmap);
		}
		if (nullptr != m_dc) {
			DeleteDC(m_dc);
		}
	}

	bool IsValid() const noexcept
	{
		return nullptr != m_dc && nullptr != m_bitmap
			&& nullptr != m_oldBitmap && HGDI_ERROR != m_oldBitmap
			&& nullptr != m_oldFont && HGDI_ERROR != m_oldFont;
	}

	HDC Get() const noexcept
	{
		return m_dc;
	}

private:
	HDC m_dc{};
	HBITMAP m_bitmap{};
	HGDIOBJ m_oldBitmap{};
	HGDIOBJ m_oldFont{};
};

bool IsLegacyBlockRenderableCodeUnit(const wchar_t code) noexcept
{
	return (0x20 <= code && code <= 0x7f)
		|| (0x2e80 <= code && code <= 0x2fdf)
		|| (0x3041 <= code && code <= 0x3096)
		|| (0x30a1 <= code && code <= 0x30fa)
		|| (0x3400 <= code && code <= 0x4dbf)
		|| (0x4e00 <= code && code <= 0x9fff)
		|| (0xf900 <= code && code <= 0xfaff)
		|| (0xff01 <= code && code <= 0xff5e)
		|| (0xff61 <= code && code <= 0xff9f);
}
}

class CTextMetricsWithGDI : public testing::Test {
protected:
	CTextMetricsWithGDI() {
		lf1.lfCharSet = DEFAULT_CHARSET;
		std::wcscpy(lf1.lfFaceName, L"MS Gothic");

		dc = GetDC(nullptr);
		font = CreateFontIndirect(&lf1);
		oldFont = (HFONT)SelectObject(dc, font);

		GetTextExtentPoint32(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &size);
		GetTextMetrics(dc, &tm);
	}
	~CTextMetricsWithGDI() {
		SelectObject(dc, oldFont);
		DeleteObject(font);
		ReleaseDC(nullptr, dc);
	}
	SIZE size;
	TEXTMETRIC tm;
	LOGFONT lf1{};
	HDC dc;
	HFONT font;
	HFONT oldFont;
};

TEST(CFigureText, BlockRenderableCodeUnit)
{
	// Existing fixed-grid ranges remain block-safe.
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(L'A'));
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x3042)); // Hiragana A
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x4e00)); // CJK ideograph

	// Common spacing punctuation can join the surrounding Japanese text run.
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x3001)); // ideographic comma
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x3002)); // ideographic full stop
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x300c)); // left corner bracket
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x301c)); // wave dash
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x30fb)); // Katakana middle dot
	EXPECT_TRUE(CFigure_Text::IsBlockRenderableCodeUnit(0x30fc)); // prolonged sound mark

	// Characters that participate in a cluster or multi-code-unit sequence keep
	// the existing one-character fallback.
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0x302a)); // combining tone mark
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0x3031)); // paired vertical repeat mark
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0x3099)); // combining voiced mark
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0x200d)); // zero-width joiner
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0xfe0f)); // variation selector
	EXPECT_FALSE(CFigure_Text::IsBlockRenderableCodeUnit(0xd83c)); // high surrogate
}

TEST_F(CTextMetricsWithGDI, Update1)
{
	// 引数に0を設定
	CTextMetrics metrics;
	metrics.Update(dc, font, 0, 0);
	EXPECT_EQ(metrics.GetHankakuWidth(), (size.cx / 26 + 1) / 2);
	EXPECT_EQ(metrics.GetHankakuHeight(), size.cy);
	EXPECT_EQ(metrics.GetCharHeightMarginByFontNo(0), 0);
	EXPECT_EQ(metrics.GetHankakuDx(), metrics.GetHankakuWidth());
	EXPECT_EQ(metrics.GetHankakuDy(), metrics.GetHankakuHeight());
	for (int i = 0; i < 64; ++i)
		EXPECT_EQ(metrics.GetDxArray_AllHankaku()[i], metrics.GetHankakuDx());

	// Updateに依存するその他の関数のテスト
	EXPECT_EQ(metrics.GetCharSpacing(), metrics.GetHankakuDx() - metrics.GetHankakuWidth());
	EXPECT_EQ(metrics.GetCharPxWidth(), 1);
	EXPECT_EQ(metrics.GetCharPxWidth(CLayoutInt(0)), 0);
	EXPECT_EQ(metrics.GetCharPxWidth(CLayoutInt(42)), 42);
	EXPECT_EQ((Int)metrics.GetLayoutXDefault(), metrics.GetHankakuDx());
	EXPECT_EQ((Int)metrics.GetLayoutXDefault(0), 0);
	EXPECT_EQ((Int)metrics.GetLayoutXDefault(42), metrics.GetHankakuDx() * 42);
	CTextMetrics metrics2;
	metrics.CopyTextMetricsStatus(&metrics2);
	EXPECT_EQ(metrics2.GetHankakuWidth(), metrics.GetHankakuWidth());
	EXPECT_EQ(metrics2.GetHankakuHeight(), metrics.GetHankakuHeight());
	EXPECT_EQ(metrics2.GetCharHeightMarginByFontNo(0), metrics.GetCharHeightMarginByFontNo(0));

	class FakeCache : public CCharWidthCache {
	public:
		int CalcPxWidthByFont(wchar_t ch) override { return 1; }
	} cache;
	EXPECT_EQ(metrics.CalcTextWidth3(L"a", 1, cache), 1);
	std::vector<int> v;
	metrics.GenerateDxArray2(&v, L"a", 1, cache);
	EXPECT_EQ(v[0], 1);
}

TEST_F(CTextMetricsWithGDI, BlockRenderingMatchesFragmentedJapanesePunctuation)
{
	constexpr wchar_t text[] =
		L"日本語\u3001句点\u3002\u3008括弧\u3009\u3010隅括弧\u3011"
		L"\u301c波\u3030\u30a0カタカナ\u30fb長音\u30fc\u30ff";
	constexpr int surfaceWidth = 1024;
	constexpr int surfaceHeight = 64;
	const int length = static_cast<int>(_countof(text) - 1);
	std::vector<int> dx(length, 24);

	GdiTextSurface fragmented(dc, font, surfaceWidth, surfaceHeight);
	GdiTextSurface block(dc, font, surfaceWidth, surfaceHeight);
	ASSERT_TRUE(fragmented.IsValid());
	ASSERT_TRUE(block.IsValid());

	RECT blockClip{ 0, 0, 24 * length, surfaceHeight };
	ASSERT_TRUE(ExtTextOutW(
		block.Get(), 0, 0, ETO_CLIPPED | ETO_OPAQUE, &blockClip, text, length, dx.data()));

	int start = 0;
	int drawX = 0;
	while (start < length) {
		int end = start + 1;
		if (IsLegacyBlockRenderableCodeUnit(text[start])) {
			while (end < length && IsLegacyBlockRenderableCodeUnit(text[end])) {
				++end;
			}
		}
		int segmentWidth = 0;
		for (int index = start; index < end; ++index) {
			segmentWidth += dx[index];
		}
		RECT segmentClip{ drawX, 0, drawX + segmentWidth, surfaceHeight };
		ASSERT_TRUE(ExtTextOutW(
			fragmented.Get(), drawX, 0, ETO_CLIPPED | ETO_OPAQUE,
			&segmentClip, text + start, end - start, dx.data() + start));
		drawX += segmentWidth;
		start = end;
	}

	int differentPixels = 0;
	POINT firstDifference{ -1, -1 };
	for (int y = 0; y < surfaceHeight; ++y) {
		for (int x = 0; x < surfaceWidth; ++x) {
			if (GetPixel(fragmented.Get(), x, y) != GetPixel(block.Get(), x, y)) {
				if (0 == differentPixels) {
					firstDifference = POINT{ x, y };
				}
				++differentPixels;
			}
		}
	}
	EXPECT_EQ(0, differentPixels)
		<< "first differing pixel: (" << firstDifference.x << ", " << firstDifference.y << ")";
}

TEST_F(CTextMetricsWithGDI, Update2)
{
	// nLineSpaceに正の数を設定
	CTextMetrics metrics;
	metrics.Update(dc, font, 1000, 0);
	EXPECT_EQ(metrics.GetHankakuWidth(), (size.cx / 26 + 1) / 2);
	EXPECT_EQ(metrics.GetHankakuHeight(), size.cy);
	EXPECT_EQ(metrics.GetCharHeightMarginByFontNo(0), 0);
	EXPECT_EQ(metrics.GetHankakuDx(), metrics.GetHankakuWidth());
	EXPECT_EQ(metrics.GetHankakuDy(), metrics.GetHankakuHeight() + 1000);
}

TEST_F(CTextMetricsWithGDI, Update3)
{
	// nLineSpaceに負の数を設定
	CTextMetrics metrics;
	metrics.Update(dc, font, -1000, 0);
	EXPECT_EQ(metrics.GetHankakuWidth(), (size.cx / 26 + 1) / 2);
	EXPECT_EQ(metrics.GetHankakuHeight(), 1);
	EXPECT_EQ(metrics.GetCharHeightMarginByFontNo(0), 0);
	EXPECT_EQ(metrics.GetHankakuDx(), metrics.GetHankakuWidth());
	EXPECT_EQ(metrics.GetHankakuDy(), 1);
}

TEST_F(CTextMetricsWithGDI, Update4)
{
	// nColmSpaceに正の数を設定
	CTextMetrics metrics;
	metrics.Update(dc, font, 0, 1000);
	EXPECT_EQ(metrics.GetHankakuWidth(), (size.cx / 26 + 1) / 2);
	EXPECT_EQ(metrics.GetHankakuHeight(), size.cy);
	EXPECT_EQ(metrics.GetCharHeightMarginByFontNo(0), 0);
	EXPECT_EQ(metrics.GetHankakuDx(), metrics.GetHankakuWidth() + 1000);
	EXPECT_EQ(metrics.GetHankakuDy(), metrics.GetHankakuHeight());
}

TEST_F(CTextMetricsWithGDI, Update5)
{
	// nColmSpaceに負の数を設定
	CTextMetrics metrics;
	metrics.Update(dc, font, 0, -1000);
	EXPECT_EQ(metrics.GetHankakuWidth(), (size.cx / 26 + 1) / 2);
	EXPECT_EQ(metrics.GetHankakuHeight(), size.cy);
	EXPECT_EQ(metrics.GetCharHeightMarginByFontNo(0), 0);
	EXPECT_EQ(metrics.GetHankakuDx(), metrics.GetHankakuWidth() - 1000);
	EXPECT_EQ(metrics.GetHankakuDy(), metrics.GetHankakuHeight());
}

class FakeCache1 : public CCharWidthCache {
	int i = 0;
public:
	int CalcPxWidthByFont(wchar_t ch) override {
		return ++i;
	}
	int CalcPxWidthByFont2(const wchar_t* p) const override {
		return 10000;
	}
};

TEST(CTextMetrics, GenerateDxArray1)
{
	// 各文字の幅を CalcPxWidthByFont で計算して返す
	std::vector<int> v;
	FakeCache1 cache;
	const int* p = CTextMetrics::GenerateDxArray(&v, L"ab", 2, 0, 0, 0, 0, cache);
	EXPECT_EQ(p, v.data());
	EXPECT_EQ(v[0], 1);
	EXPECT_EQ(v[1], 2);
}

TEST(CTextMetrics, GenerateDxArray2)
{
	// 各文字の幅に nCharSpacing を足して返す
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"ab", 2, 0, 0, 0, 10, cache);
	EXPECT_EQ(v[0], 11);
	EXPECT_EQ(v[1], 12);
}

TEST(CTextMetrics, GenerateDxArray3)
{
	// サロゲートペアの幅は CalcPxWidthByFont2 で計算する
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"\xd83c\xdf38", 2, 0, 0, 0, 0, cache);
	EXPECT_EQ(v[0], 10000);
}

TEST(CTextMetrics, GenerateDxArray4)
{
	// サロゲートペアの幅に nCharSpacing を足して返す
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"\xd83c\xdf38", 2, 0, 0, 0, 10, cache);
	EXPECT_EQ(v[0], 10020);
}

TEST(CTextMetrics, GenerateDxArray5)
{
	// 対応する下位サロゲートのない上位サロゲートの幅は CalcPxWidthByFont を使って計算する
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"\xd83c,", 2, 0, 0, 0, 0, cache);
	EXPECT_EQ(v[0], 1);
	EXPECT_EQ(v[1], 2);
}

TEST(CTextMetrics, GenerateDxArray6)
{
	// 上位サロゲート片 + nCharSpacing の組み合わせ
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"\xd83c,", 2, 0, 0, 0, 10, cache);
	EXPECT_EQ(v[0], 21);
	EXPECT_EQ(v[1], 12);
}

TEST(CTextMetrics, GenerateDxArray7)
{
	// タブ幅計算のテスト
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"\t\t \t", 4, 10, 100, 1000, 0, cache);
	EXPECT_EQ(v[0], 100);
	EXPECT_EQ(v[1], 100);
	EXPECT_EQ(v[2], 1);
	EXPECT_EQ(v[3], 99);
}

TEST(CTextMetrics, GenerateDxArray8)
{
	// IVSのVariantSelectorが続く文字列は先頭1文字 + 幅0×2で生成する
	std::vector<int> v;
	FakeCache1 cache;
	CTextMetrics::GenerateDxArray(&v, L"葛󠄀", 3, 0, 0, 0, 10, cache);
	EXPECT_TRUE(v[0]);
	EXPECT_FALSE(v[1]);
	EXPECT_FALSE(v[2]);
}

TEST(CTextMetrics, CalcTextWidth)
{
	int dx[] = {1, 2, 3};
	EXPECT_EQ(CTextMetrics::CalcTextWidth(nullptr, 3, dx), 6);
}

TEST(CTextMetrics, CalcTextWidth2)
{
	class FakeCache : public CCharWidthCache {
		int i = 0;
	public:
		int CalcPxWidthByFont(wchar_t ch) override {
			return ++i;
		}
	};
	std::vector<int> v;
	FakeCache cache;
	EXPECT_EQ(CTextMetrics::CalcTextWidth2(L"abc", 3, 0, 0, v, cache), 6);
}
