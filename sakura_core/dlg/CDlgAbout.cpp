/*!	@file
	@brief バージョン情報ダイアログ

	@author Norio Nakatani
	@date	1998/3/13 作成
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta, jepro
	Copyright (C) 2001, genta, jepro
	Copyright (C) 2002, MIK, genta, aroka
	Copyright (C) 2003, Moca
	Copyright (C) 2004, Moca
	Copyright (C) 2005, genta
	Copyright (C) 2006, Moca
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
#include "dlg/CDlgAbout.h"
#include "uiparts/HandCursor.h"
#include "util/file.h"
#include "util/module.h"
#include "util/os.h"
#include "util/shell.h"
#include "util/window.h"
#include "sakura_rc.h" // 2002/2/10 aroka 復帰
#include "version.h"
#include "apiwrap/StdApi.h"
#include "apiwrap/StdControl.h"
#include "CSelectLang.h"
#include "sakura.hh"
#include "config/app_constants.h"
#include "config/system_constants.h"
#include "theme/CThemeService.h"

// バージョン情報 CDlgAbout.cpp	//@@@ 2002.01.07 add start MIK
const DWORD p_helpids[] = {	//12900
	IDOK,					HIDOK_ABOUT,
	IDC_EDIT_ABOUT,			HIDC_ABOUT_EDIT_ABOUT,
//	IDC_STATIC_URL_UR,		12970,
//	IDC_STATIC_URL_ORG,		12971,
//	IDC_STATIC_UPDATE,		12972,
//	IDC_STATIC_VER,			12973,
//	IDC_STATIC,				-1,
	0, 0
};	//@@@ 2002.01.07 add end MIK

//	From Here Feb. 7, 2002 genta
// 2006.01.17 Moca COMPILER_VERを追加
// 2010.04.15 Moca icc/dmcを追加しCPUを分離
#if defined(_M_AMD64)
#  define TARGET_M_SUFFIX "_A64"
#else
#  define TARGET_M_SUFFIX ""
#endif

#if defined(__BORLANDC__)
// borland c++
// http://docwiki.embarcadero.com/RADStudio/Rio/en/Predefined_Macros
// http://docwiki.embarcadero.com/RADStudio/Rio/en/Predefined_Macros#C.2B.2B_Compiler_Versions_in_Predefined_Macros
#  define COMPILER_TYPE "B"
#  define COMPILER_VER  __BORLANDC__ 
#elif defined(__GNUG__)
// __GNUG__ = (__GNUC__ && __cplusplus)
// GNU C++
// https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
#  define COMPILER_TYPE "G"
#  define COMPILER_VER (__GNUC__ * 10000 + __GNUC_MINOR__  * 100 + __GNUC_PATCHLEVEL__)
#elif defined(__INTEL_COMPILER)
// Intel Compiler
// https://software.intel.com/en-us/cpp-compiler-developer-guide-and-reference-additional-predefined-macros
#  define COMPILER_TYPE "I"
#  define COMPILER_VER __INTEL_COMPILER
#elif defined(__DMC__)
// Digital Mars C/C++
// https://digitalmars.com/ctg/predefined.html
#  define COMPILER_TYPE "D"
#  define COMPILER_VER __DMC__
#elif defined(_MSC_VER)
// https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2019
#  define COMPILER_TYPE "V"
#  define COMPILER_VER _MSC_VER
#else
// unknown
#  define COMPILER_TYPE "U"
#  define COMPILER_VER 0
#endif
//	To Here Feb. 7, 2002 genta

#define TARGET_STRING_MODEL "WP"

#ifdef _DEBUG
	#define TARGET_DEBUG_MODE "D"
#else
	#define TARGET_DEBUG_MODE "R"
#endif

#define TSTR_TARGET_MODE _T(TARGET_STRING_MODEL) _T(TARGET_DEBUG_MODE)

#if defined(CI_BUILD_URL)
#pragma message("CI_BUILD_URL: " CI_BUILD_URL)
#endif
#if defined(CI_BUILD_NUMBER_LABEL)
#pragma message("CI_BUILD_NUMBER_LABEL: " CI_BUILD_NUMBER_LABEL)
#endif

namespace {
	HFONT CreateAboutFont( HWND dialog, int pointSize, int weight )
	{
		const auto baseFont = reinterpret_cast<HFONT>( ::SendMessageW( dialog, WM_GETFONT, 0, 0 ) );
		LOGFONTW font{};
		if( ::GetObjectW( baseFont, sizeof( font ), &font ) == 0 ) return nullptr;
		const UINT dpi = ::GetDpiForWindow( dialog );
		font.lfHeight = -::MulDiv( pointSize, dpi == 0 ? 96 : dpi, 72 );
		font.lfWidth = 0;
		font.lfWeight = weight;
		return ::CreateFontIndirectW( &font );
	}

	void CompactAboutFooter( HWND dialog )
	{
		const int omittedRows =
			(::GetDlgItem( dialog, IDC_STATIC_GIT_CAPTION ) == nullptr ? 1 : 0) +
			(::GetDlgItem( dialog, IDC_STATIC_URL_CI_BUILD_CAPTION ) == nullptr ? 1 : 0) +
			(::GetDlgItem( dialog, IDC_STATIC_URL_GITHUB_CAPTION ) == nullptr ? 1 : 0);
		if( omittedRows == 0 ) return;

		RECT units{ 0, 0, 0, 14 * omittedRows };
		if( !::MapDialogRect( dialog, &units ) ) return;
		const int delta = units.bottom;
		RECT bounds{};
		if( !::GetWindowRect( dialog, &bounds ) ) return;
		constexpr int footerIds[]{ IDC_STATIC_ABOUT_AUTHOR, IDC_STATIC_ABOUT_FORK_COPYRIGHT,
			IDC_STATIC_ABOUT_COPYRIGHT,
			IDC_STATIC_ABOUT_TRANSLATION, IDC_BUTTON_COPY, IDOK };
		HWND children[std::size( footerIds )]{};
		RECT positions[std::size( footerIds )]{};
		for( size_t index = 0; index < std::size( footerIds ); ++index ){
			children[index] = ::GetDlgItem( dialog, footerIds[index] );
			if( children[index] == nullptr || !::GetWindowRect( children[index], &positions[index] ) ) return;
			::MapWindowPoints( nullptr, dialog, reinterpret_cast<POINT*>( &positions[index] ), 2 );
		}

		for( size_t index = 0; index < std::size( footerIds ); ++index ){
			const RECT& position = positions[index];
			if( !::SetWindowPos( children[index], nullptr, position.left, position.top - delta, 0, 0,
				SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE ) ){
				for( size_t previous = 0; previous < index; ++previous ){
					::SetWindowPos( children[previous], nullptr, positions[previous].left, positions[previous].top,
						0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE );
				}
				return;
			}
		}
		if( ::SetWindowPos( dialog, nullptr, bounds.left, bounds.top + delta / 2,
			bounds.right - bounds.left, bounds.bottom - bounds.top - delta,
			SWP_NOZORDER | SWP_NOACTIVATE ) ) return;
		for( size_t index = 0; index < std::size( footerIds ); ++index ){
			::SetWindowPos( children[index], nullptr, positions[index].left, positions[index].top,
				0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE );
		}
	}
}

CDlgAbout::~CDlgAbout()
{
	if( m_headingFont != nullptr ) ::DeleteObject( m_headingFont );
	if( m_sectionFont != nullptr ) ::DeleteObject( m_sectionFont );
}

/* モーダルダイアログの表示 */
int CDlgAbout::DoModal( HINSTANCE hInstance, HWND hwndParent )
{
	return (int)CDialog::DoModal( hInstance, hwndParent, IDD_ABOUT, (LPARAM)nullptr );
}

