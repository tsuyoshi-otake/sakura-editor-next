/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include <cmath>
#include <vector>
#include <array>
#include <limits.h>
#include "view/CEditView_Paint.h"
#include "view/CEditView.h"
#include "view/MiniMapOverview.h"
#include "view/CViewFont.h"
#include "view/CRuler.h"
#include "view/colors/CColorStrategy.h"
#include "view/colors/CColor_Found.h"
#include "view/figures/CFigureManager.h"
#include "types/CTypeSupport.h"
#include "doc/CEditDoc.h"
#include "doc/layout/CLayout.h"
#include "debug/StartupTrace.h"
#include "window/CEditWnd.h"
#include "theme/CThemeService.h"
#include "theme/TextMateScopeColorResolver.h"
#include "senp/SenpLanguageService.h"
#include "senp/SenpRuntimeService.h"
#include "parse/CWordParse.h"
#include "util/string_ex2.h"
#ifdef USE_SSE2
#ifdef __MINGW32__
#include <x86intrin.h>
#else
#include <intrin.h>
#endif
#endif

void _DispWrap(CGraphics& gr, DispPos* pDispPos, const CEditView* pcView, CLayoutYInt nLineNum);

namespace {

COLORREF IndentDecorationColor(std::uint32_t depth) noexcept
{
	// Match indent-rainbow's classic four-color cycle. Alpha is applied by the
	// editor so text, whitespace markers, themes, and selections stay legible.
	constexpr std::array<COLORREF, 4> colors{
		RGB(255, 255, 64), RGB(127, 255, 127),
		RGB(255, 127, 255), RGB(79, 236, 236),
	};
	return colors[depth % colors.size()];
}

constexpr BYTE kIndentDecorationAlpha = 18;

std::uint32_t MiniMapDibPixel(COLORREF color) noexcept
{
	// A 32-bit BI_RGB DIB stores bytes in B, G, R, unused order.
	return (static_cast<std::uint32_t>(GetRValue(color)) << 16)
		| (static_cast<std::uint32_t>(GetGValue(color)) << 8)
		| static_cast<std::uint32_t>(GetBValue(color));
}

std::uint32_t BlendMiniMapPixel(
	std::uint32_t destination, COLORREF source, int alpha) noexcept
{
	alpha = (std::clamp)(alpha, 0, 255);
	const int inverse = 255 - alpha;
	const auto blend = [alpha, inverse](int foreground, int background) noexcept {
		return (foreground * alpha + background * inverse + 127) / 255;
	};
	const int blue = blend(GetBValue(source), destination & 0xFFU);
	const int green = blend(GetGValue(source), (destination >> 8) & 0xFFU);
	const int red = blend(GetRValue(source), (destination >> 16) & 0xFFU);
	return static_cast<std::uint32_t>(blue)
		| (static_cast<std::uint32_t>(green) << 8)
		| (static_cast<std::uint32_t>(red) << 16);
}

void BlendMiniMapRectangle(std::vector<std::uint32_t>& pixels, int width, int height,
	const RECT& rectangle, COLORREF color, int alpha) noexcept
{
	const int left = (std::clamp)(static_cast<int>(rectangle.left), 0, width);
	const int right = (std::clamp)(static_cast<int>(rectangle.right), 0, width);
	const int top = (std::clamp)(static_cast<int>(rectangle.top), 0, height);
	const int bottom = (std::clamp)(static_cast<int>(rectangle.bottom), 0, height);
	if( right <= left || bottom <= top ) return;
	for( int y = top; y < bottom; ++y ) {
		for( int x = left; x < right; ++x ) {
			auto& pixel = pixels[static_cast<std::size_t>(y)
				* static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
			pixel = BlendMiniMapPixel(pixel, color, alpha);
		}
	}
}

void PresentMiniMapPixels(HDC target, const RECT& client,
	const std::vector<std::uint32_t>& pixels) noexcept
{
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if( target == nullptr || width <= 0 || height <= 0
		|| pixels.size() != static_cast<std::size_t>(width)
			* static_cast<std::size_t>(height) ) return;
	BITMAPINFO bitmapInfo{};
	bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
	bitmapInfo.bmiHeader.biWidth = width;
	bitmapInfo.bmiHeader.biHeight = -height;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;
	(void)::SetDIBitsToDevice(target, client.left, client.top, width, height,
		0, 0, 0, height, pixels.data(), &bitmapInfo, DIB_RGB_COLORS);
}

void PaintMiniMapBlock(std::vector<std::uint32_t>& pixels, int width, int height,
	int left, int top, int columns, int scale, int lineHeight,
	COLORREF color) noexcept
{
	if( width <= 0 || height <= 0 || columns <= 0 || scale <= 0
		|| left >= width || top >= height || top + lineHeight <= 0 ) return;
	const int right = (std::min)(width, left + columns * scale);
	const int bottom = (std::min)(height, top + (std::max)(1, lineHeight));
	for( int x = (std::max)(0, left); x < right; ++x ) {
		for( int y = (std::max)(0, top); y < bottom; ++y ) {
			auto& pixel = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
				+ static_cast<std::size_t>(x)];
			// VS Code's block renderer uses half of the resolved foreground alpha.
			pixel = BlendMiniMapPixel(pixel, color, 128);
		}
	}
}

//! VS Code does not ask the platform to rasterize a two-pixel font. It samples
//! printable characters at 10x16 and downsamples that sheet to scale x 2*scale.
//! The resulting coverage atlas preserves letter shapes at minimap resolution.
class MiniMapCharAtlas final {
public:
	MiniMapCharAtlas() = default;
	MiniMapCharAtlas(const MiniMapCharAtlas&) = delete;
	MiniMapCharAtlas& operator=(const MiniMapCharAtlas&) = delete;
	~MiniMapCharAtlas() noexcept
	{
		ReleaseGdi();
	}

