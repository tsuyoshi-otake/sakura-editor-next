/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include <vector>
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
	if( m_bMiniMap ){
		// The minimap is navigational context, not a second editor.  Pull syntax
		// colors toward their background so they remain legible without competing
		// with the active document.
		fgcolor = MakeColor2(fgcolor, bkcolor, 72);
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

void CEditView::DrawMiniMapOverview(HDC hdc)
{
	if( !m_bMiniMap || hdc == nullptr ) return;
	RECT client{};
	if( !GetClientRect(&client) ) return;
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if( width <= 0 || height <= 0 ) return;

	CTypeSupport textType(this, COLORIDX_TEXT);
	const COLORREF background = textType.GetBackColor();
	const HBRUSH dcBrush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
	const HBRUSH previousBrush = static_cast<HBRUSH>(::SelectObject(hdc, dcBrush));
	const COLORREF previousBrushColor = ::SetDCBrushColor(hdc, background);
	::FillRect(hdc, &client, dcBrush);
	::SetDCBrushColor(hdc, previousBrushColor);
	::SelectObject(hdc, previousBrush);

	const auto lineCount = static_cast<std::int64_t>(m_pcEditDoc->m_cLayoutMgr.GetLineCount());
	if( lineCount <= 0 ) return;

	const COLORREF foreground = MakeColor2(textType.GetTextColor(), background, 68);
	const auto layoutGeneration = m_pcEditDoc->m_cLayoutMgr.GetLayoutGeneration();
	const auto cacheMatches = m_miniMapOverviewCache.valid
		&& m_miniMapOverviewCache.document == m_pcEditDoc
		&& m_miniMapOverviewCache.layoutGeneration == layoutGeneration
		&& m_miniMapOverviewCache.lineCount == lineCount
		&& m_miniMapOverviewCache.width == width
		&& m_miniMapOverviewCache.height == height
		&& m_miniMapOverviewCache.background == background
		&& m_miniMapOverviewCache.foreground == foreground;
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
		next.lineCount = lineCount;
		next.width = width;
		next.height = height;
		next.background = background;
		next.foreground = foreground;
		next.rowStart.assign(static_cast<std::size_t>(height), width);
		next.rowEnd.assign(static_cast<std::size_t>(height), 0);
		constexpr int kMaxColumns = 160;
		constexpr int kLeftPadding = 4;
		const int drawableWidth = (std::max)(1, width - kLeftPadding - 2);
		const CLayout* layout = m_pcEditDoc->m_cLayoutMgr.GetTopLayout();
		std::int64_t line = 0;
		while( layout != nullptr && line < lineCount ) {
			const wchar_t* text = layout->GetPtr();
			const int length = (std::min)(static_cast<int>(layout->GetLengthWithoutEOL()), kMaxColumns);
			int indent = 0;
			while( indent < length && (text[indent] == L' ' || text[indent] == L'\t') ) ++indent;
			if( length > indent ) {
				const int row = (std::min)(height - 1, minimap::LineToPixel(line, lineCount, height));
				const int start = kLeftPadding + (indent * drawableWidth) / kMaxColumns;
				const int end = kLeftPadding + ((std::max)(indent + 1, length) * drawableWidth) / kMaxColumns;
				next.rowStart[static_cast<std::size_t>(row)] = (std::min)(next.rowStart[static_cast<std::size_t>(row)], start);
				next.rowEnd[static_cast<std::size_t>(row)] = (std::max)(next.rowEnd[static_cast<std::size_t>(row)], end);
			}
			layout = layout->GetNextLayout();
			++line;
		}

		next.valid = true;
		m_miniMapOverviewCache = std::move(next);
		if (traceCache) {
			LARGE_INTEGER cacheBuildEnd{};
			::QueryPerformanceCounter(&cacheBuildEnd);
			CStartupTrace::AccumulateStartupMiniMapCacheLookup(
				false, cacheBuildEnd.QuadPart - cacheBuildStart.QuadPart, line);
		}
	} else {
		CStartupTrace::AccumulateStartupMiniMapCacheLookup(true);
	}
	const HPEN dcPen = static_cast<HPEN>(::GetStockObject(DC_PEN));
	const HPEN previousPen = static_cast<HPEN>(::SelectObject(hdc, dcPen));
	const COLORREF previousPenColor = ::SetDCPenColor(hdc, foreground);
	for( int row = 0; row < height; ++row ) {
		const int start = m_miniMapOverviewCache.rowStart[static_cast<std::size_t>(row)];
		const int end = m_miniMapOverviewCache.rowEnd[static_cast<std::size_t>(row)];
		if( end <= start ) continue;
		::MoveToEx(hdc, start, client.top + row, nullptr);
		::LineTo(hdc, (std::min)(end, static_cast<int>(client.right)), client.top + row);
	}
	::SetDCPenColor(hdc, previousPenColor);
	::SelectObject(hdc, previousPen);
}

