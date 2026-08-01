/*!	@file
	@brief 拡張（Open VSX）の検索と導入を行うサイドバー

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionPane.h"

#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include "apiwrap/DarkMode.h"
#include "config/system_constants.h"
#include "util/MessageBoxF.h"
#include "util/string_ex.h"
#include "util/window.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

namespace {

//! 子コントロールの識別子。親の中でだけ一意であればよい
enum EChildId {
	ID_SEARCH_EDIT		= 1001,
	ID_SEARCH_BUTTON	= 1002,
	ID_LIST				= 1003,
	ID_INSTALL_BUTTON	= 1004,
	ID_REMOVE_BUTTON	= 1005,
	ID_STATUS			= 1006,
	ID_SECTION_LABEL	= 1007,
};

//! 一覧の列
enum EColumn {
	COL_NAME	= 0,
	COL_VERSION	= 1,
	COL_STATE	= 2,
	COL_COUNT,
};

// 配置に使う寸法（DPI 拡大前の論理ピクセル）
constexpr int kMargin			= 4;
constexpr int kSectionHeight = 20;
constexpr int kExtensionRowHeight = 78;
constexpr int kLineHeight		= 22;
constexpr int kSearchButtonWidth = 52;
constexpr int kStatusHeight		= 40;

constexpr COLORREF kDarkCardColor = RGB( 37, 37, 38 );
constexpr COLORREF kDarkCardHoverColor = RGB( 50, 50, 50 );
constexpr COLORREF kDarkCardSelectedColor = RGB( 0, 122, 204 );
constexpr COLORREF kDarkTextColor = RGB( 224, 224, 224 );
constexpr COLORREF kDarkSecondaryTextColor = RGB( 170, 170, 170 );

//! 検索欄のサブクラス識別子
constexpr UINT_PTR kSearchEditSubclassId = 1;

/*!
	@brief 検索欄で Enter を押せるようにするためのサブクラス

	このペインの親（CEditWnd）はダイアログではないので、Enter や Tab は
	誰も面倒を見てくれない。少なくとも「入力して Enter で検索」は
	成り立たせたいので、検索欄だけ横取りする。
 */
LRESULT CALLBACK SearchEditProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/ )
{
	switch( msg ){
	case WM_KEYDOWN:
		if( wp == VK_RETURN ){
			::SendMessage( ::GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( ID_SEARCH_BUTTON, BN_CLICKED ), (LPARAM)hwnd );
			return 0;
		}
		break;
	case WM_CHAR:
		// WM_KEYDOWN で処理済み。ここで止めないと警告音が鳴る
		if( wp == VK_RETURN ){
			return 0;
		}
		break;
	case WM_NCDESTROY:
		::RemoveWindowSubclass( hwnd, SearchEditProc, kSearchEditSubclassId );
		break;
	default:
		break;
	}
	return ::DefSubclassProc( hwnd, msg, wp, lp );
}

//! 編集欄の内容を取り出す
std::wstring GetWindowTextAsString( HWND hwnd )
{
	const int nLength = ::GetWindowTextLength( hwnd );
	if( nLength <= 0 ){
		return std::wstring();
	}
	std::wstring sText( static_cast<size_t>(nLength) + 1, L'\0' );
	const int nCopied = ::GetWindowText( hwnd, sText.data(), nLength + 1 );
	sText.resize( nCopied < 0 ? 0 : static_cast<size_t>(nCopied) );
	return sText;
}

//! "namespace.name" を分解して SOpenVsxExtension に詰める
/*!
	名前空間に '.' は含まれないので、最初の '.' で分ければよい。
	導入済み一覧から作る行は導入操作の情報を持たないため、
	sDownloadUrl は空のままにしておく（それが「導入できない」の印になる）。
 */
SOpenVsxExtension MakeExtensionFromUniqueId( const std::wstring& sUniqueId, const std::wstring& sDisplayName, const std::wstring& sVersion )
{
	SOpenVsxExtension ext;
	const auto nDot = sUniqueId.find( L'.' );
	if( nDot == std::wstring::npos ){
		ext.sName = sUniqueId;
	}
	else{
		ext.sNamespace = sUniqueId.substr( 0, nDot );
		ext.sName = sUniqueId.substr( nDot + 1 );
	}
	ext.sDisplayName = sDisplayName;
	ext.sVersion = sVersion;
	return ext;
}

//! A job owns the atomic flag, so this adapter stays valid for every request in its worker.
class AtomicJobCancellation final : public platform::request::IRequestCancellation {
public:
	explicit AtomicJobCancellation( const std::atomic<bool>& cancelled ) noexcept
		: m_cancelled( cancelled )
	{
	}

	bool IsCancellationRequested() const noexcept override
	{
		return m_cancelled.load( std::memory_order_acquire );
	}

private:
	const std::atomic<bool>& m_cancelled;
};

//! Do not surface endpoint URLs, profile ids, proxy settings, or raw transport diagnostics in the UI.
std::wstring SafeOpenVsxStatusMessage( extension::openvsx::EOpenVsxRequestOutcome outcome )
{
	using extension::openvsx::EOpenVsxRequestOutcome;
	switch( outcome ){
	case EOpenVsxRequestOutcome::Cancelled:
		return L"extension operation cancelled";
	case EOpenVsxRequestOutcome::Timeout:
		return L"extension registry request timed out";
	case EOpenVsxRequestOutcome::ServerAuthenticationRequired:
	case EOpenVsxRequestOutcome::ProxyAuthenticationRequired:
		return L"extension registry authentication is required";
	case EOpenVsxRequestOutcome::TlsCertificateFailure:
		return L"extension registry TLS validation failed";
	case EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded:
	case EOpenVsxRequestOutcome::ResponseBodyLimitExceeded:
		return L"extension registry response exceeded a safety limit";
	case EOpenVsxRequestOutcome::OfflineCacheMiss:
		return L"extension registry is unavailable offline";
	case EOpenVsxRequestOutcome::UnsupportedProxyPolicy:
		return L"extension registry proxy policy is unsupported";
	case EOpenVsxRequestOutcome::InvalidRegistryUri:
	case EOpenVsxRequestOutcome::InvalidEndpointUri:
	case EOpenVsxRequestOutcome::InvalidRequest:
	case EOpenVsxRequestOutcome::InvalidRedirect:
	case EOpenVsxRequestOutcome::HttpsDowngradeRejected:
	case EOpenVsxRequestOutcome::RedirectLimitExceeded:
		return L"extension registry configuration is invalid";
	case EOpenVsxRequestOutcome::HttpStatusFailure:
	case EOpenVsxRequestOutcome::TransportFailure:
	case EOpenVsxRequestOutcome::InvalidResponse:
	case EOpenVsxRequestOutcome::SearchParseFailure:
	case EOpenVsxRequestOutcome::Unsupported:
	default:
		return L"extension registry operation failed";
	}
}