	bool Create(HDC reference, const LOGFONT& sourceFont, int scale) noexcept
	{
		Reset();
		if( reference == nullptr || scale <= 0 ) return false;
		m_outputWidth = scale;
		m_outputHeight = scale * 2;
		m_dc = ::CreateCompatibleDC(reference);
		if( m_dc == nullptr ) return false;

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
		bitmapInfo.bmiHeader.biWidth = kSheetWidth;
		bitmapInfo.bmiHeader.biHeight = -kSampleHeight;
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		m_bitmap = ::CreateDIBSection(reference, &bitmapInfo, DIB_RGB_COLORS,
			&bits, nullptr, 0);
		if( m_bitmap == nullptr || bits == nullptr ) return false;
		m_bits = static_cast<std::uint32_t*>(bits);
		const HGDIOBJ oldBitmap = ::SelectObject(m_dc, m_bitmap);
		if( oldBitmap == nullptr || oldBitmap == HGDI_ERROR ) return false;
		m_oldBitmap = static_cast<HBITMAP>(oldBitmap);

		LOGFONT glyphFont = sourceFont;
		glyphFont.lfHeight = -kSampleHeight;
		glyphFont.lfWidth = 0;
		glyphFont.lfEscapement = 0;
		glyphFont.lfOrientation = 0;
		glyphFont.lfWeight = FW_BOLD;
		glyphFont.lfQuality = ANTIALIASED_QUALITY;
		m_font = ::CreateFontIndirectW(&glyphFont);
		if( m_font == nullptr ) return false;
		const HGDIOBJ oldFont = ::SelectObject(m_dc, m_font);
		if( oldFont == nullptr || oldFont == HGDI_ERROR ) return false;
		m_oldFont = static_cast<HFONT>(oldFont);

		::SetBkMode(m_dc, OPAQUE);
		::SetBkColor(m_dc, RGB(0, 0, 0));
		::SetTextColor(m_dc, RGB(255, 255, 255));
		::PatBlt(m_dc, 0, 0, kSheetWidth, kSampleHeight, BLACKNESS);
		for( int index = 0; index < kCharacterCount; ++index ) {
			const wchar_t character = index < kPrintableCharacterCount
				? static_cast<wchar_t>(L' ' + index) : L'?';
			RECT cell{ index * kSampleWidth, 0,
				(index + 1) * kSampleWidth, kSampleHeight };
			(void)::DrawTextW(m_dc, &character, 1, &cell,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
		::GdiFlush();

		m_coverage.assign(static_cast<std::size_t>(kCharacterCount)
			* static_cast<std::size_t>(m_outputWidth)
			* static_cast<std::size_t>(m_outputHeight), 0);
		int brightest = 0;
		for( int character = 0; character < kCharacterCount; ++character ) {
			for( int outputY = 0; outputY < m_outputHeight; ++outputY ) {
				const double sourceTop = static_cast<double>(outputY)
					* kSampleHeight / m_outputHeight;
				const double sourceBottom = static_cast<double>(outputY + 1)
					* kSampleHeight / m_outputHeight;
				for( int outputX = 0; outputX < m_outputWidth; ++outputX ) {
					const double sourceLeft = static_cast<double>(outputX)
						* kSampleWidth / m_outputWidth;
					const double sourceRight = static_cast<double>(outputX + 1)
						* kSampleWidth / m_outputWidth;
					double weightedCoverage = 0.0;
					double area = 0.0;
					for( int sourceY = static_cast<int>(std::floor(sourceTop));
						sourceY < static_cast<int>(std::ceil(sourceBottom)); ++sourceY ) {
						const double verticalWeight = (std::max)(0.0,
							(std::min)(sourceBottom, static_cast<double>(sourceY + 1))
							- (std::max)(sourceTop, static_cast<double>(sourceY)));
						for( int sourceX = static_cast<int>(std::floor(sourceLeft));
							sourceX < static_cast<int>(std::ceil(sourceRight)); ++sourceX ) {
							const double horizontalWeight = (std::max)(0.0,
								(std::min)(sourceRight, static_cast<double>(sourceX + 1))
								- (std::max)(sourceLeft, static_cast<double>(sourceX)));
							const double weight = horizontalWeight * verticalWeight;
							const auto sample = m_bits[static_cast<std::size_t>(sourceY)
								* static_cast<std::size_t>(kSheetWidth)
								+ static_cast<std::size_t>(character * kSampleWidth + sourceX)];
							const int intensity = (std::max)({
								static_cast<int>(sample & 0xFFU),
								static_cast<int>((sample >> 8) & 0xFFU),
								static_cast<int>((sample >> 16) & 0xFFU) });
							weightedCoverage += intensity * weight;
							area += weight;
						}
					}
					const int coverage = area <= 0.0 ? 0
						: static_cast<int>(weightedCoverage / area + 0.5);
					const auto atlasIndex = CoverageIndex(
						character, outputX, outputY);
					m_coverage[atlasIndex] = static_cast<std::uint8_t>(coverage);
					brightest = (std::max)(brightest, coverage);
				}
			}
		}
		if( brightest > 0 ) {
			for( auto& coverage : m_coverage ) {
				coverage = static_cast<std::uint8_t>((coverage * 255 + brightest / 2)
					/ brightest);
			}
		}
		ReleaseGdi();
		return !m_coverage.empty();
	}

	bool Paint(std::vector<std::uint32_t>& pixels, int width, int height,
		int left, int top, int lineHeight, wchar_t character, COLORREF color) const noexcept
	{
		if( m_coverage.empty() || width <= 0 || height <= 0
			|| left >= width || top >= height || lineHeight <= 0 ) return false;
		const int characterIndex = character >= L' ' && character <= L'~'
			? static_cast<int>(character - L' ') : kPrintableCharacterCount;
		const int paintHeight = (std::min)(lineHeight, m_outputHeight);
		for( int y = 0; y < paintHeight; ++y ) {
			const int destinationY = top + y;
			if( destinationY < 0 || destinationY >= height ) continue;
			for( int x = 0; x < m_outputWidth; ++x ) {
				const int destinationX = left + x;
				if( destinationX < 0 || destinationX >= width ) continue;
				// Upstream softens normal character coverage to 12/15 before
				// blending it into the resolved minimap background.
				const int coverage = (m_coverage[CoverageIndex(characterIndex, x, y)]
					* 12 + 7) / 15;
				auto& pixel = pixels[static_cast<std::size_t>(destinationY)
					* static_cast<std::size_t>(width) + static_cast<std::size_t>(destinationX)];
				pixel = BlendMiniMapPixel(pixel, color, coverage);
			}
		}
		return true;
	}

private:
	static constexpr int kSampleWidth = 10;
	static constexpr int kSampleHeight = 16;
	static constexpr int kPrintableCharacterCount = 95;
	static constexpr int kCharacterCount = kPrintableCharacterCount + 1;
	static constexpr int kSheetWidth = kCharacterCount * kSampleWidth;

	[[nodiscard]] std::size_t CoverageIndex(
		int character, int x, int y) const noexcept
	{
		return (static_cast<std::size_t>(character)
			* static_cast<std::size_t>(m_outputHeight) + static_cast<std::size_t>(y))
			* static_cast<std::size_t>(m_outputWidth) + static_cast<std::size_t>(x);
	}

	void Reset() noexcept
	{
		ReleaseGdi();
		m_coverage.clear();
		m_outputWidth = 0;
		m_outputHeight = 0;
	}

	void ReleaseGdi() noexcept
	{
		if( m_dc != nullptr && m_oldFont != nullptr ) {
			::SelectObject(m_dc, m_oldFont);
		}
		if( m_dc != nullptr && m_oldBitmap != nullptr ) {
			::SelectObject(m_dc, m_oldBitmap);
		}
		if( m_font != nullptr ) ::DeleteObject(m_font);
		if( m_bitmap != nullptr ) ::DeleteObject(m_bitmap);
		if( m_dc != nullptr ) ::DeleteDC(m_dc);
		m_dc = nullptr;
		m_bitmap = nullptr;
		m_oldBitmap = nullptr;
		m_font = nullptr;
		m_oldFont = nullptr;
		m_bits = nullptr;
	}

	HDC m_dc{};
	HBITMAP m_bitmap{};
	HBITMAP m_oldBitmap{};
	HFONT m_font{};
	HFONT m_oldFont{};
	std::uint32_t* m_bits{};
	std::vector<std::uint8_t> m_coverage;
	int m_outputWidth{};
	int m_outputHeight{};
};

std::optional<theme::ThemeSyntaxTokenKind> SyntaxKindForColorIndex(EColorIndexType index) noexcept
{
	if (index == COLORIDX_COMMENT || index == COLORIDX_BLOCK1 || index == COLORIDX_BLOCK2) {
		return theme::ThemeSyntaxTokenKind::Comment;
	}
	if (index == COLORIDX_SSTRING || index == COLORIDX_WSTRING || index == COLORIDX_HEREDOC) {
		return theme::ThemeSyntaxTokenKind::String;
	}
	if (index == COLORIDX_DIGIT) return theme::ThemeSyntaxTokenKind::Number;
	if (index >= COLORIDX_KEYWORD1 && index <= COLORIDX_KEYWORD10) {
		return theme::ThemeSyntaxTokenKind::Keyword;
	}
	if ((index >= COLORIDX_REGEX1 && index <= COLORIDX_REGEX10)
		|| (index >= COLORIDX_REGEX_FIRST && index <= COLORIDX_REGEX_LAST)) {
		return theme::ThemeSyntaxTokenKind::Regexp;
	}
	return std::nullopt;
}

} // namespace

/*
	PAINT_LINENUMBER = (1<<0), //!< 行番号
	PAINT_RULER      = (1<<1), //!< ルーラー
	PAINT_BODY       = (1<<2), //!< 本文
*/

void CEditView_Paint::Call_OnPaint(
	int nPaintFlag,   //!< 描画する領域を選択する
	bool bUseMemoryDC //!< メモリDCを使用する
)
{
	CEditView* pView = GetEditView();

	//各要素
	CMyRect rcLineNumber(0,pView->GetTextArea().GetAreaTop(),pView->GetTextArea().GetAreaLeft(),pView->GetTextArea().GetAreaBottom());
	CMyRect rcRuler(pView->GetTextArea().GetAreaLeft(),0,pView->GetTextArea().GetAreaRight(),pView->GetTextArea().GetAreaTop());
	CMyRect rcBody(pView->GetTextArea().GetAreaLeft(),pView->GetTextArea().GetAreaTop(),pView->GetTextArea().GetAreaRight(),pView->GetTextArea().GetAreaBottom());

	//領域を作成 -> rc
	std::vector<CMyRect> rcs;
	rcs.reserve(3);
	if(nPaintFlag & PAINT_LINENUMBER)rcs.push_back(rcLineNumber);
	if(nPaintFlag & PAINT_RULER)rcs.push_back(rcRuler);
	if(nPaintFlag & PAINT_BODY)rcs.push_back(rcBody);
	if(rcs.size()==0)return;
	pView->MarkRenderDamage(editor::rendering::EEditViewDamage::BaseText);
	CMyRect rc=rcs[0];
	int nSize = (int)rcs.size();
	for(int i=1;i<nSize;i++)
		rc=MergeRect(rc,rcs[i]);

	//描画
	PAINTSTRUCT	ps;
	ps.rcPaint = rc;
	HDC hdc = pView->GetDC();
	pView->OnPaint( hdc, &ps, bUseMemoryDC );
	pView->ReleaseDC( hdc );
	pView->CommitGdiPaintBoundary();
}

/* フォーカス移動時の再描画

	@date 2001/06/21 asa-o 「スクロールバーの状態を更新する」「カーソル移動」削除
*/
void CEditView::RedrawAll()
{
	if( nullptr == GetHwnd() ){
		return;
	}

	if( GetDrawSwitch() ){
		MarkRenderDamage(editor::rendering::EEditViewDamage::BaseText);
		// ウィンドウ全体を再描画
		PAINTSTRUCT	ps;
		HDC hdc = ::GetDC( GetHwnd() );
		::GetClientRect( GetHwnd(), &ps.rcPaint );
		OnPaint( hdc, &ps, FALSE );
		::ReleaseDC( GetHwnd(), hdc );
		CommitGdiPaintBoundary();
	}

	// キャレットの表示
	GetCaret().ShowEditCaret();

	// キャレットの行桁位置を表示する
	GetCaret().ShowCaretPosInfo();

	// 親ウィンドウのタイトルを更新
	GetEditWnd().UpdateCaption();

	//	Jul. 9, 2005 genta	選択範囲の情報をステータスバーへ表示
	GetSelectionInfo().PrintSelectionInfoMsg();

	// スクロールバーの状態を更新する
	AdjustScrollBars();
}

// 2001/06/21 Start by asa-o 再描画
void CEditView::Redraw()
{
	if( nullptr == GetHwnd() ){
		return;
	}
	if( !GetDrawSwitch() ){
		return;
	}

	HDC			hdc;
	PAINTSTRUCT	ps;
	MarkRenderDamage(editor::rendering::EEditViewDamage::BaseText);

	hdc = ::GetDC( GetHwnd() );

	::GetClientRect( GetHwnd(), &ps.rcPaint );

	OnPaint( hdc, &ps, FALSE );

	::ReleaseDC( GetHwnd(), hdc );
	CommitGdiPaintBoundary();
}
// 2001/06/21 End

void CEditView::RedrawLines( CLayoutYInt top, CLayoutYInt bottom )
{
	if( nullptr == GetHwnd() ){
		return;
	}
	if( !GetDrawSwitch() ){
		return;
	}

	if( bottom < GetTextArea().GetViewTopLine() ){
		return;
	}
	if( GetTextArea().GetBottomLine() <= top ){
		return;
	}
	HDC			hdc;
	PAINTSTRUCT	ps;
	MarkRenderDamage(editor::rendering::EEditViewDamage::BaseText);

	hdc = GetDC();

	ps.rcPaint.left = 0;
	ps.rcPaint.right = GetTextArea().GetAreaRight();
	ps.rcPaint.top = GetTextArea().GenerateYPx(top);
	ps.rcPaint.bottom = GetTextArea().GenerateYPx(bottom);

	OnPaint( hdc, &ps, FALSE );

	ReleaseDC( hdc );
	CommitGdiPaintBoundary();
}

void MyFillRect(HDC hdc, RECT& re)
{
	::ExtTextOut(hdc, re.left, re.top, ETO_OPAQUE|ETO_CLIPPED, &re, L"", 0, nullptr);
}

HDC CEditView::GetBackImageDC(HDC hdc)
{
	if( hdc == nullptr ) return nullptr;
	if( m_hdcBackImage == nullptr ){
		m_hdcBackImage = ::CreateCompatibleDC(hdc);
	}
	return m_hdcBackImage;
}

void CEditView::DrawBackImage(HDC hdc, RECT& rcPaint, HDC hdcBgImg)
{
#if 0
//	テスト背景パターン
	static int testColorIndex = 0;
	testColorIndex = testColorIndex % 7;
	COLORREF cols[7] = {RGB(255,255,255),
		RGB(200,255,255),RGB(255,200,255),RGB(255,255,200),
		RGB(200,200,255),RGB(255,200,200),RGB(200,255,200),
	};
	COLORREF colorOld = ::SetBkColor(hdc, cols[testColorIndex]);
	MyFillRect(hdc, rcPaint);
	::SetBkColor(hdc, colorOld);
	testColorIndex++;
#else
	CTypeSupport cTextType(this,COLORIDX_TEXT);
	COLORREF colorOld = ::SetBkColor(hdc, cTextType.GetBackColor());
	MyFillRect(hdc, rcPaint);

	const CTextArea& area = GetTextArea();
	const CEditDoc& doc  = *m_pcEditDoc;
	const STypeConfig& typeConfig = doc.m_cDocType.GetDocumentAttribute();

	CMyRect rcImagePos;
	switch( typeConfig.m_backImgPos ){
	case BGIMAGE_TOP_LEFT:
	case BGIMAGE_BOTTOM_LEFT:
	case BGIMAGE_CENTER_LEFT:
		rcImagePos.left = area.GetAreaLeft();
		break;
	case BGIMAGE_TOP_RIGHT:
	case BGIMAGE_BOTTOM_RIGHT:
	case BGIMAGE_CENTER_RIGHT:
		rcImagePos.left = area.GetAreaRight() - doc.m_nBackImgWidth;
		break;
	case BGIMAGE_TOP_CENTER:
	case BGIMAGE_BOTTOM_CENTER:
	case BGIMAGE_CENTER:
		rcImagePos.left = area.GetAreaLeft() + area.GetAreaWidth()/2 - doc.m_nBackImgWidth/2;
		break;
	default:
		assert_warning(0 != typeConfig.m_backImgPos);
		break;
	}
	switch( typeConfig.m_backImgPos ){
	case BGIMAGE_TOP_LEFT:
	case BGIMAGE_TOP_RIGHT:
	case BGIMAGE_TOP_CENTER:
		rcImagePos.top  = area.GetAreaTop();
		break;
	case BGIMAGE_BOTTOM_LEFT:
	case BGIMAGE_BOTTOM_RIGHT:
	case BGIMAGE_BOTTOM_CENTER:
		rcImagePos.top  = area.GetAreaBottom() - doc.m_nBackImgHeight;
		break;
	case BGIMAGE_CENTER_LEFT:
	case BGIMAGE_CENTER_RIGHT:
	case BGIMAGE_CENTER:
		rcImagePos.top  = area.GetAreaTop() + area.GetAreaHeight()/2 - doc.m_nBackImgHeight/2;
		break;
	default:
		assert_warning(0 != typeConfig.m_backImgPos);
		break;
	}
	rcImagePos.left += typeConfig.m_backImgPosOffset.x;
	rcImagePos.top  += typeConfig.m_backImgPosOffset.y;
	// スクロール時の画面の端を作画するときの位置あたりへ移動
	if( typeConfig.m_backImgScrollX ){
		int tile = typeConfig.m_backImgRepeatX ? doc.m_nBackImgWidth : INT_MAX;
		Int posX = (area.GetViewLeftCol() % tile) * GetTextMetrics().GetCharPxWidth();
		rcImagePos.left -= posX % tile;
	}
	if( typeConfig.m_backImgScrollY ){
		int tile = typeConfig.m_backImgRepeatY ? doc.m_nBackImgHeight : INT_MAX;
		Int posY = (area.GetViewTopLine() % tile) * GetTextMetrics().GetHankakuDy();
		rcImagePos.top -= posY % tile;
	}
	if( typeConfig.m_backImgRepeatX ){
		if( 0 < rcImagePos.left ){
			// rcImagePos.left = rcImagePos.left - (rcImagePos.left / doc.m_nBackImgWidth + 1) * doc.m_nBackImgWidth;
			rcImagePos.left = rcImagePos.left % doc.m_nBackImgWidth - doc.m_nBackImgWidth;
		}
	}
	if( typeConfig.m_backImgRepeatY ){
		if( 0 < rcImagePos.top ){
			// rcImagePos.top = rcImagePos.top - (rcImagePos.top / doc.m_nBackImgHeight + 1) * doc.m_nBackImgHeight;
			rcImagePos.top = rcImagePos.top % doc.m_nBackImgHeight - doc.m_nBackImgHeight;
		}
	}
	rcImagePos.SetSize(doc.m_nBackImgWidth, doc.m_nBackImgHeight);

	RECT rc = rcPaint;
	// rc.left = t_max((int)rc.left, area.GetAreaLeft());
	rc.top  = t_max((int)rc.top,  area.GetRulerHeight()); // ルーラーを除外
	const int nXEnd = area.GetAreaRight();
	const int nYEnd = area.GetAreaBottom();
	CMyRect rcBltAll;
	rcBltAll.SetLTRB(INT_MAX, INT_MAX, -INT_MAX, -INT_MAX);
	CMyRect rcImagePosOrg = rcImagePos;
	BLENDFUNCTION bf;
	bf.BlendOp = AC_SRC_OVER;
	bf.BlendFlags = 0;
	bf.SourceConstantAlpha = typeConfig.m_backImgOpacity;
	bf.AlphaFormat = AC_SRC_ALPHA;
	for(; rcImagePos.top <= nYEnd; ){
		for(; rcImagePos.left <= nXEnd; ){
			CMyRect rcBlt;
			if( ::IntersectRect(&rcBlt, &rc, &rcImagePos) ){
				int width = rcBlt.right  - rcBlt.left;
				int height = rcBlt.bottom - rcBlt.top;
				::AlphaBlend(
					hdc,
					rcBlt.left,
					rcBlt.top,
					width,
					height,
					hdcBgImg,
					rcBlt.left - rcImagePos.left,
					rcBlt.top - rcImagePos.top,
					width,
					height,
					bf
				);
				rcBltAll.left   = t_min(rcBltAll.left,   rcBlt.left);
				rcBltAll.top    = t_min(rcBltAll.top,    rcBlt.top);
				rcBltAll.right  = t_max(rcBltAll.right,  rcBlt.right);
				rcBltAll.bottom = t_max(rcBltAll.bottom, rcBlt.bottom);
			}
			rcImagePos.left  += doc.m_nBackImgWidth;
			rcImagePos.right += doc.m_nBackImgWidth;
			if( !typeConfig.m_backImgRepeatX ){
				break;
			}
		}
		rcImagePos.left  = rcImagePosOrg.left;
		rcImagePos.right = rcImagePosOrg.right;
		rcImagePos.top    += doc.m_nBackImgHeight;
		rcImagePos.bottom += doc.m_nBackImgHeight;
		if( !typeConfig.m_backImgRepeatY ){
			break;
		}
	}
	if( rcBltAll.left != INT_MAX ){
		// 上下左右ななめの隙間を埋める
		CMyRect rcFill;
		LONG& x1 = rc.left;
		LONG& x2 = rcBltAll.left;
		LONG& x3 = rcBltAll.right;
		LONG& x4 = rc.right;
		LONG& y1 = rc.top;
		LONG& y2 = rcBltAll.top;
		LONG& y3 = rcBltAll.bottom;
		LONG& y4 = rc.bottom;
		if( y1 < y2 ){
			rcFill.SetLTRB(x1,y1, x4,y2); MyFillRect(hdc, rcFill);
		}
		if( x1 < x2 ){
			rcFill.SetLTRB(x1,y2, x2,y3); MyFillRect(hdc, rcFill);
		}
		if( x3 < x4 ){
			rcFill.SetLTRB(x3,y2, x4,y3); MyFillRect(hdc, rcFill);
		}
		if( y3 < y4 ){
			rcFill.SetLTRB(x1,y3, x4,y4); MyFillRect(hdc, rcFill);
		}
	}
	::SetBkColor(hdc, colorOld);
#endif
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                          色設定                             //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*! 指定位置のColorIndexの取得
	CEditView::DrawLogicLineを元にしたためCEditView::DrawLogicLineに
	修正があった場合は、ここも修正が必要。
*/
CColor3Setting CEditView::GetColorIndex(
	const CLayout*			pcLayout,
	CLayoutYInt				nLineNum,
	int						nIndex,
	SColorStrategyInfo* 	pInfo,			// 2010.03.31 ryoji 追加
	bool					bPrev			// 指定位置の色変更直前まで	2010.06.19 ryoji 追加
)
{
	EColorIndexType eRet = COLORIDX_TEXT;

	if(!pcLayout){
		CColor3Setting cColor = { COLORIDX_TEXT, COLORIDX_TEXT, COLORIDX_TEXT };
		return cColor;
	}
	// 2014.12.30 Skipモードの時もCOLORIDX_TEXT
	if (CColorStrategyPool::getInstance()->IsSkipBeforeLayout()) {
		CColor3Setting cColor = { COLORIDX_TEXT, COLORIDX_TEXT, COLORIDX_TEXT };
		return cColor;
	}

	const CLayoutColorInfo* colorInfo;
	const CLayout* pcLayoutLineFirst = pcLayout;
	CLayoutYInt nLineNumFirst = nLineNum;
	{
		// 2002/2/10 aroka CMemory変更
		pInfo->m_pLineOfLogic = pcLayout->GetDocLineRef()->GetPtr();

		// 論理行の最初のレイアウト情報を取得 -> pcLayoutLineFirst
		while( 0 != pcLayoutLineFirst->GetLogicOffset() ){
			pcLayoutLineFirst = pcLayoutLineFirst->GetPrevLayout();
			nLineNumFirst--;

			// 論理行の先頭まで戻らないと確実には正確な色は得られない
			// （正規表現キーワードにマッチした長い強調表示がその位置のレイアウト行頭をまたいでいる場合など）
			//if( pcLayout->GetLogicOffset() - pcLayoutLineFirst->GetLogicOffset() > 260 )
			//	break;
		}

		// 2005.11.20 Moca 色が正しくないことがある問題に対処
		eRet = pcLayoutLineFirst->GetColorTypePrev();	/* 現在の色を指定 */	// 02/12/18 ai
		colorInfo = pcLayoutLineFirst->GetColorInfo();
		pInfo->m_nPosInLogic = pcLayoutLineFirst->GetLogicOffset();

		//CColorStrategyPool初期化
		CColorStrategyPool* pool = CColorStrategyPool::getInstance();
		pool->SetCurrentView(this);
		pool->NotifyOnStartScanLogic();

		// 2009.02.07 ryoji この関数では pInfo->CheckChangeColor() で色を調べるだけなので以下の処理は不要
		//
		////############超仮。本当はVisitorを使うべき
		//class TmpVisitor{
		//public:
		//	static int CalcLayoutIndex(const CLayout* pcLayout)
		//	{
		//		int n = -1;
		//		while(pcLayout){
		//			pcLayout = pcLayout->GetPrevLayout(); //prev or null
		//			n++;
		//		}
		//		return n;
		//	}
		//};
		//pInfo->pDispPos->SetLayoutLineRef(CLayoutInt(TmpVisitor::CalcLayoutIndex(pcLayout)));
		// 2013.12.11 Moca カレント行の色替えで必要になりました
		pInfo->m_pDispPos->SetLayoutLineRef(nLineNumFirst);
	}

	//文字列参照
	const CDocLine* pcDocLine = pcLayout->GetDocLineRef();
	CStringRef cLineStr(pcDocLine->GetPtr(),pcDocLine->GetLengthWithEOL());

	//color strategy
	CColorStrategyPool* pool = CColorStrategyPool::getInstance();
	pInfo->m_pStrategy = pool->GetStrategyByColor(eRet);
	if(pInfo->m_pStrategy){
		pInfo->m_pStrategy->InitStrategyStatus();
		pInfo->m_pStrategy->SetStrategyColorInfo(colorInfo);
	}

	const CLayout* pcLayoutNext = pcLayoutLineFirst->GetNextLayout();
	CLayoutYInt nLineNumScan = nLineNumFirst;
	int nPosTo = pcLayout->GetLogicOffset() + t_min(nIndex, (int)pcLayout->GetLengthWithEOL() - 1);
	while(pInfo->m_nPosInLogic <= nPosTo){
		if( bPrev && pInfo->m_nPosInLogic == nPosTo )
			break;

		//色切替
		pInfo->CheckChangeColor(cLineStr);

		//1文字進む
		pInfo->m_nPosInLogic += CNativeW::GetSizeOfChar(
									cLineStr.GetPtr(),
									cLineStr.GetLength(),
									pInfo->m_nPosInLogic
								);
		if( pcLayoutNext && pcLayoutNext->GetLogicOffset() <= pInfo->m_nPosInLogic ){
			nLineNumScan++;
			pInfo->m_pDispPos->SetLayoutLineRef(nLineNumScan);
			pcLayoutNext = pcLayoutNext->GetNextLayout();
		}
	}

	CColor3Setting cColor;
	pInfo->DoChangeColor(&cColor);

	return cColor;
}

/*! 現在の色を指定
	@param eColorIndex   選択を含む現在の色
	@param eColorIndex2  選択以外の現在の色
	@param eColorIndexBg 背景色

	@date 2013.05.08 novice 範囲外チェック削除
*/
inline COLORREF MakeColor2(COLORREF a, COLORREF b, int alpha);

void CEditView::SetCurrentColor( CGraphics& gr, EColorIndexType eColorIndex,  EColorIndexType eColorIndex2, EColorIndexType eColorIndexBg)
{
	//インデックス決定
	int		nColorIdx = ToColorInfoArrIndex(eColorIndex);
	int		nColorIdx2 = ToColorInfoArrIndex(eColorIndex2);
	int		nColorIdxBg = ToColorInfoArrIndex(eColorIndexBg);

	//実際に色を設定
	const ColorInfo& info  = m_pTypeData->m_ColorInfoArr[nColorIdx];
	const ColorInfo& info2 = m_pTypeData->m_ColorInfoArr[nColorIdx2];
	const ColorInfo& infoBg = m_pTypeData->m_ColorInfoArr[nColorIdxBg];
	COLORREF fgcolor = GetTextColorByColorInfo2(info, info2);
	// 2012.11.21 背景色がテキストとおなじなら背景色はカーソル行背景
	const ColorInfo& info3 = (info2.m_sColorAttr.m_cBACK == m_crBack ? infoBg : info2);
	COLORREF bkcolor = (nColorIdx == nColorIdx2) ? info3.m_sColorAttr.m_cBACK : GetBackColorByColorInfo2(info, info3);
	const theme::ThemeSyntaxStyle* syntaxStyle = nullptr;
	if (eColorIndex != COLORIDX_SELECT && eColorIndex2 != COLORIDX_SELECT) {
		if (const auto* syntaxPalette = theme::CThemeService::ActiveColorThemeSyntaxPalette()) {
			auto syntaxKind = SyntaxKindForColorIndex(eColorIndex);
			if (!syntaxKind) syntaxKind = SyntaxKindForColorIndex(eColorIndex2);
			if (syntaxKind) syntaxStyle = &syntaxPalette->For(*syntaxKind);
		}
	}
	const auto* editorPalette = theme::CThemeService::ActiveColorThemePalette();
	if (editorPalette != nullptr && eColorIndex != COLORIDX_SELECT
		&& eColorIndex2 != COLORIDX_SELECT) {
		// TextMate tokens inherit editor.background. The legacy type palette may
		// still be dark while a Light workbench theme is active, so establish the
		// editor canvas before layering projected syntax colors over it.
		bkcolor = editorPalette->canvas.ToColorRef();
		if (eColorIndex == COLORIDX_TEXT || eColorIndex2 == COLORIDX_TEXT
			|| syntaxStyle != nullptr) {
			fgcolor = editorPalette->primaryText.ToColorRef();
		}
	}
	const auto applyThemeColor = [](const std::optional<theme::ThemeColor>& color, COLORREF base) noexcept {
		if (!color || color->alpha == 0) return base;
		if (color->alpha == 0xFF) return color->ToColorRef();
		return MakeColor2(color->ToColorRef(), base, color->alpha);
	};
	if (syntaxStyle != nullptr) {
		bkcolor = applyThemeColor(syntaxStyle->background, bkcolor);
		fgcolor = applyThemeColor(syntaxStyle->foreground, fgcolor);
	}
	if( m_bMiniMap && editorPalette != nullptr ){
		bkcolor = editorPalette->minimapBackground.ToColorRef();
		fgcolor = MakeColor2(fgcolor, bkcolor,
			editorPalette->minimapForegroundOpacity.alpha);
	}
	gr.SetTextForeColor(fgcolor);
	gr.SetTextBackColor(bkcolor);
	SFONT sFont;
	sFont.m_sFontAttr = (info.m_sColorAttr.m_cTEXT != info.m_sColorAttr.m_cBACK) ? info.m_sFontAttr : info2.m_sFontAttr;
	if (syntaxStyle != nullptr) {
		sFont.m_sFontAttr.m_bBoldFont = sFont.m_sFontAttr.m_bBoldFont || syntaxStyle->bold;
		sFont.m_sFontAttr.m_bUnderLine = sFont.m_sFontAttr.m_bUnderLine || syntaxStyle->underline;
	}
	sFont.m_hFont = GetFontset().ChooseFontHandle( 0, sFont.m_sFontAttr );
	gr.SetMyFont(sFont);
}

const std::vector<textmate::TextMateToken>* CEditView::GetTextMateTokens(
	CLogicInt logicalLine)
{
	if( logicalLine < 0 || m_pcEditDoc == nullptr ) return nullptr;
	auto* service = GetEditWnd().GetSenpLanguageService();
	if( service == nullptr ) {
		m_textMateSession.reset();
		m_textMateLines.clear();
		return nullptr;
	}

	const std::uint64_t layoutGeneration =
		m_pcEditDoc->m_cLayoutMgr.GetLayoutGeneration();
	const std::uint64_t serviceRevision = service->Revision();
	const std::wstring resourcePath = m_pcEditDoc->m_cDocFile.GetFilePath();
	if( m_textMateLayoutGeneration != layoutGeneration
		|| m_textMateServiceRevision != serviceRevision
		|| m_textMateResourcePath != resourcePath ) {
		m_textMateSession.reset();
		m_textMateLines.clear();
		m_textMateLayoutGeneration = layoutGeneration;
		m_textMateServiceRevision = serviceRevision;
		m_textMateResourcePath = resourcePath;

		const CDocLine* first = m_pcEditDoc->m_cDocLineMgr.GetLine(CLogicInt(0));
		const std::wstring_view firstLine = first == nullptr
			? std::wstring_view{}
			: std::wstring_view(first->GetPtr(),
				static_cast<std::size_t>(first->GetLengthWithoutEOL()));
		m_textMateSession = service->CreateSession(resourcePath, firstLine);
	}
	if( !m_textMateSession ) return nullptr;

	const std::size_t requested = static_cast<std::size_t>(static_cast<int>(logicalLine));
	while( m_textMateLines.size() <= requested && m_textMateTokenizeBudget > 0 ) {
		const auto sourceLine = static_cast<int>(m_textMateLines.size());
		const CDocLine* line = m_pcEditDoc->m_cDocLineMgr.GetLine(CLogicInt(sourceLine));
		if( line == nullptr ) break;
		const std::wstring_view text(line->GetPtr(),
			static_cast<std::size_t>(line->GetLengthWithoutEOL()));
		const textmate::RuleStackHandle previous = m_textMateLines.empty()
			? m_textMateSession->InitialState()
			: m_textMateLines.back().stateAfter;
		textmate::RuleStackHandle next;
		auto tokenized = m_textMateSession->TokenizeLine(text, previous, next);
		m_textMateLines.push_back({ std::move(next), std::move(tokenized.tokens) });
		--m_textMateTokenizeBudget;
	}
	if( requested >= m_textMateLines.size() ) {
		// TextMate state is line-ordered. Continue in later paint turns instead of
		// blocking a large jump into a long file on one synchronous scan.
		::InvalidateRect(GetHwnd(), nullptr, FALSE);
		return nullptr;
	}
	return &m_textMateLines[requested].tokens;
}

void CEditView::ApplyTextMateTokenStyle(SColorStrategyInfo& info,
	const textmate::TextMateToken* token)
{
	if( token == nullptr || info.GetCurrentColor() != info.GetCurrentColor2() ) return;
	const EColorIndexType underlying = info.GetCurrentColor2();
	if( underlying >= COLORIDX_SEARCH && underlying <= COLORIDX_SEARCHTAIL ) return;
	const auto* tokenColors = theme::CThemeService::ActiveColorThemeTokenColors();
	if( tokenColors == nullptr ) return;
	const auto style = theme::TextMateScopeColorResolver::Resolve(token->scopes, *tokenColors);
	if( !style.matched ) return;
	const auto apply = [](const theme::ThemeColor& color, COLORREF base) noexcept {
		if( color.alpha == 0 ) return base;
		return color.alpha == 0xFF ? color.ToColorRef()
			: MakeColor2(color.ToColorRef(), base, color.alpha);
	};
	if( style.foreground ) {
		COLORREF foreground = apply(*style.foreground, ::GetTextColor(info.m_gr));
		if( m_bMiniMap ) {
			if( const auto* editorPalette = theme::CThemeService::ActiveColorThemePalette() ) {
				foreground = MakeColor2(foreground,
					editorPalette->minimapBackground.ToColorRef(),
					editorPalette->minimapForegroundOpacity.alpha);
			}
		}
		info.m_gr.SetTextForeColor(foreground);
	}
	if( style.background ) {
		info.m_gr.SetTextBackColor(apply(*style.background, ::GetBkColor(info.m_gr)));
	}
}

inline COLORREF MakeColor2(COLORREF a, COLORREF b, int alpha)
{
#ifdef USE_SSE2
	// (a * alpha + b * (256 - alpha)) / 256 -> ((a - b) * alpha) / 256 + b
	__m128i xmm0, xmm1, xmm2, xmm3;
	COLORREF color;
	xmm0 = _mm_setzero_si128();
	xmm1 = _mm_cvtsi32_si128( a );
	xmm2 = _mm_cvtsi32_si128( b );
	xmm3 = _mm_cvtsi32_si128( alpha );

	xmm1 = _mm_unpacklo_epi8( xmm1, xmm0 ); // a:a:a:a
	xmm2 = _mm_unpacklo_epi8( xmm2, xmm0 ); // b:b:b:b
	xmm3 = _mm_shufflelo_epi16( xmm3, 0 ); // alpha:alpha:alpha:alpha

	xmm1 = _mm_sub_epi16( xmm1, xmm2 ); // (a - b)
	xmm1 = _mm_mullo_epi16( xmm1, xmm3 ); // (a - b) * alpha
	xmm1 = _mm_srli_epi16( xmm1, 8 ); // ((a - b) * alpha) / 256
	xmm1 = _mm_add_epi8( xmm1, xmm2 ); // ((a - b) * alpha) / 256 + b

	xmm1 = _mm_packus_epi16( xmm1, xmm0 );
	color = _mm_cvtsi128_si32( xmm1 );

	return color;
#else
	const int ap = alpha;
	const int bp = 256 - ap;
	BYTE valR = (BYTE)((GetRValue(a) * ap + GetRValue(b) * bp) / 256);
	BYTE valG = (BYTE)((GetGValue(a) * ap + GetGValue(b) * bp) / 256);
	BYTE valB = (BYTE)((GetBValue(a) * ap + GetBValue(b) * bp) / 256);
	return RGB(valR, valG, valB);
#endif
}

COLORREF CEditView::GetTextColorByColorInfo2(const ColorInfo& info, const ColorInfo& info2)
{
	if( info.m_sColorAttr.m_cTEXT != info.m_sColorAttr.m_cBACK ){
		return info.m_sColorAttr.m_cTEXT;
	}
	// 反転表示
	if( info.m_sColorAttr.m_cBACK == m_crBack ){
		return  info2.m_sColorAttr.m_cTEXT ^ 0x00FFFFFF;
	}
	int alpha = 255*30/100; // 30%
	return MakeColor2(info.m_sColorAttr.m_cTEXT, info2.m_sColorAttr.m_cTEXT, alpha);
}

COLORREF CEditView::GetBackColorByColorInfo2(const ColorInfo& info, const ColorInfo& info2)
{
	if( info.m_sColorAttr.m_cTEXT != info.m_sColorAttr.m_cBACK ){
		return info.m_sColorAttr.m_cBACK;
	}
	// 反転表示
	if( info.m_sColorAttr.m_cBACK == m_crBack ){
		return  info2.m_sColorAttr.m_cBACK ^ 0x00FFFFFF;
	}
	int alpha = 255*30/100; // 30%
	return MakeColor2(info.m_sColorAttr.m_cBACK, info2.m_sColorAttr.m_cBACK, alpha);
}

void CEditView::DrawMiniMapFrame(HDC hdc)
{
	if( !m_bMiniMap || hdc == nullptr ) return;
	RECT client{};
	if( !GetClientRect(&client) ) return;
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if( width <= 0 || height <= 0 ) return;

	CTypeSupport textType(this, COLORIDX_TEXT);
	const auto* activePalette = theme::CThemeService::ActiveColorThemePalette();
	const COLORREF background = activePalette != nullptr
		? activePalette->minimapBackground.ToColorRef() : textType.GetBackColor();
	const auto pixelCount = static_cast<std::size_t>(width)
		* static_cast<std::size_t>(height);
	m_miniMapFramePixels.assign(pixelCount, MiniMapDibPixel(background));
	const bool autoHidden = m_miniMapOptions.autohide == minimap::AutoHide::MouseOver
		? !m_bMiniMapMouseOver && !m_bMiniMapMouseDown
		: m_miniMapOptions.autohide == minimap::AutoHide::Scroll
			&& !m_bMiniMapScrollVisible && !m_bMiniMapMouseOver && !m_bMiniMapMouseDown;
	if( autoHidden ) {
		PresentMiniMapPixels(hdc, client, m_miniMapFramePixels);
		return;
	}

	const auto lineCount = static_cast<std::int64_t>(m_pcEditDoc->m_cLayoutMgr.GetLineCount());
	if( lineCount <= 0 ) {
		PresentMiniMapPixels(hdc, client, m_miniMapFramePixels);
		return;
	}

	const auto geometry = CalculateMiniMapLayout();
	if( geometry.visibleLineSpan <= 0 ) {
		PresentMiniMapPixels(hdc, client, m_miniMapFramePixels);
		return;
	}
	// TextMate rule stacks are line-ordered. Advance the minimap's own session
	// in bounded paint turns, but keep the already complete overview raster while
	// catch-up is pending. Once the final logical line is available the readiness
	// bit below changes and the overview is rebuilt exactly once with token colors.
	const auto* languageService = GetEditWnd().GetSenpLanguageService();
	const std::uint64_t languageServiceRevision = languageService == nullptr
		? 0 : languageService->Revision();
	bool textMateAvailable = false;
	bool textMateReady = true;
	const int logicalLineCount = static_cast<int>(
		m_pcEditDoc->m_cDocLineMgr.GetLineCount());
	if( logicalLineCount > 0 ) {
		const auto* finalLineTokens = GetTextMateTokens(CLogicInt(logicalLineCount - 1));
		textMateAvailable = m_textMateSession != nullptr;
		textMateReady = !textMateAvailable || finalLineTokens != nullptr;
	}
	std::uint64_t styleFingerprint = 1469598103934665603ULL;
	const auto mixStyle = [&styleFingerprint](std::uint64_t value) noexcept {
		styleFingerprint ^= value;
		styleFingerprint *= 1099511628211ULL;
	};
	for( int index = 0; index < COLORIDX_LAST; ++index ) {
		const auto& color = m_pTypeData->m_ColorInfoArr[index];
		mixStyle(color.m_sColorAttr.m_cTEXT);
		mixStyle(color.m_sColorAttr.m_cBACK);
		mixStyle(color.m_sFontAttr.m_bBoldFont ? 1U : 0U);
	}
	const LOGFONT& miniMapFont = m_pcViewFont->GetLogfont();
	mixStyle(static_cast<std::uint64_t>(miniMapFont.lfWeight));
	mixStyle(static_cast<std::uint64_t>(miniMapFont.lfItalic));
	mixStyle(static_cast<std::uint64_t>(miniMapFont.lfCharSet));
	mixStyle(static_cast<std::uint64_t>(miniMapFont.lfPitchAndFamily));
	for( const wchar_t character : miniMapFont.lfFaceName ) {
		if( character == L'\0' ) break;
		mixStyle(static_cast<std::uint64_t>(character));
	}
	mixStyle(languageServiceRevision);
	mixStyle(textMateAvailable ? 1U : 0U);
	mixStyle(textMateReady ? 1U : 0U);
	const auto layoutGeneration = m_pcEditDoc->m_cLayoutMgr.GetLayoutGeneration();
	const auto cacheMatches = m_miniMapOverviewCache.valid
		&& m_miniMapOverviewCache.document == m_pcEditDoc
		&& m_miniMapOverviewCache.layoutGeneration == layoutGeneration
		&& m_miniMapOverviewCache.styleFingerprint == styleFingerprint
		&& m_miniMapOverviewCache.lineCount == lineCount
		&& m_miniMapOverviewCache.width == width
		&& m_miniMapOverviewCache.height == height
		&& m_miniMapOverviewCache.background == background
		&& minimap::HasSameOverviewRendering(
			m_miniMapOverviewCache.options, m_miniMapOptions)
		&& minimap::HasSameOverviewIdentity(
			m_miniMapOverviewCache.geometry, geometry);
	if( !cacheMatches ) {
		const bool traceCache = CStartupTrace::IsCollectingStartupDocumentMetrics();
		LARGE_INTEGER cacheBuildStart{};
		if (traceCache) {
			::QueryPerformanceCounter(&cacheBuildStart);
		}
		// Build into a local value and publish only after the complete overview is
		// ready, so paint never observes a partially rebuilt cache.
		MiniMapOverviewCache next;
		next.document = m_pcEditDoc;
		next.layoutGeneration = layoutGeneration;
		next.styleFingerprint = styleFingerprint;
		next.lineCount = lineCount;
		next.width = width;
		next.height = height;
		next.background = background;
		next.options = m_miniMapOptions;
		next.geometry = geometry;
		next.pixels.assign(pixelCount, MiniMapDibPixel(background));
		const auto dpi = GetHwnd() == nullptr ? 96U : ::GetDpiForWindow(GetHwnd());
		const int glyphScale = (std::max)(1,
			::MulDiv((std::clamp)(m_miniMapOptions.scale, 1, 3), static_cast<int>(dpi), 96));
		const int leftPadding = (std::max)(2, ::MulDiv(4, static_cast<int>(dpi), 96));
		const int maxColumn = (std::clamp)(m_miniMapOptions.maxColumn, 1, 10000);
		const int tabSize = (std::max)(1,
			static_cast<int>(m_pcEditDoc->m_cLayoutMgr.GetTabSpaceKetas()));
		MiniMapCharAtlas characterAtlas;
		const bool drawCharacterGlyphs = m_miniMapOptions.renderCharacters
			&& characterAtlas.Create(hdc, miniMapFont, glyphScale);
		CGraphics graphics(hdc);
		std::int64_t visitedLines = 0;
		std::int64_t previousSourceLine = -1;
		const int rowCount = geometry.sampled
			? geometry.height
			: static_cast<int>(geometry.visibleLineSpan);
		for( int row = 0; row < rowCount; ++row ) {
			const std::int64_t sourceLine = geometry.sampled
				? geometry.YToLine(row)
				: geometry.firstLine + row;
			if( sourceLine == previousSourceLine || sourceLine >= geometry.LastLineExclusive() ) continue;
			previousSourceLine = sourceLine;
			const CLayout* layout = m_pcEditDoc->m_cLayoutMgr.SearchLineByLayoutY(
				CLayoutInt(static_cast<int>((std::min)(sourceLine,
					static_cast<std::int64_t>(INT_MAX)))));
			if( layout == nullptr ) continue;
			++visitedLines;
			const int lineTop = geometry.sampled ? row : geometry.LineToY(sourceLine);
			const int lineBottom = (std::min)(height,
				lineTop + (std::max)(1, geometry.lineHeight));
			if( lineTop >= height || lineBottom <= 0 ) continue;

			DispPos position(1, 1);
			position.SetLayoutLineRef(CLayoutInt(static_cast<int>(sourceLine)));
			SColorStrategyInfo colorInfo(hdc);
			colorInfo.m_pcView = this;
			colorInfo.m_pDispPos = &position;
			CColor3Setting currentColor = GetColorIndex(layout,
				CLayoutYInt(static_cast<int>(sourceLine)), 0, &colorInfo, true);
			colorInfo.m_nPosInLogic = layout->GetLogicOffset();
			SetCurrentColor(graphics, currentColor.eColorIndex2,
				currentColor.eColorIndex2, COLORIDX_TEXT);
			COLORREF characterColor = ::GetTextColor(hdc);
			int visualColumn = (std::max)(0, static_cast<int>(layout->GetIndent()));

			const wchar_t* lineText = layout->GetDocLineRef()->GetPtr();
			const int logicalLength = static_cast<int>(layout->GetDocLineRef()->GetLengthWithEOL());
			const int begin = static_cast<int>(layout->GetLogicOffset());
			const int end = (std::min)(logicalLength,
				begin + static_cast<int>(layout->GetLengthWithoutEOL()));
			const auto* textMateTokens = textMateReady
				? GetTextMateTokens(CLayout::GetLogicLineNo_Safe(layout)) : nullptr;
			std::size_t textMateIndex = 0;
			const auto tokenAt = [&](int offset) -> const textmate::TextMateToken* {
				if( textMateTokens == nullptr || offset < 0 ) return nullptr;
				const auto tokenPosition = static_cast<std::size_t>(offset);
				while( textMateIndex < textMateTokens->size()
					&& (*textMateTokens)[textMateIndex].utf16End <= tokenPosition ) {
					++textMateIndex;
				}
				if( textMateIndex >= textMateTokens->size() ) return nullptr;
				const auto& token = (*textMateTokens)[textMateIndex];
				return token.utf16Start <= tokenPosition && tokenPosition < token.utf16End
					? &token : nullptr;
			};
			const textmate::TextMateToken* activeTextMateToken = tokenAt(begin);
			ApplyTextMateTokenStyle(colorInfo, activeTextMateToken);
			characterColor = ::GetTextColor(hdc);
			for( int offset = begin; offset < end && visualColumn < maxColumn; ) {
				colorInfo.m_nPosInLogic = CLogicInt(offset);
				const textmate::TextMateToken* nextTextMateToken = tokenAt(offset);
				if( nextTextMateToken != activeTextMateToken ) {
					SetCurrentColor(graphics, currentColor.eColorIndex2,
						currentColor.eColorIndex2, COLORIDX_TEXT);
					activeTextMateToken = nextTextMateToken;
					ApplyTextMateTokenStyle(colorInfo, activeTextMateToken);
					characterColor = ::GetTextColor(hdc);
				}
				if( colorInfo.CheckChangeColor(CStringRef(lineText, logicalLength)) ) {
					colorInfo.DoChangeColor(&currentColor);
					SetCurrentColor(graphics, currentColor.eColorIndex2,
						currentColor.eColorIndex2, COLORIDX_TEXT);
					ApplyTextMateTokenStyle(colorInfo, activeTextMateToken);
					characterColor = ::GetTextColor(hdc);
				}
				const wchar_t character = lineText[offset];
				const int characterSize = (std::max)(1, static_cast<int>(
					CNativeW::GetSizeOfChar(lineText, logicalLength, offset)));
				if( character == L'\t' ) {
					visualColumn += tabSize - visualColumn % tabSize;
				} else {
					const int characterColumns = (std::clamp)(static_cast<int>(
						m_pcEditDoc->m_cLayoutMgr.GetLayoutXOfChar(
							lineText, logicalLength, offset)), 1, 2);
					const bool whitespace = character == L' ' || character == L'\r'
						|| character == L'\n' || character == L'\0';
					const int glyphLeft = leftPadding + visualColumn * glyphScale;
					const int glyphAdvance = characterColumns * glyphScale;
					const int glyphRight = glyphLeft + glyphAdvance;
					if( glyphLeft >= width ) break;
					if( drawCharacterGlyphs && !whitespace ) {
						(void)characterAtlas.Paint(next.pixels, width, height,
							glyphLeft, lineTop, lineBottom - lineTop,
							character, characterColor);
					} else if( !whitespace && glyphRight > glyphLeft ) {
						// If the character DIB cannot be created, fail visibly as the
						// same block renderer used by renderCharacters=false.
						PaintMiniMapBlock(next.pixels, width, height,
							glyphLeft, lineTop, characterColumns, glyphScale,
							lineBottom - lineTop, characterColor);
					}
					visualColumn += characterColumns;
				}
				offset += characterSize;
			}
		}

		next.valid = true;
		m_miniMapOverviewCache = std::move(next);
		if (traceCache) {
			LARGE_INTEGER cacheBuildEnd{};
			::QueryPerformanceCounter(&cacheBuildEnd);
			CStartupTrace::AccumulateStartupMiniMapCacheLookup(
				false, cacheBuildEnd.QuadPart - cacheBuildStart.QuadPart, visitedLines);
		}
	} else {
		CStartupTrace::AccumulateStartupMiniMapCacheLookup(true);
	}
	if( m_miniMapOverviewCache.pixels.size()
		== static_cast<std::size_t>(width) * static_cast<std::size_t>(height) ) {
		m_miniMapFramePixels = m_miniMapOverviewCache.pixels;
	}
	const bool sliderVisible = m_miniMapOptions.showSlider == minimap::ShowSlider::Always
		|| m_bMiniMapMouseOver || m_bMiniMapMouseDown;
	if( sliderVisible ) {
		const auto band = geometry.viewport;
		RECT viewport{ client.left, client.top + band.top,
			client.right, client.top + band.bottom };
		viewport.top = (std::clamp)(viewport.top, client.top, client.bottom);
		viewport.bottom = (std::clamp)(viewport.bottom, client.top, client.bottom);
		if( viewport.bottom <= viewport.top ) {
			viewport.bottom = (std::min)(client.bottom, viewport.top + 2);
		}
		COLORREF sliderColor = textType.GetTextColor();
		BYTE sliderAlpha = m_bMiniMapMouseDown ? 72 : m_bMiniMapMouseOver ? 48 : 32;
		if( const auto* palette = theme::CThemeService::ActiveColorThemePalette() ) {
			const auto& source = m_bMiniMapMouseDown
				? palette->minimapSliderActiveBackground
				: m_bMiniMapMouseOver
					? palette->minimapSliderHoverBackground
					: palette->minimapSliderBackground;
			sliderColor = source.ToColorRef();
			sliderAlpha = source.alpha;
		}
		BlendMiniMapRectangle(m_miniMapFramePixels, width, height,
			viewport, sliderColor, sliderAlpha);
	}
	// The live window only observes this completed frame. Cache rebuild, slider
	// composition, and hover/drag state never become intermediate screen states.
	PresentMiniMapPixels(hdc, client, m_miniMapFramePixels);
}

void CEditView::SetMiniMapOptions(const minimap::Options& options)
{
	if( m_miniMapOptions == options ) return;
	const bool overviewChanged = !minimap::HasSameOverviewRendering(
		m_miniMapOptions, options);
	m_miniMapOptions = options;
	if( overviewChanged ) m_miniMapOverviewCache = {};
	if( GetHwnd() != nullptr ) {
		::InvalidateRect(GetHwnd(), nullptr, FALSE);
	}
}

void CEditView::SetIndentGuidesEnabled(bool enabled)
{
	if( m_indentGuidesEnabled == enabled ) return;
	m_indentGuidesEnabled = enabled;
	if( GetHwnd() != nullptr ){
		::InvalidateRect(GetHwnd(), nullptr, FALSE);
	}
}

minimap::Layout CEditView::CalculateMiniMapLayout() const noexcept
{
	if( !m_bMiniMap || GetHwnd() == nullptr || m_pcEditDoc == nullptr ) return {};
	RECT client{};
	if( !::GetClientRect(GetHwnd(), &client) ) return {};
	const CEditView& activeView = GetEditWnd().GetActiveView();
	auto effectiveOptions = m_miniMapOptions;
	const auto dpi = ::GetDpiForWindow(GetHwnd());
	effectiveOptions.scale = (std::max)(1,
		::MulDiv((std::clamp)(effectiveOptions.scale, 1, 3), static_cast<int>(dpi), 96));
	return minimap::CalculateLayout(effectiveOptions, {
		.lineCount = static_cast<std::int64_t>(m_pcEditDoc->m_cLayoutMgr.GetLineCount()),
		.editorTopLine = static_cast<std::int64_t>(activeView.GetTextArea().GetViewTopLine()),
		.editorVisibleLines = static_cast<std::int64_t>((std::max)(CLayoutInt(1),
			activeView.GetTextArea().GetBottomLine() - activeView.GetTextArea().GetViewTopLine())),
		.height = client.bottom - client.top,
	});
}

bool CEditView::EnsureAlphaOverlaySource(HDC target) noexcept
{
	if( target == nullptr ) return false;
	if( m_hdcAlphaOverlay == nullptr ){
		m_hdcAlphaOverlay = ::CreateCompatibleDC(target);
	}
	if( m_hdcAlphaOverlay == nullptr ) return false;
	if( m_hbmpAlphaOverlay == nullptr ){
		m_hbmpAlphaOverlay = ::CreateCompatibleBitmap(target, 1, 1);
		if( m_hbmpAlphaOverlay == nullptr ) return false;
		m_hbmpAlphaOverlayOld = static_cast<HBITMAP>(
			::SelectObject(m_hdcAlphaOverlay, m_hbmpAlphaOverlay));
		if( m_hbmpAlphaOverlayOld == nullptr || m_hbmpAlphaOverlayOld == HGDI_ERROR ){
			::DeleteObject(m_hbmpAlphaOverlay);
			m_hbmpAlphaOverlay = nullptr;
			m_hbmpAlphaOverlayOld = nullptr;
			return false;
		}
	}
	return true;
}

void CEditView::FillAlphaOverlay(
	HDC target, const RECT& rectangle, COLORREF color, BYTE alpha) noexcept
{
	if( rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top
		|| !EnsureAlphaOverlaySource(target) ) return;
	::SetPixelV(m_hdcAlphaOverlay, 0, 0, color);
	const BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, 0 };
	(void)::AlphaBlend(target, rectangle.left, rectangle.top,
		rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
		m_hdcAlphaOverlay, 0, 0, 1, 1, blend);
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                           描画                              //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

void CEditView::OnPaint( HDC _hdc, PAINTSTRUCT *pPs, BOOL bDrawFromComptibleBmp )
{
	if (GetEditWnd().m_pPrintPreview) {
		return;
	}
	RequestGdiFrame();
	// Both the editor and minimap own a bounded, line-ordered TextMate session.
	// Resetting here lets invalidated paint turns make forward progress without
	// allowing one frame to scan an unbounded document.
	m_textMateTokenizeBudget = 128;
	if( m_bMiniMap ){
		const bool measureStartupPaint = CStartupTrace::IsEnabled()
			&& GetEditWnd().IsStartupDrawCommitting();
		LARGE_INTEGER begin{};
		if (measureStartupPaint) {
			::QueryPerformanceCounter(&begin);
		}
		DrawMiniMapFrame(_hdc);
		if (measureStartupPaint) {
			LARGE_INTEGER end{};
			::QueryPerformanceCounter(&end);
			GetEditWnd().RecordStartupMiniMapPaint(end.QuadPart - begin.QuadPart);
		}
		CaptureNativeSurface(_hdc, pPs->rcPaint);
		return;
	}
	bool bChangeFont = false;
	if( bChangeFont ){
		SelectCharWidthCache( CWM_FONT_MINIMAP, CWM_CACHE_LOCAL );
	}
	const bool traceFirstContentPaint = m_nMyIndex == 0 && CStartupTrace::IsAwaitingFirstContentPaint();
	if (traceFirstContentPaint) {
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintBegin);
	}
	const bool contentPainted = OnPaint2( _hdc, pPs, bDrawFromComptibleBmp );
	CaptureNativeSurface(_hdc, pPs->rcPaint);
	if (traceFirstContentPaint) {
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintEnd, contentPainted ? 1 : 0);
		CStartupTrace::FlushFirstContentPaintMetrics();
	}
	if( bChangeFont ){
		SelectCharWidthCache( CWM_FONT_EDIT, GetEditWnd().GetLogfontCacheMode() );
	}
	if (contentPainted && m_nMyIndex == 0) {
		GetEditWnd().RecordFirstStartupContentPaint();
	}
}

/*! 通常の描画処理 new
	@param pPs  pPs.rcPaint は正しい必要がある
	@param bDrawFromComptibleBmp  TRUE 画面バッファからhdcに作画する(コピーするだけ)。
			TRUEの場合、pPs.rcPaint領域外は作画されないが、FALSEの場合は作画される事がある。
			互換DC/BMPが無い場合は、普通の作画処理をする。

	@date 2007.09.09 Moca 元々無効化されていた第三パラメータのbUseMemoryDCをbDrawFromComptibleBmpに変更。
	@date 2009.03.26 ryoji 行番号のみ描画を通常の行描画と分離（効率化）
*/
bool CEditView::OnPaint2( HDC _hdc, PAINTSTRUCT *pPs, BOOL bDrawFromComptibleBmp )
{
//	MY_RUNNINGTIMER( cRunningTimer, "CEditView::OnPaint" );
	CGraphics gr(_hdc);

	// 2004.01.28 Moca デスクトップに作画しないように
	if( nullptr == GetHwnd() || nullptr == _hdc )return false;

	if( !GetDrawSwitch() )return false;
	//@@@
#if 0
	::MYTRACE( L"OnPaint(%d,%d)-(%d,%d) : %d\n",
		pPs->rcPaint.left,
		pPs->rcPaint.top,
		pPs->rcPaint.right,
		pPs->rcPaint.bottom,
		bDrawFromComptibleBmp
		);
#endif

	// From Here 2007.09.09 Moca 互換BMPによる画面バッファ
	// 互換BMPからの転送のみによる作画
	if( bDrawFromComptibleBmp
		&& m_hdcCompatDC && m_hbmpCompatBMP ){
		::BitBlt(
			gr,
			pPs->rcPaint.left,
			pPs->rcPaint.top,
			pPs->rcPaint.right - pPs->rcPaint.left,
			pPs->rcPaint.bottom - pPs->rcPaint.top,
			m_hdcCompatDC,
			pPs->rcPaint.left,
			pPs->rcPaint.top,
			SRCCOPY
		);
		if ( GetEditWnd().GetActivePane() == m_nMyIndex ){
			/* アクティブペインは、アンダーライン描画 */
			GetCaret().m_cUnderLine.CaretUnderLineON( true, false );
		}
		return false;
	}
	const bool traceFirstContentWork = m_nMyIndex == 0 && CStartupTrace::IsAwaitingFirstContentPaint();
	if (traceFirstContentWork) {
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintPrepareBegin);
	}
	if( (m_hdcCompatDC && nullptr == m_hbmpCompatBMP)
		 || m_nCompatBMPWidth < (pPs->rcPaint.right - pPs->rcPaint.left)
		 || m_nCompatBMPHeight < (pPs->rcPaint.bottom - pPs->rcPaint.top) ){
		RECT rect;
		::GetWindowRect( this->GetHwnd(), &rect );
		CreateOrUpdateCompatibleBitmap( rect.right - rect.left, rect.bottom - rect.top );
	}
	// To Here 2007.09.09 Moca