void CEditView::DrawMiniMapViewport(HDC hdc)
{
	if( !m_bMiniMap || hdc == nullptr ) return;
	RECT client{};
	if( !GetClientRect(&client) ) return;
	CEditView& activeView = GetEditWnd().GetActiveView();
	const auto lineCount = static_cast<std::int64_t>(m_pcEditDoc->m_cLayoutMgr.GetLineCount());
	const int height = client.bottom - client.top;
	if( lineCount <= 0 || height <= 0 ) return;
	const auto band = minimap::ViewportToPixels(
		static_cast<Int>(activeView.GetTextArea().GetViewTopLine()),
		static_cast<Int>(activeView.GetTextArea().GetBottomLine()), lineCount, height);
	RECT viewport{
		client.left,
		client.top + band.top,
		client.right,
		client.top + band.bottom
	};
	viewport.top = (std::clamp)(viewport.top, client.top, client.bottom);
	viewport.bottom = (std::clamp)(viewport.bottom, client.top, client.bottom);
	if( viewport.bottom <= viewport.top ) viewport.bottom = (std::min)(client.bottom, viewport.top + 2);
	if( viewport.right <= viewport.left || viewport.bottom <= viewport.top ) return;

	// VS Code represents the visible editor range as a quiet, neutral overlay
	// across the minimap rather than as a high-contrast outline.  Blend after
	// the map is rendered so syntax marks remain visible through the band.
	CTypeSupport textType(this, COLORIDX_TEXT);
	if( m_hdcMiniMapViewport == nullptr ){
		m_hdcMiniMapViewport = ::CreateCompatibleDC(hdc);
	}
	if( m_hdcMiniMapViewport == nullptr ) return;
	if( m_hbmpMiniMapViewport == nullptr ){
		m_hbmpMiniMapViewport = ::CreateCompatibleBitmap(hdc, 1, 1);
		if( m_hbmpMiniMapViewport == nullptr ) return;
		m_hbmpMiniMapViewportOld = static_cast<HBITMAP>(::SelectObject(m_hdcMiniMapViewport, m_hbmpMiniMapViewport));
		if( m_hbmpMiniMapViewportOld == nullptr || m_hbmpMiniMapViewportOld == HGDI_ERROR ){
			::DeleteObject(m_hbmpMiniMapViewport);
			m_hbmpMiniMapViewport = nullptr;
			m_hbmpMiniMapViewportOld = nullptr;
			return;
		}
	}
	::SetPixelV(m_hdcMiniMapViewport, 0, 0, textType.GetTextColor());
	BLENDFUNCTION blend{ AC_SRC_OVER, 0, 18, 0 };
	::AlphaBlend(
		hdc,
		viewport.left,
		viewport.top,
		viewport.right - viewport.left,
		viewport.bottom - viewport.top,
		m_hdcMiniMapViewport,
		0,
		0,
		1,
		1,
		blend
	);
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
	if( m_bMiniMap ){
		const bool measureStartupPaint = CStartupTrace::IsEnabled()
			&& GetEditWnd().IsStartupDrawCommitting();
		LARGE_INTEGER begin{};
		if (measureStartupPaint) {
			::QueryPerformanceCounter(&begin);
		}
		DrawMiniMapOverview(_hdc);
		DrawMiniMapViewport(_hdc);
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
		int nPosBgn = pInfo->m_nPosInLogic; // Logic
		int nPosLength = 0;
		CLayoutInt nDrawX = pInfo->m_pDispPos->GetDrawCol(); // Layout
		const int nDrawBlockLen = 1000; // ExtTextOutの長さ制限にかからない適当な値
		int nPosTo = pcLayout->GetLogicOffset() + pcLayout->GetLengthWithEOL();
		CFigureManager* pcFigureManager = CFigureManager::getInstance();
		FigureRenderType prevRenderType = CFigure_Text::RenderType_None;
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
			const bool renderTypeBoundary = prevRenderType != nextRenderType;
			const bool lengthBoundary = nDrawBlockLen < nPosLength;
			if (CFigure_Text::IsRenderType_Block(prevRenderType) &&
				(renderTypeBoundary || lengthBoundary)) {
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