std::wstring SafeOpenVsxClientCreationMessage(
	extension::openvsx::EOpenVsxProductionClientOutcome outcome
)
{
	using extension::openvsx::EOpenVsxProductionClientOutcome;
	switch( outcome ){
	case EOpenVsxProductionClientOutcome::InvalidProfileId:
		return L"extension registry profile is unavailable";
	case EOpenVsxProductionClientOutcome::ConfigurationUnavailable:
		return L"extension registry network configuration is unavailable";
	case EOpenVsxProductionClientOutcome::ConfigurationInvalid:
		return L"extension registry network configuration is invalid";
	case EOpenVsxProductionClientOutcome::InternalFailure:
	default:
		return L"extension registry client initialization failed";
	}
}

bool RefreshExtensionHostInventory(HWND controlProcessWindow) noexcept
{
	DWORD_PTR refreshed = 0;
	return controlProcessWindow
		&& ::IsWindow(controlProcessWindow)
		&& ::SendMessageTimeoutW(
			controlProcessWindow,
			MYWM_EXTENSION_HOST_REFRESH_INVENTORY,
			0,
			0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK,
			2000,
			&refreshed) != 0
		&& refreshed != 0;
}

} // namespace

CExtensionPane::CExtensionPane(
	config::IConfigurationService& configurationService,
	std::wstring userDataProfileId,
	HWND controlProcessWindow
)
	: CWnd( L"::CExtensionPane" )
	, m_configurationService( configurationService )
	, m_userDataProfileId( std::move( userDataProfileId ) )
	, m_controlProcessWindow( controlProcessWindow )
{
}

void CExtensionPane::SetOnExtensionInstalled( std::function<void()> callback )
{
	m_onExtensionInstalled = std::move( callback );
}

void CExtensionPane::SetOnExtensionSelected( ExtensionSelectedCallback callback )
{
	m_onExtensionSelected = std::move( callback );
	NotifySelectionChanged();
}

void CExtensionPane::SetOnExtensionReadme( ExtensionReadmeCallback callback )
{
	m_onExtensionReadme = std::move( callback );
	if( m_onExtensionReadme ){
		NotifySelectionChanged();
	}
}

void CExtensionPane::InstallSelectedExtension()
{
	StartInstall();
}

void CExtensionPane::ClearExtensionSelection()
{
	if( m_hwndList ){
		LVITEM item = {};
		item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
		::SendMessage( m_hwndList, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), (LPARAM)&item );
	}
	SetStatusText( L"" );
	NotifySelectionChanged();
}

CExtensionPane::~CExtensionPane()
{
	// 実行中のワーカーが居るかもしれないので、結果の宛先を無効にしておく。
	// CWnd::~CWnd がウィンドウを壊す前に立てる必要がある。
	if( m_pJob ){
		m_pJob->bAbandoned.store( true, std::memory_order_release );
		m_pJob->bCancelled.store( true, std::memory_order_release );
		m_pJob.reset();
	}
	::KillTimer( GetHwnd(), kJobPollTimerId );
	if( m_hFont ){
		::DeleteObject( m_hFont );
		m_hFont = nullptr;
	}
	CancelReadmeJob();
	m_onExtensionSelected = nullptr;
	m_onExtensionInstalled = nullptr;
}

HWND CExtensionPane::Open( HINSTANCE hInstance, HWND hwndParent )
{
	if( GetHwnd() ){
		return GetHwnd();
	}

	LPCWSTR pszClassName = L"CExtensionPane";
	RegisterWC(
		hInstance,
		nullptr,
		nullptr,
		::LoadCursor( nullptr, IDC_ARROW ),
		(HBRUSH)( COLOR_BTNFACE + 1 ),
		nullptr,
		pszClassName
	);

	HWND hwnd = CWnd::Create(
		hwndParent,
		0,
		pszClassName,
		pszClassName,
		WS_CHILD | WS_CLIPCHILDREN,
		0, 0, DpiScaleX( kDefaultWidth ), 100,
		nullptr
	);
	if( !hwnd ){
		return nullptr;
	}

	if( !CreateChildren() ){
		DestroyWindow();
		return nullptr;
	}

	// ダークモードへの追従。親側（背景・文字色）と子側（テーマ）の両方が必要
	DarkMode::setWindowEraseBgSubclass( hwnd );
	DarkMode::setWindowCtlColorSubclass( hwnd );
	DarkMode::setChildCtrlsSubclassAndTheme( hwnd );

	ShowInstalledList();
	SetStatusText( LS( STR_EXTENSION_NOTE_NOTRUN ) );

	return hwnd;
}