	// キャレットを隠す
	bool bCaretShowFlag_Old = GetCaret().GetCaretShowFlag();	// 2008.06.09 ryoji
	GetCaret().HideCaret_( this->GetHwnd() ); // 2002/07/22 novice

	RECT			rc;
	int				nLineHeight = GetTextMetrics().GetHankakuDy();
	int				nCharDx = GetTextMetrics().GetCharPxWidth();

	//サポート
	CTypeSupport cTextType(this,COLORIDX_TEXT);

//@@@ 2001.11.17 add start MIK
	//変更があればタイプ設定を行う。
	if( m_pTypeData->m_bUseRegexKeyword || m_cRegexKeyword->m_bUseRegexKeyword ) //OFFなのに前回のデータが残ってる
	{
		//タイプ別設定をする。設定済みかどうかは呼び先でチェックする。
		m_cRegexKeyword->RegexKeySetTypes(m_pTypeData);
	}
//@@@ 2001.11.17 add end MIK

	bool bTransText = IsBkBitmap();
	// メモリＤＣを利用した再描画の場合は描画先のＤＣを切り替える
	HDC hdcOld = nullptr;
	// 2007.09.09 Moca bUseMemoryDCを有効化。
	// bUseMemoryDC = FALSE;
	BOOL bUseMemoryDC = (m_hdcCompatDC != nullptr);
	assert_warning(gr != m_hdcCompatDC);
	bool bClipping = false;
	if( bUseMemoryDC ){
		hdcOld = gr;
		gr = m_hdcCompatDC;
	}else{
		if( bTransText || pPs->rcPaint.bottom - pPs->rcPaint.top <= 2 || pPs->rcPaint.right - pPs->rcPaint.left <= 2 ){
			// 透過処理の場合フォントの輪郭が重ね塗りになるため自分でクリッピング領域を設定
			// 2以下はたぶんアンダーライン・カーソル行縦線の作画
			// MemoryDCの場合は転送が矩形クリッピングの代わりになっている
			gr.SetClipping(pPs->rcPaint);
			bClipping = true;
		}
	}