/*! 初期化処理
	@date 2008.05.05 novice GetModuleHandle(NULL)→NULLに変更
	@date 2011.04.10 nasukoji	各国語メッセージリソース対応
	@date 2013.04.07 novice svn revision 情報追加
*/
BOOL CDlgAbout::OnInitDialog( HWND hwndDlg, WPARAM wParam, LPARAM lParam )
{
	_SetHwnd( hwndDlg );

	WCHAR			szFile[_MAX_PATH];

	/* この実行ファイルの情報 */
	::GetModuleFileName( nullptr, szFile, int(std::size(szFile)) );
	
	/* バージョン情報 */
	//	Nov. 6, 2000 genta	Unofficial Releaseのバージョンとして設定
	//	Jun. 8, 2001 genta	GPL化に伴い、OfficialなReleaseとしての道を歩み始める
	//	Feb. 7, 2002 genta コンパイラ情報追加
	//	2004.05.13 Moca バージョン番号は、プロセスごとに取得する
	//	2010.04.15 Moca コンパイラ情報を分離/WINヘッダー,N_SHAREDATA_VERSION追加

	CNativeW cmemMsg;
	CNativeW buildSummary;

	// 1行目
	// バージョン情報
	DWORD dwVersionMS, dwVersionLS;
	GetAppVersionInfo( nullptr, VS_VERSION_INFO, &dwVersionMS, &dwVersionLS );
	
	cmemMsg.AppendStringF(
		L"%s Ver. %d.%d.%d.%d " LTEXT(BUILD_ENV_NAME) LTEXT(VERSION_HASH) L"\r\n",
		LS(STR_GSTR_APPNAME),
		HIWORD(dwVersionMS), LOWORD(dwVersionMS), HIWORD(dwVersionLS), LOWORD(dwVersionLS) // e.g. {2, 3, 2, 0}
	);

	// 2行目
#ifdef GIT_COMMIT_HASH
	cmemMsg.AppendString( L"(GitHash " _T(GIT_COMMIT_HASH) L")\r\n" );
	buildSummary.AppendString( L"Git: " _T(GIT_COMMIT_HASH) L"\r\n" );
#endif

	// 3行目
#ifdef GIT_REMOTE_ORIGIN_URL
	cmemMsg.AppendString( L"(GitURL " _T(GIT_REMOTE_ORIGIN_URL) L")\r\n");
#endif

	// 段落区切り
	cmemMsg.AppendString( L"\r\n" );

	// コンパイル情報
	CNativeW compileLine;
	compileLine.AppendStringF(
		L"Compiler: " _T(COMPILER_TYPE) _T(TARGET_M_SUFFIX) L"%d " TSTR_TARGET_MODE L" WIN%03x/I%03x/N%03x\r\n",
		COMPILER_VER, WINVER, _WIN32_IE, _WIN32_WINNT
	);
	cmemMsg.AppendString( compileLine.GetStringPtr() );
	buildSummary.AppendString( compileLine.GetStringPtr() );

	// 更新日情報
	//	Oct. 22, 2005 genta タイムスタンプ取得の共通関数利用
	CFileTime cFileTime;
	GetLastWriteTimestamp( szFile, &cFileTime );
	CNativeW modifiedLine;
	modifiedLine.AppendStringF(
		L"Modified: %d/%d/%d %02d:%02d:%02d\r\n",
		cFileTime->wYear,
		cFileTime->wMonth,
		cFileTime->wDay,
		cFileTime->wHour,
		cFileTime->wMinute,
		cFileTime->wSecond
	);
	cmemMsg.AppendString( modifiedLine.GetStringPtr() );
	buildSummary.AppendString( modifiedLine.GetStringPtr() );

	// パッチの情報をコンパイル時に渡せるようにする
#ifdef SKR_PATCH_INFO
	cmemMsg.AppendString( L"      " );
	const WCHAR szPatchInfo[] = SKR_PATCH_INFO;
	constexpr auto patchInfoLen = std::size(szPatchInfo) - 1;
	cmemMsg.AppendString( szPatchInfo, t_min(80, patchInfoLen) );
	buildSummary.AppendString( szPatchInfo, t_min(80, patchInfoLen) );
#endif
	cmemMsg.AppendString( L"\r\n");

	m_fullVersionInfo = cmemMsg.GetStringPtr();
	ApiWrap::DlgItem_SetText( GetHwnd(), IDC_EDIT_VER, buildSummary.GetStringPtr() );

	//	From Here Jun. 8, 2001 genta
	//	Edit Boxにメッセージを追加する．
	// 2011.06.01 nasukoji	各国語メッセージリソース対応
	LPCWSTR pszDesc = LS( IDS_ABOUT_DESCRIPTION );
	WCHAR szMsg[2048];
	if( pszDesc[0] != '\0' ) {
		wcsncpy_s(szMsg, pszDesc, _TRUNCATE);
		ApiWrap::DlgItem_SetText( GetHwnd(), IDC_EDIT_ABOUT, szMsg );
	}
	//	To Here Jun. 8, 2001 genta

	//	From Here Dec. 2, 2002 genta
	//	アイコンをカスタマイズアイコンに合わせる
	HICON hIcon = GetAppIcon( m_hInstance, ICON_DEFAULT_APP, FN_APP_ICON, false );
	HWND hIconWnd = GetItemHwnd( IDC_STATIC_MYICON );
	
	if( hIconWnd != nullptr && hIcon != nullptr ){
		ApiWrap::StCtl_SetIcon( hIconWnd, hIcon );
	}
	//	To Here Dec. 2, 2002 genta

	/* 基底クラスメンバ */
	(void)CDialog::OnInitDialog( GetHwnd(), wParam, lParam );

	::SetDlgItemTextW( GetHwnd(), IDC_STATIC_ABOUT_NAME, GetAppName() );
#ifdef _WIN64
	constexpr wchar_t architecture[] = L"x64";
#else
	constexpr wchar_t architecture[] = L"x86";
#endif
#ifdef _DEBUG
	constexpr wchar_t configuration[] = L"Debug";
#else
	constexpr wchar_t configuration[] = L"Release";
#endif
	WCHAR versionText[96]{};
	swprintf_s( versionText, L"%u.%u.%u.%u  |  %ls  |  %ls",
		HIWORD( dwVersionMS ), LOWORD( dwVersionMS ), HIWORD( dwVersionLS ), LOWORD( dwVersionLS ),
		architecture, configuration );
	::SetDlgItemTextW( GetHwnd(), IDC_STATIC_ABOUT_VERSION, versionText );

	m_headingFont = CreateAboutFont( GetHwnd(), 14, FW_SEMIBOLD );
	m_sectionFont = CreateAboutFont( GetHwnd(), 9, FW_SEMIBOLD );
	if( m_headingFont != nullptr ){
		::SendMessageW( GetItemHwnd( IDC_STATIC_ABOUT_NAME ), WM_SETFONT, reinterpret_cast<WPARAM>( m_headingFont ), TRUE );
	}
	if( m_sectionFont != nullptr ){
		::SendMessageW( GetItemHwnd( IDC_STATIC_ABOUT_BUILD_HEADING ), WM_SETFONT, reinterpret_cast<WPARAM>( m_sectionFont ), TRUE );
		::SendMessageW( GetItemHwnd( IDC_STATIC_ABOUT_LINKS_HEADING ), WM_SETFONT, reinterpret_cast<WPARAM>( m_sectionFont ), TRUE );
	}
	CompactAboutFooter( GetHwnd() );

	// URLウィンドウをサブクラス化する
	m_UrlUrWnd.SetSubclassWindow( GetItemHwnd( IDC_STATIC_URL_UR ) );
#ifdef GIT_REMOTE_ORIGIN_URL
	m_UrlGitWnd.SetSubclassWindow( GetItemHwnd( IDC_STATIC_URL_GIT ) );
#endif
#ifdef CI_BUILD_NUMBER_LABEL
	m_UrlBuildLinkWnd.SetSubclassWindow( GetItemHwnd( IDC_STATIC_URL_CI_BUILD ) );
#endif
#if defined( GITHUB_COMMIT_URL )
	m_UrlGitHubCommitWnd.SetSubclassWindow( GetItemHwnd( IDC_STATIC_URL_GITHUB_COMMIT ) );
#endif
#if defined( GITHUB_PR_HEAD_URL )
	m_UrlGitHubPRWnd.SetSubclassWindow( GetItemHwnd( IDC_STATIC_URL_GITHUB_PR ) );
#endif

	//	Oct. 22, 2005 genta 原作者ホームページが無くなったので削除
	//m_UrlOrgWnd.SubclassWindow( GetItemHwnd(IDC_STATIC_URL_ORG ) );

	/* デフォルトでは一番最初のタブオーダーの要素になるので明示的に OK ボタンにフォーカスを合わせる */
	::SetFocus(GetItemHwnd(IDOK));

	/*
		SetFocus() の効果を有効にするために FALSE を返す
		参考: https://msdn.microsoft.com/ja-jp/library/fwz35s59.aspx
	*/
	return FALSE;
}

