/*! @file */
/*
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "CMainStatusBar.h"
#include "window/CEditWnd.h"
#include "CEditApp.h"
#include "apiwrap/CommonControl.h"
#include "apiwrap/DarkMode.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include "charset/CCodeFactory.h"

#include <vector>

namespace {

constexpr UINT_PTR kSakuraStatusBarSubclassId = 1;

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) noexcept
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		::FillRect(dc, &rect, brush);
		::DeleteObject(brush);
	}
}

void DrawBranchIcon(HDC dc, const RECT& part, COLORREF color, UINT dpi) noexcept
{
	if (dc == nullptr) return;
	const auto box = workbench::icons::LeadingStatusIconBounds(
		{ part.left, part.top, part.right, part.bottom }, dpi);
	if (box.Width() <= 0 || box.Height() <= 0) return;
	workbench::icons::codicons::Draw(dc, box,
		workbench::icons::codicons::Icon::GitBranch, color);
}

} // namespace

/*!
 * 文字コードの16進表示
 *
 * ステータスバー表示用に文字を16進表記に変換する
 *
 * @param [in] eCodeType 文字コードセット種別
 * @param [in] wide 表示する文字
 * @param [in] sStatusBar 共通設定 ステータスバー
 */
/* static */ std::wstring CMainStatusBar::UnicodeToHex(ECodeType eCodeType, std::wstring_view wide, const CommonSetting_Statusbar& sStatusBar)
{
	// 出力先バッファを確保する
	std::wstring buffer(32, L'\0');

	// Hex変換
	if (CCodeFactory::CreateCodeBase(eCodeType)->UnicodeToHex(std::data(wide), int(std::size(wide)), std::data(buffer), &sStatusBar) != RESULT_COMPLETE) {
		// 変換に失敗したらUNICODEで変換する
		CCodeFactory::CreateCodeBase(CODE_UTF16BE)->UnicodeToHex(std::data(wide), int(std::size(wide)), std::data(buffer), &sStatusBar);
	}

	// 出力先バッファのサイズを調整する
	buffer.resize(::wcsnlen(buffer.c_str(), std::size(buffer)));

	return buffer;
}

CMainStatusBar::CMainStatusBar(CEditWnd* pOwner)
: m_pOwner(pOwner)
{
}

//	キーワード：ステータスバー順序
/* ステータスバー作成 */
void CMainStatusBar::CreateStatusBar()
{
	if( m_hwndStatusBar )return;

	/* ステータスバー */
	m_hwndStatusBar = ::CreateWindowEx(
		WS_EX_RIGHT | WS_EX_COMPOSITED,
		STATUSCLASSNAME,
		nullptr,
		WS_CHILD/* | WS_VISIBLE*/ | SBARS_SIZEGRIP,	// 2007.03.08 ryoji WS_VISIBLE 除去
		0, 0, 0, 0, // X, Y, nWidth, nHeight
		m_pOwner->GetHwnd(),
		(HMENU)IDW_STATUSBAR,
		CEditApp::getInstance()->GetAppInstance(),
		nullptr
	);

	InstallPaletteSubclass();

	/* プログレスバー */
	m_hwndProgressBar = ::CreateWindowEx(
		WS_EX_TOOLWINDOW,
		PROGRESS_CLASS,
		nullptr,
		WS_CHILD /*|  WS_VISIBLE*/,
		3,
		5,
		150,
		13,
		m_hwndStatusBar,
		nullptr,
		CEditApp::getInstance()->GetAppInstance(),
		nullptr
	);

	DarkMode::setProgressBarCtrlSubclass(m_hwndProgressBar);

	if( nullptr != m_pOwner->m_cFuncKeyWnd.GetHwnd() ){
		m_pOwner->m_cFuncKeyWnd.SizeBox_ONOFF( FALSE );
	}

	//スプリッターの、サイズボックスの位置を変更
	m_pOwner->m_cSplitterWnd.DoSplit( -1, -1);
}