	/* 03/02/18 対括弧の強調表示(消去) ai */
	if( !bUseMemoryDC ){
		// MemoryDCだとスクロール時に先に括弧だけ表示されて不自然なので後でやる。
		DrawBracketPair( false );
	}

	CEditView& cActiveView = GetEditWnd().GetActiveView();
	m_nPageViewTop = cActiveView.GetTextArea().GetViewTopLine();
	m_nPageViewBottom = cActiveView.GetTextArea().GetBottomLine();

	// 背景の表示
	if( bTransText ){
		HDC hdcBgImg = GetBackImageDC(gr);
		if( hdcBgImg != nullptr ){
			HBITMAP hOldBmp = (HBITMAP)::SelectObject(hdcBgImg, m_pcEditDoc->m_hBackImg);
			DrawBackImage(gr, pPs->rcPaint, hdcBgImg);
			SelectObject(hdcBgImg, hOldBmp);
		}
	}

	/* ルーラーとテキストの間の余白 */
	//@@@ 2002.01.03 YAZAKI 余白が0のときは無駄でした。
	if ( GetTextArea().GetTopYohaku() ){
		if( !bTransText ){
			rc.left   = 0;
			rc.top    = GetTextArea().GetRulerHeight();
			rc.right  = GetTextArea().GetAreaRight();
			rc.bottom = GetTextArea().GetAreaTop();
			cTextType.FillBack(gr,rc);
		}
	}