BOOL CDlgAbout::OnBnClicked( int wID )
{
	switch( wID ){
	case IDC_BUTTON_COPY:
		SetClipboardText( GetHwnd(), m_fullVersionInfo.c_str(), static_cast<int>( m_fullVersionInfo.size() ) );
		return TRUE;
	default:
		break;
	}
	return CDialog::OnBnClicked( wID );
}

BOOL CDlgAbout::OnStnClicked( int wID )
{
	switch( wID ){
	//	2006.07.27 genta 原作者連絡先のボタンを削除 (ヘルプから削除されているため)
	case IDC_STATIC_URL_UR:
	case IDC_STATIC_URL_GIT:
//	case IDC_STATIC_URL_ORG:	del 2008/7/4 Uchi
		//	Web Browserの起動
		{
			WCHAR buf[512];
			::GetWindowText( GetItemHwnd( wID ), buf, int(std::size(buf)) );
			::ShellExecute( GetHwnd(), nullptr, buf, nullptr, nullptr, SW_SHOWNORMAL );
			return TRUE;
		}
	case IDC_STATIC_URL_CI_BUILD:
		{
#if defined(CI_BUILD_URL)
			::ShellExecute(GetHwnd(), nullptr, _T(CI_BUILD_URL), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(GIT_REMOTE_ORIGIN_URL)
			::ShellExecute(GetHwnd(), nullptr, _T(GIT_REMOTE_ORIGIN_URL), nullptr, nullptr, SW_SHOWNORMAL);
#endif
			return TRUE;
		}
	case IDC_STATIC_URL_GITHUB_COMMIT:
#if defined(GITHUB_COMMIT_URL)
		::ShellExecute(GetHwnd(), nullptr, _T(GITHUB_COMMIT_URL), nullptr, nullptr, SW_SHOWNORMAL);
#endif
		return TRUE;
	case IDC_STATIC_URL_GITHUB_PR:
#if defined(GITHUB_PR_HEAD_URL)
		::ShellExecute(GetHwnd(), nullptr, _T(GITHUB_PR_HEAD_URL), nullptr, nullptr, SW_SHOWNORMAL);
#endif
		return TRUE;
	default:
		break;
	}
	/* 基底クラスメンバ */
	return CDialog::OnStnClicked( wID );
}

//@@@ 2002.01.18 add start
LPVOID CDlgAbout::GetHelpIdTable(void)
{
	return (LPVOID)p_helpids;
}

BOOL CUrlWnd::SetSubclassWindow( HWND hWnd )
{
	// STATICウィンドウをサブクラス化する
	// 元のSTATICは WS_TABSTOP, SS_NOTIFY スタイルのものを使用すること
	if( GetHwnd() != nullptr )
		return FALSE;
	if( !IsWindow( hWnd ) )
		return FALSE;

	// サブクラス化を実行する
	LONG_PTR lptr;
	SetLastError( 0 );
	lptr = SetWindowLongPtr( hWnd, GWLP_USERDATA, (LONG_PTR)this );
	if( lptr == 0 && GetLastError() != 0 )
		return FALSE;
	m_pOldProc = (WNDPROC)SetWindowLongPtr( hWnd, GWLP_WNDPROC, (LONG_PTR)UrlWndProc );
	if( m_pOldProc == nullptr )
		return FALSE;
	m_hWnd = hWnd;

	// 下線付きフォントに変更する
	HFONT hFont;
	LOGFONT lf;
	hFont = (HFONT)SendMessageAny( hWnd, WM_GETFONT, (WPARAM)0, (LPARAM)0 );
	GetObject( hFont, sizeof(lf), &lf );
	lf.lfUnderline = TRUE;
	m_hFont = CreateFontIndirect( &lf );
	if(m_hFont != nullptr)
		SendMessageAny( hWnd, WM_SETFONT, (WPARAM)m_hFont, (LPARAM)FALSE );

	// 設定されているテキストを取得する
	std::wstring strText;
	if( ApiWrap::Wnd_GetText( hWnd, strText ) ){
		// サイズを調整する
		auto retSetText = OnSetText( strText.data(), strText.length() );
		return retSetText ? TRUE : FALSE;
	}

	return FALSE;
}

LRESULT CALLBACK CUrlWnd::UrlWndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	CUrlWnd* pUrlWnd = (CUrlWnd*)GetWindowLongPtr( hWnd, GWLP_USERDATA );

	HDC hdc;
	POINT pt;
	RECT rc;

	switch ( msg ) {
	case WM_SETCURSOR:
		// カーソル形状変更
		SetHandCursor();		// Hand Cursorを設定 2013/1/29 Uchi
		return (LRESULT)0;
	case WM_LBUTTONDOWN:
		// キーボードフォーカスを自分に当てる
		SendMessageAny( GetParent(hWnd), WM_NEXTDLGCTL, (WPARAM)hWnd, (LPARAM)1 );
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		// 再描画
		InvalidateRect( hWnd, nullptr, TRUE );
		UpdateWindow( hWnd );
		break;
	case WM_GETDLGCODE:
		// デフォルトプッシュボタンのように振舞う（Enterキーの有効化）
		// 方向キーは無効化（IEのバージョン情報ダイアログと同様）
		return DLGC_DEFPUSHBUTTON | DLGC_WANTARROWS;
	case WM_MOUSEMOVE:
		// カーソルがウィンドウ内に入ったらタイマー起動
		// ウィンドウ外に出たらタイマー削除
		// 各タイミングで再描画
		BOOL bHilighted;
		pt.x = LOWORD( lParam );
		pt.y = HIWORD( lParam );
		GetClientRect( hWnd, &rc );
		bHilighted = PtInRect( &rc, pt );
		if( bHilighted != pUrlWnd->m_bHilighted ){
			pUrlWnd->m_bHilighted = bHilighted;
			InvalidateRect( hWnd, nullptr, TRUE );
			if( pUrlWnd->m_bHilighted )
				SetTimer( hWnd, 1, 200, nullptr );
			else
				KillTimer( hWnd, 1 );
		}
		break;
	case WM_TIMER:
		// カーソルがウィンドウ外にある場合にも WM_MOUSEMOVE を送る
		GetCursorPos( &pt );
		ScreenToClient( hWnd, &pt );
		GetClientRect( hWnd, &rc );
		if( !PtInRect( &rc, pt ) )
			SendMessageAny( hWnd, WM_MOUSEMOVE, 0, MAKELONG( pt.x, pt.y ) );
		break;
	case WM_PAINT: {
		// ウィンドウの描画
		PAINTSTRUCT ps;
		HFONT hFont;
		HFONT hFontOld;
		WCHAR szText[512];

		hdc = BeginPaint( hWnd, &ps );

		// 現在のクライアント矩形、テキスト、フォントを取得する
		GetClientRect( hWnd, &rc );
		GetWindowText( hWnd, szText, int(std::size(szText)) );
		hFont = (HFONT)SendMessageAny( hWnd, WM_GETFONT, (WPARAM)0, (LPARAM)0 );

		// テキスト描画
		SetBkMode( hdc, TRANSPARENT );
		const auto mode = GetDllShareData().m_Common.m_sWindow.m_bDarkMode
			? theme::ThemeMode::Dark : theme::ThemeMode::Light;
		const auto palette = theme::CThemeService::EffectivePalette( mode );
		SetTextColor( hdc, (pUrlWnd->m_bHilighted ? palette.buttonHoverBackground : palette.accent).ToColorRef() );
		hFontOld = (HFONT)SelectObject( hdc, (HGDIOBJ)hFont );
		::TextOutW(hdc, ::DpiScaleX(2), 0, PSZ_ARGS(szText));
		SelectObject( hdc, (HGDIOBJ)hFontOld );

		// フォーカス枠描画
		if( GetFocus() == hWnd )
			DrawFocusRect( hdc, &rc );

		EndPaint( hWnd, &ps );
		return (LRESULT)0;
	}
	case WM_ERASEBKGND: {
		hdc = (HDC)wParam;
		GetClientRect( hWnd, &rc );

		// 親の背景を使い、ホバー色だけを変える。
		HBRUSH hbr = (HBRUSH)SendMessageAny( GetParent( hWnd ), WM_CTLCOLORSTATIC, wParam, (LPARAM)hWnd );
		HBRUSH hbrOld = (HBRUSH)SelectObject( hdc, hbr );
		::PatBlt( hdc, rc.left, rc.top, rc.right, rc.bottom, PATCOPY );
		SelectObject( hdc, hbrOld );
		return (LRESULT)1;
	}
	case WM_DESTROY:
		// 後始末
		KillTimer( hWnd, 1 );
		SetWindowLongPtr( hWnd, GWLP_WNDPROC, (LONG_PTR)pUrlWnd->m_pOldProc );
		if( pUrlWnd->m_hFont != nullptr )
			DeleteObject( pUrlWnd->m_hFont );
		pUrlWnd->m_hWnd = nullptr;
		pUrlWnd->m_hFont = nullptr;
		pUrlWnd->m_bHilighted = FALSE;
		pUrlWnd->m_pOldProc = nullptr;
		return (LRESULT)0;
	case WM_SETTEXT:
		return pUrlWnd->OnSetText( (LPCWSTR)lParam ) ? TRUE : FALSE;
	default:
		break;
	}

	return CallWindowProc( pUrlWnd->m_pOldProc, hWnd, msg, wParam, lParam );
}
//@@@ 2002.01.18 add end