/* ステータスバー破棄 */
void CMainStatusBar::DestroyStatusBar()
{
	if( nullptr != m_hwndProgressBar ){
		::DestroyWindow( m_hwndProgressBar );
		m_hwndProgressBar = nullptr;
	}
	if (m_hwndStatusBar != nullptr) {
		::RemoveWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId);
		::DestroyWindow(m_hwndStatusBar);
	}
	m_hwndStatusBar = nullptr;

	if( nullptr != m_pOwner->m_cFuncKeyWnd.GetHwnd() ){
		bool bSizeBox;
		if( GetDllShareData().m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ){	/* ファンクションキー表示位置／0:上 1:下 */
			/* サイズボックスの表示／非表示切り替え */
			bSizeBox = false;
		}
		else{
			bSizeBox = true;
			/* ステータスパーを表示している場合はサイズボックスを表示しない */
			if( nullptr != m_hwndStatusBar ){
				bSizeBox = false;
			}
		}
		m_pOwner->m_cFuncKeyWnd.SizeBox_ONOFF( bSizeBox );
	}
	//スプリッターの、サイズボックスの位置を変更
	m_pOwner->m_cSplitterWnd.DoSplit( -1, -1 );
}

void CMainStatusBar::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (m_hwndStatusBar != nullptr) {
		::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
	}
}

void CMainStatusBar::SetScmText(std::wstring text)
{
	if (m_scmText == text) return;
	m_scmText = std::move(text);
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

void CMainStatusBar::InstallPaletteSubclass() noexcept
{
	if (m_hwndStatusBar == nullptr) return;
	// The recursive darkmodelib pass runs after child creation. Remove only its
	// status-bar painter, then put Sakura's palette-aware painter last in the chain.
	DarkMode::removeStatusBarCtrlSubclass(m_hwndStatusBar);
	::RemoveWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId);
	::SetWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId,
		reinterpret_cast<DWORD_PTR>(this));
	::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