	/* 行番号の表示 */
	//	From Here Sep. 7, 2001 genta
	//	Sep. 23, 2002 genta 行番号非表示でも行番号色の帯があるので隙間を埋める
	if( GetTextArea().GetTopYohaku() ){
		if( bTransText && m_pTypeData->m_ColorInfoArr[COLORIDX_GYOU].m_sColorAttr.m_cBACK == cTextType.GetBackColor() ){
		}else{
			rc.left   = 0;
			rc.top    = GetTextArea().GetRulerHeight();
			rc.right  = GetTextArea().GetLineNumberWidth(); //	Sep. 23 ,2002 genta 余白はテキスト色のまま残す
			rc.bottom = GetTextArea().GetAreaTop();
			gr.SetTextBackColor(m_pTypeData->m_ColorInfoArr[COLORIDX_GYOU].m_sColorAttr.m_cBACK);
			gr.FillMyRectTextBackColor(rc);
		}
	}
	//	To Here Sep. 7, 2001 genta

	::SetBkMode( gr, TRANSPARENT );

	cTextType.SetGraphicsState_WhileThisObj(gr);

	int nTop = pPs->rcPaint.top;

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//           描画開始レイアウト絶対行 -> nLayoutLine             //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	CLayoutInt nLayoutLine;
	if( 0 > nTop - GetTextArea().GetAreaTop() ){
		nLayoutLine = GetTextArea().GetViewTopLine(); //ビュー上部から描画
	}else{
		nLayoutLine = GetTextArea().GetViewTopLine() + CLayoutInt( ( nTop - GetTextArea().GetAreaTop() ) / nLineHeight ); //ビュー途中から描画
	}

	// ※ ここにあった描画範囲の 260 文字ロールバック処理は GetColorIndex() に吸収	// 2009.02.11 ryoji

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//          描画終了レイアウト絶対行 -> nLayoutLineTo            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	CLayoutInt nLayoutLineTo = GetTextArea().GetViewTopLine()
		+ CLayoutInt( ( pPs->rcPaint.bottom - GetTextArea().GetAreaTop() + (nLineHeight - 1) ) / nLineHeight ) - 1;	// 2007.02.17 ryoji 計算を精密化