bool CExtensionPane::CreateChildren()
{
	HWND hwnd = GetHwnd();
	HINSTANCE hInstance = GetAppInstance();

	// 表示用フォント。利用者が OS に設定した UI フォントに従う
	NONCLIENTMETRICS met = {};
	met.cbSize = CCSIZEOF_STRUCT( NONCLIENTMETRICS, lfMessageFont );
	if( ::SystemParametersInfo( SPI_GETNONCLIENTMETRICS, met.cbSize, &met, 0 ) ){
		m_hFont = ::CreateFontIndirect( &met.lfMessageFont );
	}

	m_hwndSearchEdit = ::CreateWindowEx(
		WS_EX_CLIENTEDGE, WC_EDIT, nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		0, 0, 0, 0, hwnd, (HMENU)ID_SEARCH_EDIT, hInstance, nullptr );

	m_hwndSearchButton = ::CreateWindowEx(
		0, WC_BUTTON, LS( STR_EXTENSION_SEARCH ),
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, hwnd, (HMENU)ID_SEARCH_BUTTON, hInstance, nullptr );
	m_hwndSectionLabel = ::CreateWindowEx(
		0, WC_STATIC, L"Installed", WS_CHILD | WS_VISIBLE | SS_LEFT,
		0, 0, 0, 0, hwnd, (HMENU)ID_SECTION_LABEL, hInstance, nullptr );

	m_hwndList = ::CreateWindowEx(
		WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDRAWFIXED | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
		0, 0, 0, 0, hwnd, (HMENU)ID_LIST, hInstance, nullptr );

	m_hwndInstallButton = ::CreateWindowEx(
		0, WC_BUTTON, LS( STR_EXTENSION_INSTALL ),
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, hwnd, (HMENU)ID_INSTALL_BUTTON, hInstance, nullptr );

	m_hwndRemoveButton = ::CreateWindowEx(
		0, WC_BUTTON, LS( STR_EXTENSION_REMOVE ),
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, hwnd, (HMENU)ID_REMOVE_BUTTON, hInstance, nullptr );

	m_hwndStatus = ::CreateWindowEx(
		0, WC_STATIC, nullptr,
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, hInstance, nullptr );

	if( !m_hwndSearchEdit || !m_hwndSearchButton || !m_hwndSectionLabel || !m_hwndList
		|| !m_hwndInstallButton || !m_hwndRemoveButton || !m_hwndStatus ){
		return false;
	}

	if( m_hFont ){
		for( HWND hwndChild : { m_hwndSearchEdit, m_hwndSearchButton, m_hwndList,
			m_hwndInstallButton, m_hwndRemoveButton, m_hwndStatus, m_hwndSectionLabel } ){
			::SendMessage( hwndChild, WM_SETFONT, (WPARAM)m_hFont, MAKELPARAM( TRUE, 0 ) );
		}
	}

	::SendMessage( m_hwndList, LVM_SETEXTENDEDLISTVIEWSTYLE,
		LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_TRACKSELECT,
		LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_TRACKSELECT );
	::SendMessage( m_hwndSearchEdit, EM_SETCUEBANNER, TRUE,
		(LPARAM)L"Search Extensions in Marketplace" );

	// 列を作る。幅は LayoutChildren で決めるのでここでは仮の値
	static const UINT auColumnLabels[COL_COUNT] = {
		STR_EXTENSION_COL_NAME, STR_EXTENSION_COL_VERSION, STR_EXTENSION_COL_STATE
	};
	for( int i = 0; i < COL_COUNT; ++i ){
		LVCOLUMN col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		col.iSubItem = i;
		col.cx = DpiScaleX( 60 );
		col.pszText = const_cast<LPWSTR>( LS( auColumnLabels[i] ) );
		::SendMessage( m_hwndList, LVM_INSERTCOLUMN, i, (LPARAM)&col );
	}
	// The Marketplace view is a stack of extension cards, not a table. Keep the
	// legacy columns as the list's data model for keyboard/accessibility support,
	// but hide their report header so the native projection matches VS Code's
	// card presentation.
	if (const HWND header = reinterpret_cast<HWND>(::SendMessage(m_hwndList, LVM_GETHEADER, 0, 0));
		header != nullptr) {
		::ShowWindow(header, SW_HIDE);
	}

	::SetWindowSubclass( m_hwndSearchEdit, SearchEditProc, kSearchEditSubclassId, 0 );

	return true;
}

int CExtensionPane::GetDockWidth() const
{
	if( !GetHwnd() || !::IsWindowVisible( GetHwnd() ) ){
		return 0;
	}
	return DpiScaleX( kDefaultWidth );
}

void CExtensionPane::SetFocusToSearchBox()
{
	if( m_hwndSearchEdit ){
		::SetFocus( m_hwndSearchEdit );
		::SendMessage( m_hwndSearchEdit, EM_SETSEL, 0, -1 );
	}
}

LRESULT CExtensionPane::OnSize( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	LayoutChildren( LOWORD( lp ), HIWORD( lp ) );
	return 0;
}

void CExtensionPane::LayoutChildren( int cx, int cy )
{
	if( cx <= 0 || cy <= 0 || !m_hwndList || !m_hwndSearchEdit ){
		return;
	}

	const int nMargin	= DpiScaleX( kMargin );
	const int nLine		= DpiScaleY( kLineHeight );
	const int nButtonW	= DpiScaleX( kSearchButtonWidth );
	const int nStatusH	= DpiScaleY( kStatusHeight );
	const int nSectionH = DpiScaleY( kSectionHeight );

	// 上段：検索欄と検索ボタン
	int nEditWidth = cx - nMargin * 3 - nButtonW;
	if( nEditWidth < nMargin ){
		nEditWidth = nMargin;
	}
	::MoveWindow( m_hwndSearchEdit, nMargin, nMargin, nEditWidth, nLine, TRUE );
	::MoveWindow( m_hwndSearchButton, nMargin * 2 + nEditWidth, nMargin, nButtonW, nLine, TRUE );

	// 下段：操作ボタンと状態表示
	const int nStatusTop = std::max( nMargin, cy - nMargin - nStatusH );
	const int nButtonTop = std::max( nMargin, nStatusTop - nMargin - nLine );
	const int nActionW = ( cx - nMargin * 3 ) / 2;
	::MoveWindow( m_hwndInstallButton, nMargin, nButtonTop, nActionW, nLine, TRUE );
	::MoveWindow( m_hwndRemoveButton, nMargin * 2 + nActionW, nButtonTop, nActionW, nLine, TRUE );
	::MoveWindow( m_hwndStatus, nMargin, nStatusTop, cx - nMargin * 2, nStatusH, TRUE );

	// 中段：一覧。残りをすべて使う
	::MoveWindow( m_hwndSectionLabel, nMargin, nMargin * 2 + nLine,
		cx - nMargin * 2, nSectionH, TRUE );
	const int nListTop = nMargin * 2 + nLine + nSectionH;
	int nListHeight = nButtonTop - nMargin - nListTop;
	if( nListHeight < nLine ){
		nListHeight = nLine;
	}
	const int nListWidth = std::max( nMargin, cx - nMargin * 2 );
	::MoveWindow( m_hwndList, nMargin, nListTop, nListWidth, nListHeight, TRUE );

	// 列幅。縦スクロールバーの分を残して名前欄を可変にする
	const int nScrollBar = ::GetSystemMetrics( SM_CXVSCROLL );
	const int nVersionW = DpiScaleX( 60 );
	const int nStateW = DpiScaleX( 64 );
	int nNameW = nListWidth - nVersionW - nStateW - nScrollBar - DpiScaleX( 4 );
	if( nNameW < DpiScaleX( 40 ) ){
		nNameW = DpiScaleX( 40 );
	}
	::SendMessage( m_hwndList, LVM_SETCOLUMNWIDTH, COL_NAME, nNameW );
	::SendMessage( m_hwndList, LVM_SETCOLUMNWIDTH, COL_VERSION, nVersionW );
	::SendMessage( m_hwndList, LVM_SETCOLUMNWIDTH, COL_STATE, nStateW );
	::InvalidateRect( m_hwndList, nullptr, TRUE );
}