LRESULT CALLBACK CMainStatusBar::StatusBarSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR referenceData)
{
	auto* self = reinterpret_cast<CMainStatusBar*>(referenceData);
	switch (message) {
	case WM_ERASEBKGND:
		return TRUE;
	case WM_PRINTCLIENT:
		if (self != nullptr && reinterpret_cast<HDC>(wParam) != nullptr) {
			self->PaintStatusBar(reinterpret_cast<HDC>(wParam));
			return 0;
		}
		break;
	case WM_PAINT:
		if (self != nullptr) {
			PAINTSTRUCT paint{};
			const HDC dc = ::BeginPaint(window, &paint);
			self->PaintStatusBar(dc);
			::EndPaint(window, &paint);
			return 0;
		}
		break;
	case WM_THEMECHANGED:
		::InvalidateRect(window, nullptr, FALSE);
		break;
	case WM_NCDESTROY:
		::RemoveWindowSubclass(window, StatusBarSubclassProc, subclassId);
		if (self != nullptr && self->m_hwndStatusBar == window) {
			self->m_hwndStatusBar = nullptr;
		}
		break;
	default:
		break;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

void CMainStatusBar::PaintStatusBar(HDC dc) const noexcept
{
	if (dc == nullptr || m_hwndStatusBar == nullptr) return;

	RECT client{};
	::GetClientRect(m_hwndStatusBar, &client);
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if (width <= 0 || height <= 0) return;

	const HDC buffer = ::CreateCompatibleDC(dc);
	const HBITMAP bitmap = buffer == nullptr ? nullptr : ::CreateCompatibleBitmap(dc, width, height);
	const HGDIOBJ oldBitmap = bitmap == nullptr ? nullptr : ::SelectObject(buffer, bitmap);
	HDC target = oldBitmap == nullptr ? dc : buffer;

	// Match the VS Code workbench status treatment: the complete bar carries
	// the active accent, while every label and line icon uses its paired
	// high-contrast foreground.
	FillSolidRect(target, client, m_palette.accent.ToColorRef());
	::SetBkMode(target, TRANSPARENT);
	::SetTextColor(target, m_palette.highlightText.ToColorRef());
	const HFONT font = reinterpret_cast<HFONT>(::SendMessageW(m_hwndStatusBar, WM_GETFONT, 0, 0));
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(target, font);
	int scmWidth = 0;
	if (!m_scmText.empty()) {
		SIZE extent{};
		if (::GetTextExtentPoint32W(target, m_scmText.c_str(), static_cast<int>(m_scmText.size()), &extent)) {
			const UINT scmDpi = static_cast<UINT>(::GetDpiForWindow(m_hwndStatusBar));
			const int textInset = workbench::icons::StatusTextInsetPixels(scmDpi);
			scmWidth = std::min(width, static_cast<int>(extent.cx) + textInset + 8);
			const RECT scmIconRect{ 0, 0, scmWidth, height };
			DrawBranchIcon(target, scmIconRect, m_palette.highlightText.ToColorRef(), scmDpi);
			RECT scmRect{ textInset, 0, scmWidth - 4, height };
			::DrawTextW(target, m_scmText.c_str(), static_cast<int>(m_scmText.size()), &scmRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
	}

	const int partCount = static_cast<int>(::SendMessageW(m_hwndStatusBar, SB_GETPARTS, 0, 0));
	const UINT dpi = static_cast<UINT>(::GetDpiForWindow(m_hwndStatusBar));
	const int itemInset = workbench::icons::ScaleDip(workbench::icons::kStatusItemInsetDip, dpi);
	for (int part = 0; part < partCount; ++part) {
		RECT partRect{};
		if (::SendMessageW(m_hwndStatusBar, SB_GETRECT, part, reinterpret_cast<LPARAM>(&partRect)) == FALSE) continue;
		if (part == 0 && scmWidth > partRect.left) partRect.left = std::min<LONG>(partRect.right, scmWidth);
		const LRESULT textInfo = ::SendMessageW(m_hwndStatusBar, SB_GETTEXTLENGTHW, part, 0);
		const UINT textLength = LOWORD(textInfo);
		const UINT style = HIWORD(textInfo);
		std::vector<wchar_t> text(static_cast<size_t>(textLength) + 1, L'\0');
		const LRESULT itemData = ::SendMessageW(m_hwndStatusBar, SB_GETTEXTW, part,
			reinterpret_cast<LPARAM>(text.data()));
		if ((style & SBT_OWNERDRAW) != 0) {
			DRAWITEMSTRUCT drawItem{ ODT_STATIC, 0, static_cast<UINT>(part), ODA_DRAWENTIRE, 0,
				m_hwndStatusBar, target, partRect, static_cast<ULONG_PTR>(itemData) };
			::SendMessageW(::GetParent(m_hwndStatusBar), WM_DRAWITEM,
				static_cast<WPARAM>(::GetDlgCtrlID(m_hwndStatusBar)), reinterpret_cast<LPARAM>(&drawItem));
		} else if (textLength != 0) {
			partRect.left = std::min<LONG>(partRect.right, partRect.left + itemInset);
			partRect.right = std::max<LONG>(partRect.left, partRect.right - itemInset);
			::DrawTextW(target, text.data(), static_cast<int>(textLength), &partRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
	}

	if (oldFont != nullptr) ::SelectObject(target, oldFont);
	if (oldBitmap != nullptr) {
		::BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
		::SelectObject(buffer, oldBitmap);
	}
	if (bitmap != nullptr) ::DeleteObject(bitmap);
	if (buffer != nullptr) ::DeleteDC(buffer);
}

/*!
	@brief メッセージの表示
	
	指定されたメッセージをステータスバーに表示する．
	メニューバー右端に入らないものや，桁位置表示を隠したくないものに使う
	
	呼び出し前にSendStatusMessage2IsEffective()で処理の有無を
	確認することで無駄な処理を省くことが出来る．

	@param msg [in] 表示するメッセージ
	@date 2005.07.09 genta 新規作成
	
	@sa SendStatusMessage2IsEffective
*/
void CMainStatusBar::SendStatusMessage2( const WCHAR* msg )
{
	if( nullptr != m_hwndStatusBar ){
		SetStatusText(0, SBT_NOBORDERS, msg);
	}
}

/*!
	@brief 文字列をステータスバーの指定されたパートに表示する
	
	@param nIndex [in] パートのインデクス
	@param nOption [in] 描画オペレーションタイプ
	@param pszText [in] 表示テキスト
	@param textLen [in] 表示テキストの文字数
*/
bool CMainStatusBar::SetStatusText(int nIndex, int nOption, const WCHAR* pszText, size_t textLen /* = SIZE_MAX */)
{
	if( !m_hwndStatusBar ){
		assert(m_hwndStatusBar != nullptr);
		return false;
	}
	// StatusBar_SetText 関数を呼びだすかどうかを判定する
	// （StatusBar_SetText は SB_SETTEXT メッセージを SendMessage で送信する）
	bool bDraw = true;
	do {
		// オーナードローの場合は SB_SETTEXT メッセージを無条件に発行するように判定
		// 本来表示に変化が無い場合には呼び出さない方が表示のちらつきが減るので好ましいが
		// 判定が難しいので諦める
		if( nOption == SBT_OWNERDRAW ){
			break;
		}
		// オーナードローではない場合で NULLの場合は空文字に置き換える
		// NULL を渡しても問題が無いのかどうか公式ドキュメントに記載されていない
		// NULL のままでも問題は発生しないようだが念の為に対策を追加
		if( pszText == nullptr ){
			static const wchar_t emptyStr[] = L"";
			pszText = emptyStr;
			textLen = 0;
		}
		LRESULT res = ::ApiWrap::StatusBar_GetTextLength( m_hwndStatusBar, nIndex );
		// 表示オペレーション値が変化する場合は SB_SETTEXT メッセージを発行
		if( HIWORD(res) != nOption ){
			break;
		}
		size_t prevTextLen = LOWORD(res);
		WCHAR prev[1024];
		// 設定済みの文字列長が長過ぎて取得できない場合は、SB_SETTEXT メッセージを発行
		if( prevTextLen >= int(std::size(prev)) ){
			break;
		}
		// 設定する文字列長パラメータが SIZE_MAX（引数のデフォルト値）な場合は文字列長を取得
		if( textLen == SIZE_MAX ){
			textLen = wcslen(pszText);
		}
		// 設定済みの文字列長と設定する文字列長が異なる場合は、SB_SETTEXT メッセージを発行
		if( prevTextLen != textLen ){
			break;
		}
		if( prevTextLen > 0 ){
			ApiWrap::StatusBar_GetText( m_hwndStatusBar, nIndex, prev );
			// 設定済みの文字列と設定する文字列を比較して異なる場合は、SB_SETTEXT メッセージを発行
			bDraw = wcscmp(prev, pszText) != 0;
		}
		else {
			// 設定する文字列長が0の場合は設定する文字列長が0より大きい場合のみ設定する（既に空文字なら空文字を設定する必要は無い為）
			bDraw = textLen > 0;
		}
	}while (false);
	if (bDraw) {
		ApiWrap::StatusBar_SetText(m_hwndStatusBar, nIndex | nOption, pszText);
	}
	return bDraw;
}

//! プログレスバーの表示/非表示を切り替える
void CMainStatusBar::ShowProgressBar(bool bShow) const {
	if (m_hwndStatusBar && m_hwndProgressBar) {
		// プログレスバーを表示するステータスバー上の領域を取得
		RECT rcProgressArea = {};
		ApiWrap::StatusBar_GetRect(m_hwndStatusBar, 0, &rcProgressArea);
		if (bShow) {
			::ShowWindow(m_hwndProgressBar, SW_SHOW);
		} else {
			::ShowWindow(m_hwndProgressBar, SW_HIDE);
		}
		// プログレスバー表示領域を再描画
		::InvalidateRect(m_hwndStatusBar, &rcProgressArea, TRUE);
		::UpdateWindow(m_hwndStatusBar);
	}
}