	// A paint turn owns an explicit, document-size-independent quantum.  The
	// cursor is keyed to the complete visible viewport rather than the current
	// invalidation rectangle: the next asynchronous paint may be narrower while
	// it still belongs to the same retained surface.
	std::optional<editor::rendering::EditViewPaintCursor> paintCursor;
	if( m_pRenderState ){
		const auto renderSnapshot = m_pRenderState->Snapshot();
		const editor::rendering::EditViewPaintViewport viewport{
			.contentGeneration = renderSnapshot.surface.contentGeneration,
			.layoutEpoch = renderSnapshot.surface.layoutEpoch,
			.layoutTop = static_cast<std::int64_t>(GetTextArea().GetViewTopLine()),
			.layoutBottom = static_cast<std::int64_t>(GetTextArea().GetBottomLine()),
			.viewLeftColumn = static_cast<std::int64_t>(GetTextArea().GetViewLeftCol()),
			.viewRightColumn = static_cast<std::int64_t>(GetTextArea().GetRightCol()),
		};
		(void)m_pRenderState->BeginPaintQuantum(viewport);
		paintCursor = m_pRenderState->PaintCursor();
		if( paintCursor ){
			nLayoutLine = CLayoutInt(paintCursor->layoutLine);
		}
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         描画座標                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	DispPos sPos(nCharDx, GetTextMetrics().GetHankakuDy());
	sPos.InitDrawPos(CMyPoint(
		GetTextArea().GetAreaLeft() - (Int)GetTextArea().GetViewLeftCol() * nCharDx,
		GetTextArea().GetAreaTop() + (Int)( nLayoutLine - GetTextArea().GetViewTopLine() ) * nLineHeight
	));
	if( paintCursor && paintCursor->layoutLine == static_cast<std::int64_t>(nLayoutLine) ){
		// Width is not reconstructed from source text.  This is essential for
		// tabs, surrogate pairs, and custom figures whose advance is contextual.
		sPos.ForwardDrawCol(CLayoutInt(paintCursor->drawColumn));
	}
	sPos.SetLayoutLineRef(nLayoutLine);

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                      全部の行を描画                         //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

	/* アクティブペインは、アンダーライン描画 */
	const bool bDrawUnderLine = GetEditWnd().GetActivePane() == m_nMyIndex;
	// カーソル行アンダーライン描画を行描画ループ内で行うかどうか
	const bool bDrawUnderLineWithoutDelay =
		bDrawUnderLine
		&& !bUseMemoryDC  // メモリDCを利用しない場合はアンダーライン描画を行描画の直後に行う事でちらつきを抑える
		&& !m_pTypeData->m_ColorInfoArr[COLORIDX_CURSORVLINE].m_bDisp  // カーソル行より下にあるカーソル位置縦線が消えてしまうので、非表示設定でのみ行う
		&& m_pTypeData->m_nLineSpace > 0  // 行間を0以下にすると、カーソル行アンダーラインの位置が一つ下の行に含まれるようになるため、レンダリングの対象行が変わるので、行間が0より大きい場合のみ行う
		;

	//必要な行を描画する	// 2009.03.26 ryoji 行番号のみ描画を通常の行描画と分離（効率化）
	if (traceFirstContentWork) {
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintPrepareEnd);
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintLinesBegin);
		CStartupTrace::BeginFirstContentPaintMetrics();
	}
	if(pPs->rcPaint.right <= GetTextArea().GetAreaLeft()){
		while(sPos.GetLayoutLineRef() <= nLayoutLineTo)
		{
			if(!sPos.GetLayoutRef())
				break;

			//1行描画（行番号のみ）
			GetTextDrawer().DispLineNumber(
				gr,
				sPos.GetLayoutLineRef(),
				sPos.GetDrawPos().y
			);
			//行を進める
			sPos.ForwardDrawLine(1);		//描画Y座標＋＋
			sPos.ForwardLayoutLineRef(1);	//レイアウト行＋＋
		}
	}else{
		auto caretY = GetCaret().GetCaretLayoutPos().GetY2();
		SColorStrategyInfo sInfo(gr);
		sInfo.m_pDispPos = &sPos;
		sInfo.m_pcView = this;
		sInfo.m_collectStartupPaintMetrics = traceFirstContentWork;
		while(sPos.GetLayoutLineRef() <= nLayoutLineTo)
		{
			if( m_pRenderState && m_pRenderState->HasPaintContinuation() ){
				// A previous turn has already painted the prefix of this viewport;
				// do not redraw it and spend the new quantum on the saved cursor.
				if( !paintCursor || static_cast<std::int64_t>(sPos.GetLayoutLineRef())
					!= paintCursor->layoutLine ){
					break;
				}
			}
			// Keep the saved draw column when resuming a partial logical line.
			// Every new layout line starts at column zero as before.
			const bool resumePaintLine = m_pRenderState
				&& m_pRenderState->IsPaintCursorFor(
					static_cast<std::int64_t>(sPos.GetLayoutLineRef()));
			if( !resumePaintLine ){
				sPos.ResetDrawCol();
			}

			//DrawLogicLineを呼ぶと値が変わるので呼ぶ前に取得
			auto nCurrLine = sPos.GetLayoutLineRef();
			
			//1行描画
			bool bDispResult = DrawLogicLine(
				&sInfo,
				nLayoutLineTo
			);

			if(bDispResult){
				// EOF再描画対応
				nLayoutLineTo++;
				int nBackImageTop = pPs->rcPaint.bottom;
				pPs->rcPaint.bottom += nLineHeight;
				if(bClipping){
					gr.SetClipping(pPs->rcPaint);
				}
				if(bTransText){
					HDC hdcBgImg = GetBackImageDC(gr);
					if( hdcBgImg != nullptr ){
						HBITMAP hOldBmp = (HBITMAP)::SelectObject(hdcBgImg, m_pcEditDoc->m_hBackImg);
						RECT rc2 = pPs->rcPaint;
						rc2.top = nBackImageTop;
						DrawBackImage(gr, rc2, hdcBgImg);
						SelectObject(hdcBgImg, hOldBmp);
					}
				}
			}
			if( m_pRenderState && m_pRenderState->HasPaintContinuation() ){
				break;
			}
			if (bDrawUnderLineWithoutDelay && nCurrLine == caretY) {
				GetCaret().m_cUnderLine.CaretUnderLineON(true, false);
			}
		}
	}
	if (traceFirstContentWork) {
		CStartupTrace::EndFirstContentPaintMetrics();
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintLinesEnd);
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintFinishBegin);
	}

	cTextType.RewindGraphicsState(gr);

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                       ルーラー描画                          //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	if ( pPs->rcPaint.top < GetTextArea().GetRulerHeight() ) { // ルーラーが再描画範囲にあるときのみ再描画する 2002.02.25 Add By KK
		GetRuler().SetRedrawFlag(); //2002.02.25 Add By KK ルーラー全体を描画。
		GetRuler().DispRuler( gr );
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                     その他後始末など                        //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	/* メモリＤＣを利用した再描画の場合はメモリＤＣに描画した内容を画面へコピーする */
	if( bUseMemoryDC ){
		// 2010.10.11 先に描くと背景固定のスクロールなどでの表示が不自然になる
		DrawBracketPair( false );

		::BitBlt(
			hdcOld,
			pPs->rcPaint.left,
			pPs->rcPaint.top,
			pPs->rcPaint.right - pPs->rcPaint.left,
			pPs->rcPaint.bottom - pPs->rcPaint.top,
			gr,
			pPs->rcPaint.left,
			pPs->rcPaint.top,
			SRCCOPY
		);
	}

	// From Here 2007.09.09 Moca 互換BMPによる画面バッファ
	//     アンダーライン描画をメモリDCからのコピー前処理から後に移動
	if ( bDrawUnderLine && !bDrawUnderLineWithoutDelay ){
		/* アクティブペインは、アンダーライン描画 */
		GetCaret().m_cUnderLine.CaretUnderLineON( true, false );
	}
	// To Here 2007.09.09 Moca

	/* 03/02/18 対括弧の強調表示(描画) ai */
	DrawBracketPair( true );

	/* キャレットを現在位置に表示します */
	if( bCaretShowFlag_Old )	// 2008.06.09 ryoji
		GetCaret().ShowCaret_( this->GetHwnd() ); // 2002/07/22 novice
	if (traceFirstContentWork) {
		CStartupTrace::Mark(CStartupTrace::Event::FirstContentPaintFinishEnd);
	}
	if( m_pRenderState && m_pRenderState->HasPaintContinuation() ){
		// Continue through the ordinary asynchronous paint queue.  There is no
		// timer, wait, or nested UpdateWindow call: user input remains responsive
		// while the saved cursor consumes one explicit quantum per paint.
		const auto continuation = m_pRenderState->PaintCursor();
		if( continuation ){
			const int y = GetTextArea().GenerateYPx(
				CLayoutInt(continuation->layoutLine));
			RECT continuationRect{
				GetTextArea().GetAreaLeft(),
				y,
				GetTextArea().GetAreaRight(),
				y + nLineHeight,
			};
			::InvalidateRect(GetHwnd(), &continuationRect, FALSE);
		}
	}
	return true;
}

/*!
	行のテキスト／選択状態の描画
	1回で1ロジック行分を作画する。

	@return EOFを作画したらtrue

	@date 2001.02.17 MIK
	@date 2001.12.21 YAZAKI 改行記号の描きかたを変更
	@date 2007.08.31 kobake 引数 bDispBkBitmap を削除
*/
bool CEditView::DrawLogicLine(
	SColorStrategyInfo* pInfo,		//!< [in,out] 作画情報
	CLayoutInt		nLineTo			//!< [in]     作画終了するレイアウト行番号
)
{
//	MY_RUNNINGTIMER( cRunningTimer, "CEditView::DrawLogicLine" );
	bool bDispEOF = false;

	//CColorStrategyPool初期化
	CColorStrategyPool* pool = CColorStrategyPool::getInstance();
	pool->SetCurrentView(this);
	pool->NotifyOnStartScanLogic();
	bool bSkipBeforeLayout = pool->IsSkipBeforeLayout();

	//DispPosを保存しておく
	pInfo->m_sDispPosBegin = *pInfo->m_pDispPos;
	const auto paintCursor = m_pRenderState
		? m_pRenderState->PaintCursor()
		: std::optional<editor::rendering::EditViewPaintCursor>{};
	const auto currentLayoutLine = static_cast<std::int64_t>(
		pInfo->m_pDispPos->GetLayoutLineRef());
	const bool resumePaintLine = paintCursor
		&& paintCursor->layoutLine == currentLayoutLine;
	if( m_pRenderState && m_pRenderState->PaintQuantumRemaining() == 0 ){
		const CLayout* pcLayout = pInfo->m_pDispPos->GetLayoutRef();
		m_pRenderState->SavePaintCursor(
			currentLayoutLine,
			pcLayout ? static_cast<std::int64_t>(pcLayout->GetLogicOffset()) : 0,
			0);
		return false;
	}

	//処理する文字位置
	pInfo->m_nPosInLogic = CLogicInt(0); //☆開始

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//          論理行データの取得 -> pLine, pLineLen              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	// 前行の最終設定色
	{
		const CLayout* pcLayout = pInfo->m_pDispPos->GetLayoutRef();
		if( bSkipBeforeLayout ){
			EColorIndexType eRet = COLORIDX_TEXT;
			const CLayoutColorInfo* colorInfo = nullptr;
			if( pcLayout ){
				eRet = pcLayout->GetColorTypePrev(); // COLORIDX_TEXTのはず
				colorInfo = pcLayout->GetColorInfo();
			}
			pInfo->m_pStrategy = pool->GetStrategyByColor(eRet);
			if( pInfo->m_pStrategy ){
				pInfo->m_pStrategy->InitStrategyStatus();
				pInfo->m_pStrategy->SetStrategyColorInfo(colorInfo);
			}
		}else{
		const int resumeOffset = resumePaintLine && paintCursor && pcLayout
			? static_cast<int>(paintCursor->logicOffset
				- static_cast<std::int64_t>(pcLayout->GetLogicOffset()))
			: 0;
		CColor3Setting cColor = GetColorIndex(
			pcLayout, pInfo->m_pDispPos->GetLayoutLineRef(), resumeOffset, pInfo, true);
		if( resumePaintLine ){
			// GetColorIndex reconstructs syntax state from the logical-line start;
			// restore the exact UTF-16 cursor after that scan.
			pInfo->m_nPosInLogic = CLogicInt(paintCursor->logicOffset);
		}
		SetCurrentColor(pInfo->m_gr, cColor.eColorIndex, cColor.eColorIndex2, cColor.eColorIndexBg);
		}
	}

	//開始ロジック位置を算出
	{
		const CLayout* pcLayout = pInfo->m_pDispPos->GetLayoutRef();
		// GetColorIndex scans from the logical-line start to reconstruct syntax
		// state.  A resumed paint must retain the exact UTF-16 position saved by
		// the previous quantum instead of restarting at this layout row.
		if( resumePaintLine && paintCursor ){
			pInfo->m_nPosInLogic = CLogicInt(paintCursor->logicOffset);
		}else{
			pInfo->m_nPosInLogic = pcLayout?pcLayout->GetLogicOffset():CLogicInt(0);
		}
	}

	for (;;) {
		//対象行が描画範囲外だったら終了
		if( GetTextArea().GetBottomLine() < pInfo->m_pDispPos->GetLayoutLineRef() ){
			pInfo->m_pDispPos->SetLayoutLineRef(nLineTo + CLayoutInt(1));
			break;
		}
		if( nLineTo < pInfo->m_pDispPos->GetLayoutLineRef() ){
			break;
		}

		//レイアウト行を1行描画
		bDispEOF = DrawLayoutLine(pInfo);
		if( m_pRenderState && m_pRenderState->HasPaintContinuation() ){
			// DrawLayoutLine yielded at the explicit quantum.  Its cursor owns
			// the current layout line, so advancing here would skip its remainder.
			break;
		}

		//行を進める
		CLogicInt nOldLogicLineNo = CLayout::GetLogicLineNo_Safe(pInfo->m_pDispPos->GetLayoutRef());
		pInfo->m_pDispPos->ForwardDrawLine(1);		//描画Y座標＋＋
		pInfo->m_pDispPos->ForwardLayoutLineRef(1);	//レイアウト行＋＋

		// ロジック行を描画し終わったら抜ける
		if(CLayout::GetLogicLineNo_Safe(pInfo->m_pDispPos->GetLayoutRef()) != nOldLogicLineNo){
			break;
		}

		// nLineToを超えたら抜ける
		if(pInfo->m_pDispPos->GetLayoutLineRef() >= nLineTo + CLayoutInt(1)){
			break;
		}
	}

	return bDispEOF;
}