LRESULT CExtensionPane::OnCommand( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	switch( LOWORD( wp ) ){
	case ID_SEARCH_BUTTON:
		StartSearch();
		return 0;
	case ID_INSTALL_BUTTON:
		StartInstall();
		return 0;
	case ID_REMOVE_BUTTON:
		StartUninstall();
		return 0;
	default:
		break;
	}
	return CallDefWndProc( hwnd, msg, wp, lp );
}

LRESULT CExtensionPane::OnNotify( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	const NMHDR* pNmhdr = (const NMHDR*)lp;
	if( pNmhdr && pNmhdr->hwndFrom == m_hwndList ){
		switch( pNmhdr->code ){
		case LVN_ITEMCHANGED:
			{
				const NMLISTVIEW* pNmlv = (const NMLISTVIEW*)lp;
				if( ( pNmlv->uChanged & LVIF_STATE ) != 0 ){
					UpdateButtons();
					// 選択されたものの説明を状態欄に出す。狭い場所を有効に使う
					const int nRow = GetSelectedRow();
					if( 0 <= nRow ){
						const std::wstring& sDescription = m_rows[nRow].ext.sDescription;
						SetStatusText( sDescription.empty() ? m_rows[nRow].ext.GetUniqueId() : sDescription );
					}
					else{
						SetStatusText( L"" );
					}
					NotifySelectionChanged();
				}
			}
			return 0;
		case LVN_ITEMACTIVATE:
			// ダブルクリックで導入
			StartInstall();
			return 0;
		default:
			break;
		}
	}
	return CallDefWndProc( hwnd, msg, wp, lp );
}

LRESULT CExtensionPane::OnMeasureItem( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if( lp ){
		auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>( lp );
		if( measure->CtlID == ID_LIST ){
			measure->itemHeight = DpiScaleY( kExtensionRowHeight );
			return TRUE;
		}
	}
	return CallDefWndProc( hwnd, msg, wp, lp );
}