//WM_SETTEXTハンドラ
//https://docs.microsoft.com/en-us/windows/desktop/winmsg/wm-settext
bool CUrlWnd::OnSetText( _In_opt_z_ LPCWSTR pchText, _In_opt_ size_t cchText ) const
{
	// 標準のメッセージハンドラに処理させる
	auto retSetText = ::CallWindowProc( m_pOldProc, GetHwnd(), WM_SETTEXT, 0, (LPARAM)pchText );
	if ( retSetText == FALSE ) {
		return false;
	}

	// サイズを調整のためにDCを取得
	HDC hDC = ::GetDC( GetHwnd() );
	auto hObj = ::SelectObject( hDC, GetFont() );

	// DrawText関数を使ってサイズを計測する
	// ※この処理は実際には描かない
	CMyRect rcText;
	int retDrawText = ::DrawText( hDC, pchText, static_cast<int>(cchText), &rcText, DT_CALCRECT );

	// DCの後始末
	::SelectObject( hDC, hObj );
	::ReleaseDC( GetHwnd(), hDC );

	// サイズを取得できなければ処理失敗とする
	if ( retDrawText == 0 ) {
		return false;
	}

	// マージン用にシステム設定値を取得する。
	// ※ユーザーが変えられる値なので毎回取りに行く（EDGE = 2px on 96dpi）
	const int cxEdge = ::GetSystemMetrics( SM_CXEDGE );
	const int cyEdge = ::GetSystemMetrics( SM_CYEDGE );

	// 計測結果のRECT構造体をSIZE構造体に読み替え、マージンを付加する
	SIZE size;
	size.cx = cxEdge + rcText.Width() + cxEdge;
	size.cy = cyEdge + rcText.Height() + cyEdge;

	// マージン込みのサイズをウインドウに反映する
	auto retSetPos = ::SetWindowPos( GetHwnd(), nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER );

	return retSetPos != FALSE;
}