/*!
	レイアウト行を1行描画
*/
//改行記号を描画した場合はtrueを返す？
bool CEditView::DrawLayoutLine(SColorStrategyInfo* pInfo)
{
	bool bDispEOF = false;
	CTypeSupport cTextType(this,COLORIDX_TEXT);

	const CLayout* pcLayout = pInfo->m_pDispPos->GetLayoutRef(); //m_pcEditDoc->m_cLayoutMgr.SearchLineByLayoutY( pInfo->pDispPos->GetLayoutLineRef() );
	const auto paintCursor = m_pRenderState
		? m_pRenderState->PaintCursor()
		: std::optional<editor::rendering::EditViewPaintCursor>{};
	const auto currentLayoutLine = static_cast<std::int64_t>(
		pInfo->m_pDispPos->GetLayoutLineRef());
	const bool resumePaintLine = paintCursor
		&& paintCursor->layoutLine == currentLayoutLine;

	// レイアウト情報
	if( pcLayout ){
		pInfo->m_pLineOfLogic = pcLayout->GetDocLineRef()->GetPtr();
	}
	else{
		pInfo->m_pLineOfLogic = nullptr;
	}

	//文字列参照
	const CDocLine* pcDocLine = pInfo->GetDocLine();
	CStringRef cLineStr = CDocLine::GetStringRefWithEOL_Safe(pcDocLine);

	// 描画範囲外の場合は色切替だけで抜ける
	if(pInfo->m_pDispPos->GetDrawPos().y < GetTextArea().GetAreaTop()){
		if(pcLayout){
			bool bChange = false;
			int nPosTo = pcLayout->GetLogicOffset() + pcLayout->GetLengthWithEOL();
			CColor3Setting cColor;
			while(pInfo->m_nPosInLogic < nPosTo){
				//色切替
				bChange |= pInfo->CheckChangeColor(cLineStr);

				//1文字進む
				pInfo->m_nPosInLogic += CNativeW::GetSizeOfChar(
											cLineStr.GetPtr(),
											cLineStr.GetLength(),
											pInfo->m_nPosInLogic
										);
			}
			if( bChange ){
				pInfo->DoChangeColor(&cColor);
				SetCurrentColor(pInfo->m_gr, cColor.eColorIndex, cColor.eColorIndex2, cColor.eColorIndexBg);
			}
		}
		return false;
	}

	// コンフィグ
	int nLineHeight = GetTextMetrics().GetHankakuDy();  //行の縦幅？
	CTypeSupport	cCaretLineBg(this, COLORIDX_CARETLINEBG);
	CTypeSupport	cEvenLineBg(this, COLORIDX_EVENLINEBG);
	CTypeSupport&	cBackType = (cCaretLineBg.IsDisp() &&
		GetCaret().GetCaretLayoutPos().GetY() == pInfo->m_pDispPos->GetLayoutLineRef() && !m_bMiniMap
			? cCaretLineBg
			: cEvenLineBg.IsDisp() && pInfo->m_pDispPos->GetLayoutLineRef() % 2 == 1 && !m_bMiniMap
				? cEvenLineBg
				: cTextType);
	bool bTransText = IsBkBitmap();
	if( bTransText ){
		bTransText = cBackType.GetBackColor() == cTextType.GetBackColor();
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                        行番号描画                           //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	GetTextDrawer().DispLineNumber(
		pInfo->m_gr,
		pInfo->m_pDispPos->GetLayoutLineRef(),
		pInfo->m_pDispPos->GetDrawPos().y
	);

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                       本文描画開始                          //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	if( !resumePaintLine ){
		pInfo->m_pDispPos->ResetDrawCol();
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                 行頭(インデント)背景描画                    //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	if( pcLayout && pcLayout->GetIndent()!=0 && !resumePaintLine )
	{
		RECT rcClip;
		if(!bTransText && GetTextArea().GenerateClipRect(&rcClip, *pInfo->m_pDispPos, pcLayout->GetIndent())){
			cBackType.FillBack(pInfo->m_gr,rcClip);
		}
		//描画位置進める
		pInfo->m_pDispPos->ForwardDrawCol(pcLayout->GetIndent());
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         本文描画                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	bool bSkipRight = false; // 続きを描画しなくていい場合はスキップする
	if(pcLayout){
		const CLayout* pcLayoutNext = pcLayout->GetNextLayout();
		if( nullptr == pcLayoutNext ){
			bSkipRight = true;
		}else if( pcLayoutNext->GetLogicOffset() == 0 ){
			bSkipRight = true; // 次の行は別のロジック行なのでスキップ可能
		}
		if( !bSkipRight ){
			bSkipRight = CColorStrategyPool::getInstance()->IsSkipBeforeLayout();
		}
	}
	//行終端または折り返しに達するまでループ
	if(pcLayout){
		const auto* textMateTokens = GetTextMateTokens(
			CLayout::GetLogicLineNo_Safe(pcLayout));
		std::size_t textMateIndex = 0;
		const auto tokenAt = [&](int offset) -> const textmate::TextMateToken* {
			if( textMateTokens == nullptr || offset < 0 ) return nullptr;
			const auto position = static_cast<std::size_t>(offset);
			while( textMateIndex < textMateTokens->size()
				&& (*textMateTokens)[textMateIndex].utf16End <= position ) {
				++textMateIndex;
			}
			if( textMateIndex >= textMateTokens->size() ) return nullptr;
			const auto& token = (*textMateTokens)[textMateIndex];
			return token.utf16Start <= position && position < token.utf16End
				? &token : nullptr;
		};
		int nPosBgn = pInfo->m_nPosInLogic; // Logic
		int nPosLength = 0;
		CLayoutInt nDrawX = pInfo->m_pDispPos->GetDrawCol(); // Layout
		const int nDrawBlockLen = 1000; // ExtTextOutの長さ制限にかからない適当な値
		int nPosTo = pcLayout->GetLogicOffset() + pcLayout->GetLengthWithEOL();
		CFigureManager* pcFigureManager = CFigureManager::getInstance();
		FigureRenderType prevRenderType = CFigure_Text::RenderType_None;
		const textmate::TextMateToken* activeTextMateToken = tokenAt(nPosBgn);
		ApplyTextMateTokenStyle(*pInfo, activeTextMateToken);
		bool paintYielded = false;
		while(pInfo->m_nPosInLogic < nPosTo){
			if( m_pRenderState && !m_pRenderState->ConsumePaintWork() ){
				paintYielded = true;
				break;
			}
			int nPosInLogic = pInfo->GetPosInLogic(); // FowardChars/DrawImpで更新される
			nPosLength = nPosInLogic - nPosBgn;
			//1文字情報取得
			CFigure& cFigure = pcFigureManager->GetFigure(&cLineStr.GetPtr()[nPosInLogic],
				cLineStr.GetLength() - nPosInLogic);
			FigureRenderType nextRenderType = CFigure_Text::RenderType_None;
			bool is_text = (typeid(cFigure) == typeid(CFigure_Text));
			if (is_text) {
				nextRenderType = CFigure_Text::GetRenderType(pInfo);
			}
			const textmate::TextMateToken* nextTextMateToken = tokenAt(nPosInLogic);
			const bool textMateBoundary = nextTextMateToken != activeTextMateToken;
			const bool renderTypeBoundary = prevRenderType != nextRenderType;
			const bool lengthBoundary = nDrawBlockLen < nPosLength;
			if (CFigure_Text::IsRenderType_Block(prevRenderType) &&
				(renderTypeBoundary || lengthBoundary || textMateBoundary)) {
				if (0 < nPosLength) {
					if (pInfo->m_collectStartupPaintMetrics) {
						CStartupTrace::AccumulateFirstContentTextBoundary(
							renderTypeBoundary, lengthBoundary, false, false);
					}
					CFigure_Text::DrawImpBlock(pInfo, nPosBgn, nPosLength);
					nPosBgn = nPosInLogic;
					nPosLength = 0;
				}
			}
			if( textMateBoundary ) {
				SetCurrentColor(pInfo->m_gr, pInfo->GetCurrentColor(),
					pInfo->GetCurrentColor2(), pInfo->GetCurrentColorBg());
				activeTextMateToken = nextTextMateToken;
				ApplyTextMateTokenStyle(*pInfo, activeTextMateToken);
			}
			prevRenderType = nextRenderType;

			//色切替
			const bool colorChanged = pInfo->CheckChangeColor(cLineStr);
			if (pInfo->m_collectStartupPaintMetrics) {
				CStartupTrace::AccumulateFirstContentTextScan(is_text, colorChanged);
			}
			if( colorChanged ){
				if (0 < nPosLength) {
					if (pInfo->m_collectStartupPaintMetrics) {
						CStartupTrace::AccumulateFirstContentTextBoundary(false, false, true, false);
					}
					CFigure_Text::DrawImpBlock(pInfo, nPosBgn, nPosLength);
					nPosBgn = nPosInLogic;
					nPosLength = 0;
				}
				CColor3Setting cColor;
				pInfo->DoChangeColor(&cColor);
				SetCurrentColor(pInfo->m_gr, cColor.eColorIndex, cColor.eColorIndex2, cColor.eColorIndexBg);
				ApplyTextMateTokenStyle(*pInfo, activeTextMateToken);
			}

			if (is_text && CFigure_Text::IsRenderType_Block(nextRenderType)){
				nDrawX += CFigure_Text::FowardChars(pInfo);
				nPosInLogic = pInfo->GetPosInLogic();
				nPosLength = nPosInLogic - nPosBgn;
			}else{
				//1文字描画
				cFigure.DrawImp(pInfo);
				nPosBgn = nPosInLogic = pInfo->GetPosInLogic();
				nPosLength = 0;
				nDrawX = pInfo->m_pDispPos->GetDrawCol();
			}
			if( bSkipRight && GetTextArea().GetRightCol() < nDrawX ){
				if (0 < nPosLength) {
					if (pInfo->m_collectStartupPaintMetrics) {
						CStartupTrace::AccumulateFirstContentTextBoundary(false, false, false, true);
					}
					CFigure_Text::DrawImpBlock(pInfo, nPosBgn, nPosLength);
					nPosBgn = nPosInLogic;
				}
				pInfo->m_nPosInLogic = nPosTo;
				nPosLength = nPosTo - nPosBgn;
				break;
			}
		}
		if (0 < nPosLength) {
			if (pInfo->m_collectStartupPaintMetrics) {
				CStartupTrace::AccumulateFirstContentTextBoundary(false, false, false, true);
			}
			CFigure_Text::DrawImpBlock(pInfo, nPosBgn, nPosLength);
		}
		if( paintYielded ){
			// Flush the current text run before yielding.  The UTF-16 offset and
			// measured draw column are retained, so no source unit disappears and
			// the next paint does not repeat a figure with side effects.
			if( m_pRenderState ){
				m_pRenderState->SavePaintCursor(
					currentLayoutLine,
					static_cast<std::int64_t>(pInfo->m_nPosInLogic),
					static_cast<std::int64_t>(pInfo->m_pDispPos->GetDrawCol()));
			}
			return false;
		}
	}

	// SENP editor decorations are cache-only on the paint thread. A missing
	// result schedules bounded work in the isolated host and returns immediately.
	if( pcLayout && pcLayout->GetLogicOffset() == 0 && !m_bMiniMap
		&& !theme::CThemeService::IsHighContrastActive() ){
		if( auto* runtime = GetEditWnd().GetSenpRuntime() ){
			std::size_t lineLength = static_cast<std::size_t>(cLineStr.GetLength());
			while( lineLength > 0 && (cLineStr.GetPtr()[lineLength - 1] == L'\r'
				|| cLineStr.GetPtr()[lineLength - 1] == L'\n') ) --lineLength;
			const auto decorations = runtime->RequestIndentDecorations(
				std::wstring_view(cLineStr.GetPtr(), lineLength),
				static_cast<std::uint32_t>(m_pcEditDoc->m_cLayoutMgr.GetTabSpaceKetas()),
				reinterpret_cast<std::uintptr_t>(GetHwnd()));
			if( decorations ){
				const CLayoutInt viewLeft = GetTextArea().GetViewLeftCol();
				const int layoutUnitsPerColumn = static_cast<int>(
					GetTextMetrics().GetLayoutXDefault());
				RECT body{ GetTextArea().GetAreaLeft(), pInfo->m_pDispPos->GetDrawPos().y,
					GetTextArea().GetAreaRight(), pInfo->m_pDispPos->GetDrawPos().y + nLineHeight };
				for( const auto& decoration : *decorations ){
					const auto layoutRange = view::indent_decoration::ProjectVisualColumns(
						decoration.visualStart, decoration.visualLength, layoutUnitsPerColumn);
					if( !layoutRange ) continue;
					if( CLayoutInt(layoutRange->end) <= viewLeft ) continue;
					const CLayoutInt relativeStart = CLayoutInt(layoutRange->start)
						- viewLeft;
					const CLayoutInt relativeEnd = CLayoutInt(layoutRange->end)
						- viewLeft;
					const int left = GetTextArea().GetAreaLeft()
						+ static_cast<int>(GetTextMetrics().GetCharPxWidth(relativeStart));
					if( left >= body.right ) break;
					const int right = GetTextArea().GetAreaLeft()
						+ static_cast<int>(GetTextMetrics().GetCharPxWidth(relativeEnd));
					RECT band{ left, pInfo->m_pDispPos->GetDrawPos().y,
						right, pInfo->m_pDispPos->GetDrawPos().y + nLineHeight };
					RECT clipped{};
					if( ::IntersectRect(&clipped, &band, &body) ){
						FillAlphaOverlay(pInfo->m_gr, clipped,
							IndentDecorationColor(decoration.depth), kIndentDecorationAlpha);
					}
				}
			}
		}
	}

	// VS Code owns indentation guides in the Editor. Keep them independent of
	// SENP decorations so disabling or uninstalling indent-rainbow leaves the
	// built-in guide surface intact.
	if( pcLayout && pcLayout->GetLogicOffset() == 0 && !m_bMiniMap
		&& m_indentGuidesEnabled ){
		std::size_t lineLength = static_cast<std::size_t>(cLineStr.GetLength());
		while( lineLength > 0 && (cLineStr.GetPtr()[lineLength - 1] == L'\r'
			|| cLineStr.GetPtr()[lineLength - 1] == L'\n') ) --lineLength;
		const auto tabSize = static_cast<std::uint32_t>((std::max)(1,
			static_cast<int>(m_pcEditDoc->m_cLayoutMgr.GetTabSpaceKetas())));
		const auto indentColumns = view::indent_guide::LeadingVisualColumns(
			std::wstring_view(cLineStr.GetPtr(), lineLength), tabSize);
		if( indentColumns > 0 ){
			const int layoutUnitsPerColumn = static_cast<int>(
				GetTextMetrics().GetLayoutXDefault());
			const CLayoutInt viewLeft = GetTextArea().GetViewLeftCol();
			const int lineWidth = static_cast<int>((std::max)(1L, DpiScaleX(1)));
			const int top = pInfo->m_pDispPos->GetDrawPos().y;
			RECT body{ GetTextArea().GetAreaLeft(), top,
				GetTextArea().GetAreaRight(), top + nLineHeight };
			theme::ThemeColor guideColor = theme::CThemeService::PaletteFor(
				GetDllShareData().m_Common.m_sWindow.m_bDarkMode
					? theme::ThemeMode::Dark : theme::ThemeMode::Light)
				.editorIndentGuideBackground;
			if( theme::CThemeService::IsHighContrastActive() ){
				guideColor = theme::CThemeService::HighContrastPalette()
					.editorIndentGuideBackground;
			}else if( const auto* activePalette =
				theme::CThemeService::ActiveColorThemePalette() ){
				guideColor = activePalette->editorIndentGuideBackground;
			}
			for( std::uint32_t column = 0; column < indentColumns; ){
				const auto projected = view::indent_decoration::ProjectVisualColumns(
					column, 1, layoutUnitsPerColumn);
				if( !projected ) break;
				if( CLayoutInt(projected->start) >= viewLeft ){
					const CLayoutInt relative = CLayoutInt(projected->start) - viewLeft;
					const int left = GetTextArea().GetAreaLeft()
						+ static_cast<int>(GetTextMetrics().GetCharPxWidth(relative));
					if( left >= body.right ) break;
					RECT guide{ left, top, left + lineWidth, top + nLineHeight };
					RECT clipped{};
					if( ::IntersectRect(&clipped, &guide, &body) ){
						FillAlphaOverlay(pInfo->m_gr, clipped,
							guideColor.ToColorRef(), guideColor.alpha);
					}
				}
				if( column > std::numeric_limits<std::uint32_t>::max() - tabSize ) break;
				column += tabSize;
			}
		}
	}

	// 必要ならEOF描画
	void _DispEOF( CGraphics& gr, DispPos* pDispPos, const CEditView* pcView);
	if(pcLayout && pcLayout->GetNextLayout()==nullptr && pcLayout->GetLayoutEol().GetLen()==0){
		// 有文字行のEOF
		_DispEOF(pInfo->m_gr,pInfo->m_pDispPos,this);
		bDispEOF = true;
	}
	else if(!pcLayout && pInfo->m_pDispPos->GetLayoutLineRef()==m_pcEditDoc->m_cLayoutMgr.GetLineCount()){
		// 空行のEOF
		const CLayout* pBottom = m_pcEditDoc->m_cLayoutMgr.GetBottomLayout();
		if(pBottom==nullptr || (pBottom && pBottom->GetLayoutEol().GetLen())){
			_DispEOF(pInfo->m_gr,pInfo->m_pDispPos,this);
			bDispEOF = true;
		}
	}

	// 必要なら折り返し記号描画
	if(pcLayout && pcLayout->GetLayoutEol().GetLen()==0 && pcLayout->GetNextLayout()!=nullptr){
		_DispWrap(pInfo->m_gr,pInfo->m_pDispPos,this,pInfo->m_pDispPos->GetLayoutLineRef());
	}

	// 行末背景描画
	RECT rcClip;
	bool rcClipRet = GetTextArea().GenerateClipRectRight(&rcClip,*pInfo->m_pDispPos);
	if(rcClipRet){
		if( !bTransText ){
			cBackType.FillBack(pInfo->m_gr,rcClip);
		}
		CTypeSupport cSelectType(this, COLORIDX_SELECT);
		if( GetSelectionInfo().IsTextSelected() && cSelectType.IsDisp() ){
			// 選択範囲の指定色：必要ならテキストのない部分の矩形選択を作画
			CLayoutRange selectArea = GetSelectionInfo().GetSelectAreaLine(pInfo->m_pDispPos->GetLayoutLineRef(), pcLayout);
			// 2010.10.04 スクロール分の足し忘れ
			CPixelXInt nSelectFromPx =  GetTextMetrics().GetCharPxWidth(selectArea.GetFrom().x - GetTextArea().GetViewLeftCol());
			CPixelXInt nSelectToPx   = GetTextMetrics().GetCharPxWidth(selectArea.GetTo().x - GetTextArea().GetViewLeftCol());
			if( nSelectFromPx < nSelectToPx && selectArea.GetTo().x != INT_MAX ){
				RECT rcSelect; // Pixel
				rcSelect.top    = pInfo->m_pDispPos->GetDrawPos().y;
				rcSelect.bottom = pInfo->m_pDispPos->GetDrawPos().y + GetTextMetrics().GetHankakuDy();
				rcSelect.left   = GetTextArea().GetAreaLeft() + nSelectFromPx;
				rcSelect.right  = GetTextArea().GetAreaLeft() + nSelectToPx;
				RECT rcDraw;
				if( ::IntersectRect(&rcDraw, &rcClip, &rcSelect) ){
					COLORREF color = GetBackColorByColorInfo2(cSelectType.GetColorInfo(), cBackType.GetColorInfo());
					if( color != cBackType.GetBackColor() ){
						pInfo->m_gr.FillSolidMyRect(rcDraw, color);
					}
				}
			}
		}
	}

	// ノート線描画
	if( !m_bMiniMap ){
		GetTextDrawer().DispNoteLine(
			pInfo->m_gr,
			pInfo->m_pDispPos->GetDrawPos().y,
			pInfo->m_pDispPos->GetDrawPos().y + nLineHeight,
			GetTextArea().GetAreaLeft(),
			GetTextArea().GetAreaRight()
		);
	}

	// 指定桁縦線描画
	GetTextDrawer().DispVerticalLines(
		pInfo->m_gr,
		pInfo->m_pDispPos->GetDrawPos().y,
		pInfo->m_pDispPos->GetDrawPos().y + nLineHeight,
		CLayoutInt(0),
		CLayoutInt(-1)
	);

	// 折り返し桁縦線描画
	if( !m_bMiniMap ){
		GetTextDrawer().DispWrapLine(
			pInfo->m_gr,
			pInfo->m_pDispPos->GetDrawPos().y,
			pInfo->m_pDispPos->GetDrawPos().y + nLineHeight
		);
	}

	// 反転描画
	if( pcLayout && GetSelectionInfo().IsTextSelected() ){
		DispTextSelected(
			pInfo->m_gr,
			pInfo->m_pDispPos->GetLayoutLineRef(),
			CMyPoint(pInfo->m_sDispPosBegin.GetDrawPos().x, pInfo->m_pDispPos->GetDrawPos().y),
			pcLayout->CalcLayoutWidth(m_pcEditDoc->m_cLayoutMgr)
				+ CLayoutInt(pcLayout->GetLayoutEol().GetLen()
					? (CTypeSupport(this, COLORIDX_EOL).IsDisp()
						? (GetTextMetrics().GetLayoutXDefault()+CLayoutXInt(4)) // HACK:EOLの描画幅分だけ確保する。4pxはCRLFのはみ出している分
						: CLayoutXInt(2)) // 非表示 = 2px
					: CLayoutInt(0))
		);
	}
	if( resumePaintLine && m_pRenderState ){
		m_pRenderState->CompletePaintCursor();
	}

	return bDispEOF;
}

/* テキスト反転

	@param hdc
	@param nLineNum
	@param x
	@param y
	@param nX

	@note
	CCEditView::DrawLogicLine() での作画(WM_PAINT)時に、1レイアウト行をまとめて反転処理するための関数。
	範囲選択の随時更新は、CEditView::DrawSelectArea() が選択・反転解除を行う。

*/
void CEditView::DispTextSelected(
	HDC				hdc,		//!< 作画対象ビットマップを含むデバイス
	CLayoutInt		nLineNum,	//!< 反転処理対象レイアウト行番号(0開始)
	const CMyPoint&	ptXY,		//!< (相対レイアウト0桁目の左端座標, 対象行の上端座標)
	CLayoutInt		nX_Layout	//!< 対象行の終了桁位置。　[ABC\n]なら改行の後ろで4
)
{
	CLayoutInt	nSelectFrom;
	CLayoutInt	nSelectTo;
	RECT		rcClip;
	int			nLineHeight = GetTextMetrics().GetHankakuDy();
	int			nCharWidth = GetTextMetrics().GetCharPxWidth();
	HRGN		hrgnDraw;
	const CLayout* pcLayout = m_pcEditDoc->m_cLayoutMgr.SearchLineByLayoutY( nLineNum );
	const CLayoutRange& sSelect = GetSelectionInfo().GetSelectionRange();

	/* 選択範囲内の行かな */
//	if( IsTextSelected() ){
		if( nLineNum >= sSelect.GetFrom().y && nLineNum <= sSelect.GetTo().y ){
			CLayoutRange selectArea = GetSelectionInfo().GetSelectAreaLine(nLineNum, pcLayout);
			nSelectFrom = selectArea.GetFrom().x;
			nSelectTo   = selectArea.GetTo().x;
			if( nSelectFrom == INT_MAX ){
				nSelectFrom = nX_Layout;
			}
			if( nSelectTo == INT_MAX ){
				nSelectTo = nX_Layout;
			}

			// 2006.03.28 Moca 表示域外なら何もしない
			if( GetTextArea().GetRightCol() < nSelectFrom ){
				return;
			}
			if( nSelectTo < GetTextArea().GetViewLeftCol() ){	// nSelectTo == GetTextArea().GetViewLeftCol()のケースは後で０文字マッチでないことを確認してから抜ける
				return;
			}

			if( nSelectFrom < GetTextArea().GetViewLeftCol() ){
				nSelectFrom = GetTextArea().GetViewLeftCol();
			}
			rcClip.left   = ptXY.x + (Int)nSelectFrom * nCharWidth;
			rcClip.right  = ptXY.x + (Int)nSelectTo   * nCharWidth;
			rcClip.top    = ptXY.y;
			rcClip.bottom = ptXY.y + nLineHeight;

			bool bOMatch = false;

			// 2005/04/02 かろと ０文字マッチだと反転幅が０となり反転されないので、1/3文字幅だけ反転させる
			// 2005/06/26 zenryaku 選択解除でキャレットの残骸が残る問題を修正
			// 2005/09/29 ryoji スクロール時にキャレットのようなゴミが表示される問題を修正
			if (GetSelectionInfo().IsTextSelected() && rcClip.right == rcClip.left &&
				sSelect.IsLineOne() &&
				sSelect.GetFrom().x >= GetTextArea().GetViewLeftCol())
			{
				HWND hWnd = ::GetForegroundWindow();
				if( hWnd && (hWnd == GetEditWnd().m_cDlgFind.GetHwnd() || hWnd == GetEditWnd().m_cDlgReplace.GetHwnd()) ){
					rcClip.right = rcClip.left + 2;
					bOMatch = true;
				}
			}
			if( rcClip.right == rcClip.left ){
				return;	//０文字マッチによる反転幅拡張なし
			}

			// 2006.03.28 Moca ウィンドウ幅が大きいと正しく反転しない問題を修正
			if( rcClip.right > GetTextArea().GetAreaRight() ){
				rcClip.right = GetTextArea().GetAreaRight();
			}

			// 選択色表示なら反転しない
			if( !bOMatch && CTypeSupport(this, COLORIDX_SELECT).IsDisp() ){
				return;
			}

			int    nROP_Old  = ::SetROP2( hdc, SELECTEDAREA_ROP2 );
			const HBRUSH dcBrush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
			const HBRUSH hBrushOld = static_cast<HBRUSH>(::SelectObject(hdc, dcBrush));
			const COLORREF oldBrushColor = ::SetDCBrushColor(hdc, SELECTEDAREA_RGB);
			hrgnDraw = ::CreateRectRgn( rcClip.left, rcClip.top, rcClip.right, rcClip.bottom );
			::PaintRgn( hdc, hrgnDraw );
			::DeleteObject( hrgnDraw );

			::SetDCBrushColor(hdc, oldBrushColor);
			::SetROP2( hdc, nROP_Old );
			::SelectObject(hdc, hBrushOld);
		}
//	}
	return;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                       画面バッファ                          //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!
	画面の互換ビットマップを作成または更新する。
		必要の無いときは何もしない。

	@param cx ウィンドウの高さ
	@param cy ウィンドウの幅
	@return true: ビットマップを利用可能 / false: ビットマップの作成・更新に失敗

	@date 2007.09.09 Moca CEditView::OnSizeから分離。
		単純に生成するだけだったものを、仕様変更に従い内容コピーを追加。
		サイズが同じときは何もしないように変更

	@par 互換BMPにはキャレット・カーソル位置横縦線・対括弧以外の情報を全て書き込む。
		選択範囲変更時の反転処理は、画面と互換BMPの両方を別々に変更する。
		カーソル位置横縦線変更時には、互換BMPから画面に元の情報を復帰させている。

*/
bool CEditView::CreateOrUpdateCompatibleBitmap( int cx, int cy )
{
	if( nullptr == m_hdcCompatDC ){
		return false;
	}
	// サイズを64の倍数で整列
	int nBmpWidthNew  = ((cx + 63) & (0x7fffffff - 63));
	int nBmpHeightNew = ((cy + 63) & (0x7fffffff - 63));
	// 保持面積が必要面積の2倍を超えたら縮小方向にも作り直して余剰を解放する
	bool bShrink = 0 < nBmpWidthNew && 0 < nBmpHeightNew
		&& static_cast<long long>(m_nCompatBMPWidth) * m_nCompatBMPHeight
			> static_cast<long long>(nBmpWidthNew) * nBmpHeightNew * 2;
	if( nBmpWidthNew > m_nCompatBMPWidth || nBmpHeightNew > m_nCompatBMPHeight || bShrink ){
#if 0
	MYTRACE( L"CEditView::CreateOrUpdateCompatibleBitmap( %d, %d ): resized\n", cx, cy );
#endif
		HDC	hdc = ::GetDC( GetHwnd() );
		HBITMAP hBitmapNew = nullptr;
		if( m_hbmpCompatBMP ){
			// BMPの更新
			HDC hdcTemp = ::CreateCompatibleDC( hdc );
			hBitmapNew = ::CreateCompatibleBitmap( hdc, nBmpWidthNew, nBmpHeightNew );
			if( hBitmapNew ){
				HBITMAP hBitmapOld = (HBITMAP)::SelectObject( hdcTemp, hBitmapNew );
				// 前の画面内容をコピーする
				::BitBlt( hdcTemp, 0, 0,
					t_min( nBmpWidthNew,m_nCompatBMPWidth ),
					t_min( nBmpHeightNew, m_nCompatBMPHeight ),
					m_hdcCompatDC, 0, 0, SRCCOPY );
				::SelectObject( hdcTemp, hBitmapOld );
				::SelectObject( m_hdcCompatDC, m_hbmpCompatBMPOld );
				::DeleteObject( m_hbmpCompatBMP );
			}
			::DeleteDC( hdcTemp );
		}else{
			// BMPの新規作成
			hBitmapNew = ::CreateCompatibleBitmap( hdc, nBmpWidthNew, nBmpHeightNew );
		}
		if( hBitmapNew ){
			m_hbmpCompatBMP = hBitmapNew;
			m_nCompatBMPWidth = nBmpWidthNew;
			m_nCompatBMPHeight = nBmpHeightNew;
			m_hbmpCompatBMPOld = (HBITMAP)::SelectObject( m_hdcCompatDC, m_hbmpCompatBMP );
		}else{
			// 互換BMPの作成に失敗
			// 今後も失敗を繰り返す可能性が高いので
			// m_hdcCompatDCをNULLにすることで画面バッファ機能をこのウィンドウのみ無効にする。
			//	2007.09.29 genta 関数化．既存のBMPも解放
			UseCompatibleDC(FALSE);
		}
		::ReleaseDC( GetHwnd(), hdc );
	}
	return nullptr != m_hbmpCompatBMP;
}

/*!
	互換メモリBMPを削除

	@note 分割ビューが非表示になった場合と
		親ウィンドウが非表示・最小化された場合に削除される。
	@date 2007.09.09 Moca 新規作成
*/
void CEditView::DeleteCompatibleBitmap()
{
	if( m_hbmpCompatBMP ){
		::SelectObject( m_hdcCompatDC, m_hbmpCompatBMPOld );
		::DeleteObject( m_hbmpCompatBMP );
		m_hbmpCompatBMP = nullptr;
		m_hbmpCompatBMPOld = nullptr;
		m_nCompatBMPWidth = -1;
		m_nCompatBMPHeight = -1;
	}
}

/** 画面キャッシュ用CompatibleDCを用意する

	@param[in] TRUE: 画面キャッシュON

	@date 2007.09.30 genta 関数化
*/
void CEditView::UseCompatibleDC(BOOL fCache)
{
	// From Here 2007.09.09 Moca 互換BMPによる画面バッファ
	if( fCache ){
		if( m_hdcCompatDC == nullptr ){
			HDC			hdc;
			hdc = ::GetDC( GetHwnd() );
			m_hdcCompatDC = ::CreateCompatibleDC( hdc );
			::ReleaseDC( GetHwnd(), hdc );
			DEBUG_TRACE(L"CEditView::UseCompatibleDC: Created\n", fCache);
		}
		else {
			DEBUG_TRACE(L"CEditView::UseCompatibleDC: Reused\n", fCache);
		}
	}
	else {
		//	CompatibleBitmapが残っているかもしれないので最初に削除
		DeleteCompatibleBitmap();
		if( m_hdcCompatDC != nullptr ){
			::DeleteDC( m_hdcCompatDC );
			DEBUG_TRACE(L"CEditView::UseCompatibleDC: Deleted.\n");
			m_hdcCompatDC = nullptr;
		}
	}
}