LRESULT CExtensionPane::OnDrawItem( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if( !lp ) return CallDefWndProc( hwnd, msg, wp, lp );
	auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>( lp );
	if( draw->CtlID != ID_LIST || draw->itemID >= m_rows.size() ) return TRUE;
	const auto& row = m_rows[draw->itemID];
	const bool selected = ( draw->itemState & ODS_SELECTED ) != 0;
	const bool focused = ( draw->itemState & ODS_FOCUS ) != 0;
	const bool hovered = ( draw->itemState & ODS_HOTLIGHT ) != 0;
	const bool dark = IsDarkModeActive();
	const COLORREF background = dark
		? ( selected ? kDarkCardSelectedColor : hovered ? kDarkCardHoverColor : kDarkCardColor )
		: ( selected ? ::GetSysColor( COLOR_HIGHLIGHT ) : hovered ? RGB( 232, 240, 254 ) : ::GetSysColor( COLOR_WINDOW ) );
	const COLORREF foreground = dark
		? ( selected ? RGB( 255, 255, 255 ) : kDarkTextColor )
		: ( selected ? ::GetSysColor( COLOR_HIGHLIGHTTEXT ) : ::GetSysColor( COLOR_WINDOWTEXT ) );
	const COLORREF secondary = dark ? kDarkSecondaryTextColor : ::GetSysColor( COLOR_GRAYTEXT );
	HBRUSH brush = ::CreateSolidBrush( background );
	::FillRect( draw->hDC, &draw->rcItem, brush );
	::DeleteObject( brush );
	::SetBkMode( draw->hDC, TRANSPARENT );
	::SetTextColor( draw->hDC, foreground );
	RECT text = draw->rcItem;
	text.left += DpiScaleX( 8 ); text.right -= DpiScaleX( 8 );
	const std::wstring name = row.ext.sDisplayName.empty() ? row.ext.sName : row.ext.sDisplayName;
	::DrawText( draw->hDC, name.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
	text.top += DpiScaleY( 18 );
	const std::wstring id = row.ext.GetUniqueId();
	::SetTextColor( draw->hDC, secondary );
	::DrawText( draw->hDC, id.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
	text.top += DpiScaleY( 16 );
	std::wstring details = row.sInstalledVersion.empty() ? L"Available  " : L"Installed  ";
	details += row.sInstalledVersion.empty() ? row.ext.sVersion : row.sInstalledVersion;
	::DrawText( draw->hDC, details.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
	text.top += DpiScaleY( 16 );
	details = row.ext.sDescription;
	if( row.ext.nDownloadCount > 0 ) details += L"  " + std::to_wstring( row.ext.nDownloadCount ) + L" downloads";
	if( row.ext.HasRating() ) details += L"  " + std::to_wstring( row.ext.dAverageRating ) + L"/5";
	if( row.ext.bVerified ) details += L"  Verified";
	::DrawText( draw->hDC, details.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
	if( focused ) ::DrawFocusRect( draw->hDC, &draw->rcItem );
	return TRUE;
}

LRESULT CExtensionPane::OnDestroy( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	// 共有を解放する前に取消しを公開する。ワーカーはネットワーク・展開・削除の境界で
	// これを確認して副作用を止めるため、UI スレッドは完了を待たずに破棄できる。
	if( m_pJob ){
		m_pJob->bAbandoned.store( true, std::memory_order_release );
		m_pJob->bCancelled.store( true, std::memory_order_release );
		m_pJob.reset();
	}
	// README runs on the same detached-worker boundary and must not publish after
	// the pane HWND has begun destruction.
	CancelReadmeJob();
	::KillTimer( hwnd, kJobPollTimerId );
	return CallDefWndProc( hwnd, msg, wp, lp );
}

LRESULT CExtensionPane::OnTimer( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if( wp == kJobPollTimerId ){
		// PostMessage が失敗しても、完了済みジョブをここで必ず終端状態へ遷移させる。
		if( m_pJob && m_pJob->bDone.load( std::memory_order_acquire ) ){
			FinishJob( m_pJob->nSerial );
		}
		if( m_pReadmeJob && m_pReadmeJob->bDone.load( std::memory_order_acquire ) ){
			FinishJob( m_pReadmeJob->nSerial );
		}
		if( !m_pJob && !m_pReadmeJob ){
			::KillTimer( hwnd, kJobPollTimerId );
		}
		return 0;
	}
	return CallDefWndProc( hwnd, msg, wp, lp );
}

LRESULT CExtensionPane::DispatchEvent_WM_APP( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if( msg == kJobDoneMessage ){
		FinishJob( static_cast<int>(wp) );
		return 0;
	}
	return CWnd::DispatchEvent_WM_APP( hwnd, msg, wp, lp );
}

void CExtensionPane::ShowInstalledList()
{
	m_bSearchResultShown = false;
	m_rows.clear();
	for( const auto& installed : m_cManager.EnumInstalled() ){
		SRow row;
		row.ext = MakeExtensionFromUniqueId( installed.sUniqueId, installed.sDisplayName, installed.sVersion );
		row.sInstalledVersion = installed.sVersion;
		m_rows.push_back( std::move( row ) );
	}
	UpdateListView();
}

void CExtensionPane::RefreshInstalledState()
{
	for( auto& row : m_rows ){
		SInstalledExtension found;
		row.sInstalledVersion = m_cManager.FindInstalled( row.ext.GetUniqueId(), found )
			? found.sVersion
			: std::wstring();
	}
}

void CExtensionPane::UpdateListView()
{
	if( !m_hwndList ){
		return;
	}

	::SendMessage( m_hwndList, WM_SETREDRAW, FALSE, 0 );
	const int selectedRow = GetSelectedRow();
	const std::wstring selectedId = selectedRow >= 0 && static_cast<size_t>( selectedRow ) < m_rows.size()
		? m_rows[static_cast<size_t>(selectedRow)].ext.GetUniqueId() : std::wstring();
	// Do not notify while the old rows still exist.  The public selection callback
	// must observe either a current row or an explicit cleared selection.
	LVITEM clearSelection = {};
	clearSelection.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
	::SendMessage( m_hwndList, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), (LPARAM)&clearSelection );
	::SendMessage( m_hwndList, LVM_DELETEALLITEMS, 0, 0 );
	if( m_hwndSectionLabel ){
		::SetWindowText( m_hwndSectionLabel,
			m_bSearchResultShown ? L"Popular / Search results" : L"Installed" );
	}

	for( size_t i = 0; i < m_rows.size(); ++i ){
		const SRow& row = m_rows[i];
		const std::wstring& sName = row.ext.sDisplayName.empty() ? row.ext.sName : row.ext.sDisplayName;

		LVITEM item = {};
		item.mask = LVIF_TEXT;
		item.iItem = static_cast<int>(i);
		item.iSubItem = COL_NAME;
		item.pszText = const_cast<LPWSTR>( sName.c_str() );
		const int nInserted = static_cast<int>( ::SendMessage( m_hwndList, LVM_INSERTITEM, 0, (LPARAM)&item ) );
		if( nInserted < 0 ){
			continue;
		}

		const std::wstring& sVersion = row.sInstalledVersion.empty() ? row.ext.sVersion : row.sInstalledVersion;
		LVITEM sub = {};
		sub.mask = LVIF_TEXT;
		sub.iItem = nInserted;
		sub.iSubItem = COL_VERSION;
		sub.pszText = const_cast<LPWSTR>( sVersion.c_str() );
		::SendMessage( m_hwndList, LVM_SETITEM, 0, (LPARAM)&sub );

		sub.iSubItem = COL_STATE;
		sub.pszText = const_cast<LPWSTR>( LS( row.sInstalledVersion.empty()
			? STR_EXTENSION_STATE_NOTINSTALLED : STR_EXTENSION_STATE_INSTALLED ) );
		::SendMessage( m_hwndList, LVM_SETITEM, 0, (LPARAM)&sub );
	}
	if( !selectedId.empty() ){
		for( size_t i = 0; i < m_rows.size(); ++i ){
			if( m_rows[i].ext.GetUniqueId() == selectedId ){
				LVITEM state = {};
				state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
				state.state = LVIS_SELECTED | LVIS_FOCUSED;
				::SendMessage( m_hwndList, LVM_SETITEMSTATE, static_cast<WPARAM>(i), (LPARAM)&state );
				break;
			}
		}
	}

	::SendMessage( m_hwndList, WM_SETREDRAW, TRUE, 0 );
	::InvalidateRect( m_hwndList, nullptr, TRUE );

	UpdateButtons();
	NotifySelectionChanged();
}

void CExtensionPane::UpdateButtons()
{
	const bool bBusy = ( m_pJob != nullptr );
	const int nRow = GetSelectedRow();

	const bool bCanInstall = !bBusy && 0 <= nRow
		&& !m_rows[nRow].ext.sDownloadUrl.empty()
		&& m_rows[nRow].sInstalledVersion.empty();
	const bool bCanRemove = !bBusy && 0 <= nRow && !m_rows[nRow].sInstalledVersion.empty();

	::EnableWindow( m_hwndSearchButton, !bBusy );
	::EnableWindow( m_hwndInstallButton, bCanInstall );
	::EnableWindow( m_hwndRemoveButton, bCanRemove );
}

void CExtensionPane::SetStatusText( const std::wstring& sText )
{
	if( m_hwndStatus ){
		::SetWindowText( m_hwndStatus, sText.c_str() );
	}
}

int CExtensionPane::GetSelectedRow() const
{
	if( !m_hwndList ){
		return -1;
	}
	const int nIndex = static_cast<int>( ::SendMessage( m_hwndList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED ) );
	if( nIndex < 0 || m_rows.size() <= static_cast<size_t>(nIndex) ){
		return -1;
	}
	return nIndex;
}

void CExtensionPane::NotifySelectionChanged()
{
	const int nRow = GetSelectedRow();
	if( nRow < 0 || static_cast<size_t>(nRow) >= m_rows.size() ){
		// An empty model is the typed clear signal for the composition root. This
		// prevents stale details from surviving a refresh, uninstall, or close gesture.
		CancelReadmeJob();
		m_sReadmeCacheKey.clear();
		if( m_onExtensionSelected ) m_onExtensionSelected( SOpenVsxExtension{} );
		if( m_onExtensionReadme ) NotifyReadme( SOpenVsxExtension{}, EExtensionReadmeState::Unsupported, L"", L"" );
		return;
	}
	const SOpenVsxExtension& extension = m_rows[static_cast<size_t>(nRow)].ext;
	if( m_onExtensionSelected ) m_onExtensionSelected( extension );
	StartReadmeJob( extension );
}

void CExtensionPane::NotifyReadme(
	const SOpenVsxExtension& extension,
	EExtensionReadmeState state,
	const std::wstring& payload,
	const std::wstring& error )
{
	if( m_onExtensionReadme ) m_onExtensionReadme( extension, state, payload, error );
}

void CExtensionPane::CancelReadmeJob()
{
	if( m_pReadmeJob ){
		m_pReadmeJob->bAbandoned.store( true, std::memory_order_release );
		m_pReadmeJob->bCancelled.store( true, std::memory_order_release );
		m_pReadmeJob.reset();
	}
}

void CExtensionPane::StartReadmeJob( const SOpenVsxExtension& extension )
{
	CancelReadmeJob();
	const std::wstring key = extension.GetUniqueId() + L"\n" + extension.sVersion + L"\n" + extension.sReadmeUrl;
	if( key == m_sReadmeCacheKey ){
		NotifyReadme( extension, m_eReadmeCacheState, m_sReadmeCachePayload, m_sReadmeCacheError );
		return;
	}
	m_sReadmeCacheKey.clear();
	NotifyReadme( extension, EExtensionReadmeState::Loading, L"", L"" );
	if( extension.sReadmeUrl.empty() ){
		NotifyReadme( extension, EExtensionReadmeState::Unsupported, L"", L"" );
		return;
	}
	auto clientResult = extension::openvsx::CreateOpenVsxProductionClient(
		m_configurationService, m_userDataProfileId );
	if( !clientResult ){
		NotifyReadme( extension, EExtensionReadmeState::Error, L"", SafeOpenVsxClientCreationMessage( clientResult.outcome ) );
		return;
	}
	auto pJob = std::make_shared<SJob>();
	pJob->eKind = EJobKind::Readme;
	pJob->ext = extension;
	pJob->sReadmeUrl = extension.sReadmeUrl;
	pJob->registryClient = std::move( clientResult.client );
	pJob->nSerial = AllocateJobSerial();
	m_pReadmeJob = pJob;
	if( 0 == ::SetTimer( GetHwnd(), kJobPollTimerId, kJobPollIntervalMs, nullptr ) ){
		pJob->bCancelled.store( true, std::memory_order_release );
		m_pReadmeJob.reset();
		NotifyReadme( extension, EExtensionReadmeState::Error, L"", L"cannot start extension detail timer" );
		return;
	}
	try {
		std::thread( &CExtensionPane::RunJob, pJob, GetHwnd() ).detach();
	}
	catch( ... ){
		pJob->bCancelled.store( true, std::memory_order_release );
		m_pReadmeJob.reset();
		NotifyReadme( extension, EExtensionReadmeState::Error, L"", L"cannot start extension detail worker" );
	}
}

void CExtensionPane::StartSearch()
{
	if( m_pJob ){
		SetStatusText( LS( STR_EXTENSION_STATUS_BUSY ) );
		return;
	}

	const std::wstring sQuery = GetWindowTextAsString( m_hwndSearchEdit );
	if( sQuery.empty() ){
		// 検索語が無いときは導入済み一覧に戻す。通信しない
		ShowInstalledList();
		SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_INSTALLED_COUNT ), (int)m_rows.size() ) );
		return;
	}

	auto pJob = std::make_shared<SJob>();
	pJob->eKind = EJobKind::Search;
	pJob->sQuery = sQuery;
	SetStatusText( LS( STR_EXTENSION_STATUS_SEARCHING ) );
	StartJob( std::move( pJob ) );
}

void CExtensionPane::StartInstall()
{
	if( m_pJob ){
		SetStatusText( LS( STR_EXTENSION_STATUS_BUSY ) );
		return;
	}

	const int nRow = GetSelectedRow();
	if( nRow < 0 || m_rows[nRow].ext.sDownloadUrl.empty() || !m_rows[nRow].sInstalledVersion.empty() ){
		return;
	}

	auto pJob = std::make_shared<SJob>();
	pJob->eKind = EJobKind::Install;
	pJob->ext = m_rows[nRow].ext;
	pJob->sTargetName = pJob->ext.GetUniqueId();
	SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_INSTALLING ), pJob->sTargetName.c_str() ) );
	StartJob( std::move( pJob ) );
}

void CExtensionPane::StartUninstall()
{
	if( m_pJob ){
		SetStatusText( LS( STR_EXTENSION_STATUS_BUSY ) );
		return;
	}

	const int nRow = GetSelectedRow();
	if( nRow < 0 || m_rows[nRow].sInstalledVersion.empty() ){
		return;
	}

	const std::wstring sUniqueId = m_rows[nRow].ext.GetUniqueId();
	if( IDYES != ConfirmMessage( GetHwnd(), LS( STR_EXTENSION_CONFIRM_REMOVE ), sUniqueId.c_str() ) ){
		return;
	}

	auto pJob = std::make_shared<SJob>();
	pJob->eKind = EJobKind::Uninstall;
	pJob->sUniqueId = sUniqueId;
	pJob->sTargetName = sUniqueId;
	SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_REMOVING ), sUniqueId.c_str() ) );
	StartJob( std::move( pJob ) );
}

int CExtensionPane::AllocateJobSerial() noexcept
{
	// At most one job exists.  The next job is not launched before the current
	// job is terminal, so reuse after INT_MAX cannot collide with a live result.
	if( m_nNextSerial <= 0 || m_nNextSerial == (std::numeric_limits<int>::max)() ){
		m_nNextSerial = 1;
	}
	const int nSerial = m_nNextSerial;
	++m_nNextSerial;
	return nSerial;
}

void CExtensionPane::StartJob( std::shared_ptr<SJob> pJob )
{
	if( !pJob ){
		SetStatusText( L"cannot start extension job" );
		UpdateButtons();
		return;
	}

	// The configuration snapshot and all network-service construction happen on
	// the UI thread.  The self-contained client is then transferred to SJob;
	// detached work never touches CEditWnd, IWorkbenchRuntime, or configuration.
	if( pJob->eKind == EJobKind::Search || pJob->eKind == EJobKind::Install ){
		auto clientResult = extension::openvsx::CreateOpenVsxProductionClient(
			m_configurationService,
			m_userDataProfileId
		);
		if( !clientResult ){
			SetStatusText( strprintf(
				LS( STR_EXTENSION_STATUS_FAILED ),
				SafeOpenVsxClientCreationMessage( clientResult.outcome ).c_str()
			) );
			UpdateButtons();
			return;
		}
		pJob->registryClient = std::move( clientResult.client );
	}

	pJob->nSerial = AllocateJobSerial();
	m_pJob = pJob;
	UpdateButtons();
	if( 0 == ::SetTimer( GetHwnd(), kJobPollTimerId, kJobPollIntervalMs, nullptr ) ){
		pJob->bCancelled.store( true, std::memory_order_release );
		m_pJob.reset();
		SetStatusText( L"cannot start extension job completion timer" );
		UpdateButtons();
		return;
	}

	// detach する。共有するのは取消し可能な SJob だけで、this には触れさせない。
	try {
		std::thread( &CExtensionPane::RunJob, pJob, GetHwnd() ).detach();
	}
	catch( ... ) {
		// ワーカーを作れなかった場合も、タイマーと busy 状態を必ず終端にする。
		pJob->bCancelled.store( true, std::memory_order_release );
		::KillTimer( GetHwnd(), kJobPollTimerId );
		if( m_pJob && m_pJob->nSerial == pJob->nSerial ){
			m_pJob.reset();
		}
		SetStatusText( L"cannot start extension job worker" );
		UpdateButtons();
	}
}

/* static */ void CExtensionPane::RunJob( std::shared_ptr<SJob> pJob, HWND hwndNotify )
{
	try {
		switch( pJob->eKind ){
	case EJobKind::Search:
		{
			if( !pJob->registryClient ){
				pJob->sErrorMsg = L"extension registry client is unavailable";
				break;
			}
			const AtomicJobCancellation cancellation( pJob->bCancelled );
			auto operation = pJob->registryClient->Search(
				pJob->sQuery,
				0,
				extension::openvsx::OpenVsxRequestServiceAdapter::kDefaultPageSize,
				&cancellation
			);
			pJob->bSucceeded = static_cast<bool>( operation.status );
			if( pJob->bSucceeded ){
				pJob->result = std::move( operation.value );
			}
			else{
				pJob->sErrorMsg = SafeOpenVsxStatusMessage( operation.status.outcome );
			}
		}
		break;
	case EJobKind::Readme:
		{
			if( !pJob->registryClient ){
				pJob->sErrorMsg = L"extension registry client is unavailable";
				break;
			}
			const AtomicJobCancellation cancellation( pJob->bCancelled );
			auto operation = pJob->registryClient->FetchText( pJob->sReadmeUrl, &cancellation );
			pJob->bSucceeded = static_cast<bool>( operation.status );
			pJob->sReadmePayload = std::move( operation.value );
			pJob->sErrorMsg = operation.status.message;
			if( !pJob->bSucceeded && pJob->sErrorMsg.empty() ){
				pJob->sErrorMsg = SafeOpenVsxStatusMessage( operation.status.outcome );
			}
		}
		break;
	case EJobKind::Install:
		{
			if( !pJob->registryClient ){
				pJob->sErrorMsg = L"extension registry client is unavailable";
				break;
			}
			const AtomicJobCancellation cancellation( pJob->bCancelled );
			CExtensionManager cManager;
			pJob->bSucceeded = cManager.Install(
				pJob->ext,
				*pJob->registryClient,
				pJob->sErrorMsg,
				&cancellation,
				&pJob->bCancelled
			);
			if( !pJob->bSucceeded ){
				// CExtensionManager intentionally keeps detailed diagnostics for logs/tests.
				// This UI boundary must not display request URLs, proxy values, or secrets.
				pJob->sErrorMsg = pJob->bCancelled.load( std::memory_order_acquire )
					? L"extension operation cancelled"
					: L"extension installation failed";
			}
		}
		break;
	case EJobKind::Uninstall:
		{
			CExtensionManager cManager;
			pJob->bSucceeded = cManager.Uninstall( pJob->sUniqueId, pJob->sErrorMsg, &pJob->bCancelled );
		}
		break;
	default:
		pJob->sErrorMsg = L"unknown job";
		break;
		}
	}
	catch( const std::exception& ) {
		pJob->bSucceeded = false;
		pJob->sErrorMsg = L"extension job threw an exception";
	}
	catch( ... ) {
		pJob->bSucceeded = false;
		pJob->sErrorMsg = L"extension job threw an unknown exception";
	}

	// 結果を書き終えたことを公開する。UI スレッドはこれを見てから結果を読む
	pJob->bDone.store( true, std::memory_order_release );

	// 通常は即時通知する。失敗しても UI 側のポーリングタイマーが完了状態を回収する。
	if( !pJob->bAbandoned.load( std::memory_order_acquire ) ){
		::PostMessage( hwndNotify, kJobDoneMessage, static_cast<WPARAM>(pJob->nSerial), 0 );
	}
}

void CExtensionPane::FinishJob( int nSerial )
{
	if( m_pReadmeJob && m_pReadmeJob->nSerial == nSerial ){
		if( !m_pReadmeJob->bDone.load( std::memory_order_acquire ) ) return;
		const std::shared_ptr<SJob> pJob = std::move( m_pReadmeJob );
		const int nRow = GetSelectedRow();
		const bool current = nRow >= 0 && static_cast<size_t>(nRow) < m_rows.size()
			&& m_rows[static_cast<size_t>(nRow)].ext.GetUniqueId() == pJob->ext.GetUniqueId()
			&& m_rows[static_cast<size_t>(nRow)].ext.sVersion == pJob->ext.sVersion
			&& m_rows[static_cast<size_t>(nRow)].ext.sReadmeUrl == pJob->sReadmeUrl;
		if( current ){
			const EExtensionReadmeState state = pJob->bSucceeded ? EExtensionReadmeState::Ready : EExtensionReadmeState::Error;
			m_sReadmeCacheKey = pJob->ext.GetUniqueId() + L"\n" + pJob->ext.sVersion + L"\n" + pJob->sReadmeUrl;
			m_eReadmeCacheState = state;
			m_sReadmeCachePayload = pJob->bSucceeded ? pJob->sReadmePayload : L"";
			m_sReadmeCacheError = pJob->bSucceeded ? L"" : pJob->sErrorMsg;
			NotifyReadme( pJob->ext, state, m_sReadmeCachePayload, m_sReadmeCacheError );
		}
		if( !m_pJob ) ::KillTimer( GetHwnd(), kJobPollTimerId );
		return;
	}
	std::shared_ptr<SJob>* ppJob = nullptr;
	if( m_pJob && m_pJob->nSerial == nSerial ) ppJob = &m_pJob;
	else if( m_pReadmeJob && m_pReadmeJob->nSerial == nSerial ) ppJob = &m_pReadmeJob;
	if( !ppJob ){
		return;	// 世代違いの結果は捨てる
	}
	if( !(*ppJob)->bDone.load( std::memory_order_acquire ) ){
		return;	// 書き終えていないものは読まない
	}

	// 実行中でなくなったことを先に確定させる（move 後 m_pJob は空になる）
	const std::shared_ptr<SJob> pJob = std::move( *ppJob );
	if( !m_pJob && !m_pReadmeJob ) ::KillTimer( GetHwnd(), kJobPollTimerId );

	if( pJob->eKind == EJobKind::Readme ){
		if( pJob->bSucceeded ){
			m_sReadmeCacheKey = pJob->ext.GetUniqueId() + L"\n" + pJob->ext.sVersion + L"\n" + pJob->ext.sReadmeUrl;
			m_eReadmeCacheState = EExtensionReadmeState::Ready;
			m_sReadmeCachePayload = pJob->sReadmePayload;
			m_sReadmeCacheError.clear();
			NotifyReadme( pJob->ext, EExtensionReadmeState::Ready, pJob->sReadmePayload, L"" );
		}
		else if( !pJob->bCancelled.load( std::memory_order_acquire ) ){
			m_sReadmeCacheKey = pJob->ext.GetUniqueId() + L"\n" + pJob->ext.sVersion + L"\n" + pJob->ext.sReadmeUrl;
			m_eReadmeCacheState = EExtensionReadmeState::Error;
			m_sReadmeCachePayload.clear();
			m_sReadmeCacheError = pJob->sErrorMsg;
			NotifyReadme( pJob->ext, EExtensionReadmeState::Error, L"", pJob->sErrorMsg );
		}
		return;
	}

	if( !pJob->bSucceeded ){
		SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_FAILED ), pJob->sErrorMsg.c_str() ) );
		UpdateButtons();
		return;
	}

	switch( pJob->eKind ){
	case EJobKind::Search:
		m_bSearchResultShown = true;
		m_rows.clear();
		for( const auto& ext : pJob->result.extensions ){
			SRow row;
			row.ext = ext;
			m_rows.push_back( std::move( row ) );
		}
		RefreshInstalledState();
		UpdateListView();
		SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_FOUND ),
			(int)m_rows.size(), pJob->result.nTotalSize ) );
		break;

	case EJobKind::Install:
		if( m_bSearchResultShown ){
			RefreshInstalledState();
			UpdateListView();
		}
		else{
			ShowInstalledList();
		}
		SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_INSTALLED ), pJob->sTargetName.c_str() ) );
		if( !RefreshExtensionHostInventory( m_controlProcessWindow ) ){
			SetStatusText( L"Extension installed, but the extension-host inventory refresh failed." );
		}
		// The running editor's extension host still only knows the extensions it saw at
		// last connect/registration. Tell it (via the composition root's callback) that
		// the installed set may have grown, so it can pick up the new extension without
		// requiring a restart -- the same way real VS Code activates on install.
		if( m_onExtensionInstalled ){
			m_onExtensionInstalled();
		}
		break;

	case EJobKind::Uninstall:
		if( m_bSearchResultShown ){
			RefreshInstalledState();
			UpdateListView();
		}
		else{
			ShowInstalledList();
		}
		SetStatusText( strprintf( LS( STR_EXTENSION_STATUS_REMOVED ), pJob->sTargetName.c_str() ) );
		if( !RefreshExtensionHostInventory( m_controlProcessWindow ) ){
			SetStatusText( L"Extension removed, but the extension-host inventory refresh failed." );
		}
		break;

	default:
		break;
	}

	UpdateButtons();
	NotifySelectionChanged();
}
