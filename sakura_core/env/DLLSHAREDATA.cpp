/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "DLLSHAREDATA.h"
#include "env/DLLSHAREDATA_Abi.h"
#include "env/SharedDataWin32Adapter.h"
#include <bit>
#include <sakura/shareddata/SharedDataCapabilities.h>
#include "_main/CMutex.h"
#include "dlg/CDlgCancel.h"
#include "uiparts/CWaitCursor.h"
#include "util/os.h"
#include "util/window.h"
#include "apiwrap/StdApi.h"
#include "apiwrap/CommonControl.h"
#include "CSelectLang.h"
#include "sakura_rc.h"
#include "config/system_constants.h"

namespace legacy::shareddata {

namespace {

using NativeWindowHandle = decltype(((DLLSHAREDATA*)nullptr)->m_sHandles.m_hwndTray);

[[nodiscard]] std::uintptr_t EncodeWindowHandle(NativeWindowHandle window) noexcept
{
	return std::bit_cast<std::uintptr_t>(window);
}

[[nodiscard]] NativeWindowHandle DecodeWindowHandle(std::uintptr_t window) noexcept
{
	return std::bit_cast<NativeWindowHandle>(window);
}

} // namespace

SharedDataHeaderSnapshot SharedDataHeaderReader::Snapshot() const noexcept
{
	return { m_data->m_vStructureVersion, m_data->m_nSize };
}

SharedDataMacroSnapshot SharedDataMacroReader::Snapshot() const noexcept
{
	return {
		m_data->m_sFlags.m_bEditWndChanging != FALSE,
		m_data->m_sFlags.m_bRecordingKeyMacro != FALSE,
		EncodeWindowHandle(m_data->m_sFlags.m_hwndRecordingKeyMacro),
	};
}

void SharedDataMacroWriter::SetEditWindowChanging(bool changing) noexcept
{
	m_data->m_sFlags.m_bEditWndChanging = changing ? TRUE : FALSE;
}

void SharedDataMacroWriter::StartRecording(std::uintptr_t ownerWindow) noexcept
{
	m_data->m_sFlags.m_hwndRecordingKeyMacro = DecodeWindowHandle(ownerWindow);
	m_data->m_sFlags.m_bRecordingKeyMacro = TRUE;
}

void SharedDataMacroWriter::StopRecording() noexcept
{
	m_data->m_sFlags.m_bRecordingKeyMacro = FALSE;
	m_data->m_sFlags.m_hwndRecordingKeyMacro = nullptr;
}

SharedDataWindowEndpointSnapshot SharedDataWindowEndpointReader::Snapshot() const noexcept
{
	return {
		EncodeWindowHandle(m_data->m_sHandles.m_hwndTray),
		EncodeWindowHandle(m_data->m_sHandles.m_hwndDebug),
	};
}

void SharedDataWindowEndpointWriter::SetTrayWindow(std::uintptr_t window) noexcept
{
	m_data->m_sHandles.m_hwndTray = DecodeWindowHandle(window);
}

void SharedDataWindowEndpointWriter::SetDebugWindow(std::uintptr_t window) noexcept
{
	m_data->m_sHandles.m_hwndDebug = DecodeWindowHandle(window);
}

SharedDataSettingsSnapshot SharedDataSettingsReader::Snapshot() const noexcept
{
	return { m_data->m_nTypesCount, m_data->m_nLockCount };
}

SharedDataSearchSettingsSnapshot SharedDataSearchSettingsReader::Snapshot() const noexcept
{
	return {
		m_data->m_Common.m_sSearch.m_bUseCaretKeyWord != FALSE,
		m_data->m_Common.m_sSearch.m_bGTJW_LDBLCLK != FALSE,
		m_data->m_Common.m_sSearch.m_bGTJW_RETURN != FALSE,
		m_data->m_Common.m_sEdit.m_bEnableExtEol != FALSE,
	};
}

void SharedDataSearchSettingsWriter::SetUseCaretKeyword(bool enabled) noexcept
{
	m_data->m_Common.m_sSearch.m_bUseCaretKeyWord = enabled ? TRUE : FALSE;
}

SharedDataWindowNodesSnapshot SharedDataWindowNodesReader::Snapshot() const noexcept
{
	return SharedDataWindowNodesSnapshot(m_data->m_sNodes.m_nEditArrNum);
}

void SharedDataSettingsWriter::SetTypeCount(int count) noexcept
{
	m_data->m_nTypesCount = count;
}

int SharedDataSettingsWriter::IncrementLockCount() noexcept
{
	return ++m_data->m_nLockCount;
}

int SharedDataSettingsWriter::DecrementLockCount() noexcept
{
	return --m_data->m_nLockCount;
}

SharedDataCapabilities OpenSharedDataCapabilities(DLLSHAREDATA& data) noexcept
{
	return SharedDataCapabilities(&data);
}

std::optional<SharedDataCapabilities> TryOpenSharedDataCapabilities() noexcept
{
	auto* const data = GetDllShareDataPtr();
	if (!data) return std::nullopt;
	return SharedDataCapabilities(data);
}

SharedDataCapabilities RequireSharedDataCapabilities()
{
	auto capabilities = TryOpenSharedDataCapabilities();
	if (!capabilities) throw std::domain_error("DLLSHAREDATA is not initialized");
	return *capabilities;
}

} // namespace legacy::shareddata

namespace legacy::shareddata::win32 {

void ActivateRequiredTrayWindow()
{
	const auto endpoints = RequireSharedDataCapabilities().WindowEndpoints().Snapshot();
	::SetForegroundWindow(DecodeWindowHandle(endpoints.TrayWindow()));
}

void NotifyRequiredTraySettingsChanged()
{
	const auto endpoints = RequireSharedDataCapabilities().WindowEndpoints().Snapshot();
	::SendMessageAny(DecodeWindowHandle(endpoints.TrayWindow()), MYWM_CHANGESETTING, 0, PM_CHANGESETTING_ALL);
}

bool IsMacroRecordingOwnedBy(std::uintptr_t window)
{
	const auto macro = RequireSharedDataCapabilities().Macro().Snapshot();
	return macro.IsRecording() && macro.RecordingWindow() == window;
}

} // namespace legacy::shareddata::win32

static CMutex g_cKeywordMutex( FALSE, GSTR_MUTEX_SAKURA_KEYWORD );

/*!
 * 共有データの参照を取得する
 *
 * @throw CShareDataが初期化されていない
 * 
 * @date 2007/10/30 kobake どこからでもアクセスできる、共有データアクセサ。
 */
DLLSHAREDATA& GetDllShareData()
{
	auto pShareData = GetDllShareDataPtr();
	if (!pShareData) {
		throw std::domain_error("DLLSHAREDATA is not initialized");
	}
	return *pShareData;
}

CShareDataLockCounter::CShareDataLockCounter(){
	LockGuard<CMutex> guard( g_cKeywordMutex );
	auto capabilities = legacy::shareddata::TryOpenSharedDataCapabilities();
	if (!capabilities) throw std::domain_error("DLLSHAREDATA is not initialized");
	assert_warning( 0 <= capabilities->Settings().Snapshot().LockCount() );
	capabilities->SettingsWriter().IncrementLockCount();
}

CShareDataLockCounter::~CShareDataLockCounter(){
	LockGuard<CMutex> guard( g_cKeywordMutex );
	auto capabilities = legacy::shareddata::TryOpenSharedDataCapabilities();
	if (!capabilities) return;
	const int lockCount = capabilities->SettingsWriter().DecrementLockCount();
	assert_warning( 0 <= lockCount );
}

int CShareDataLockCounter::GetLockCounter(){
	LockGuard<CMutex> guard( g_cKeywordMutex );
	auto capabilities = legacy::shareddata::TryOpenSharedDataCapabilities();
	if (!capabilities) throw std::domain_error("DLLSHAREDATA is not initialized");
	const int lockCount = capabilities->Settings().Snapshot().LockCount();
	assert_warning( 0 <= lockCount );
	return lockCount;
}

class CLockCancel final: public CDlgCancel{
public:
	BOOL OnInitDialog( HWND hwnd, WPARAM wParam, LPARAM lParam ) override{
		BOOL ret = CDlgCancel::OnInitDialog(hwnd, wParam, lParam);
		HWND hwndCancel = GetHwnd();
		HWND hwndMsg = ::GetDlgItem(hwndCancel, IDC_STATIC_MSG);
		HWND hwndCancelButton = ::GetDlgItem(hwndCancel, IDCANCEL);
		HWND hwndKensuu = ::GetDlgItem(hwndCancel, IDC_STATIC_KENSUU);
		LPCWSTR msg = LS(STR_PRINT_WAITING);
		CTextWidthCalc calc(hwndMsg);
		calc.SetTextWidthIfMax(msg);
		RECT rc;
		GetItemClientRect(IDC_STATIC_MSG, rc);
		rc.right = rc.left + calc.GetCx() + 2;
		::MoveWindow(hwndMsg, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, FALSE);
		::SetWindowText(hwndMsg, msg);
		::ShowWindow(hwndCancelButton, SW_HIDE);
		::ShowWindow(hwndKensuu, SW_HIDE);
		if( GetComctl32Version() >= PACKVERSION(6, 0) ){
			// マーキーにする(CommCtrl 6.0以上)
			HWND hwndProgress = GetItemHwnd(IDC_PROGRESS);
			// スタイル変更+メッセージでないと機能しない
			LONG_PTR style = ::GetWindowLongPtr(hwndProgress, GWL_STYLE);
			::SetWindowLongPtr(hwndProgress, GWL_STYLE, style | PBS_MARQUEE);
			ApiWrap::Progress_SetMarquee(hwndProgress, TRUE, 100);
		}else{
			HWND hwndProgress = ::GetDlgItem(hwndCancel, IDC_PROGRESS);
			::ShowWindow(hwndProgress, SW_HIDE);
		}
		return ret;
	}
};

// countが0だったらLockして返す
static int GetCountIf0Lock( CShareDataLockCounter** ppLock )
{
	LockGuard<CMutex> guard(g_cKeywordMutex);
	auto capabilities = legacy::shareddata::TryOpenSharedDataCapabilities();
	if (!capabilities) throw std::domain_error("DLLSHAREDATA is not initialized");
	int count = capabilities->Settings().Snapshot().LockCount();
	if( count <= 0 ){
		if( ppLock ){
			*ppLock = new CShareDataLockCounter();
		}
	}
	return count;
}

void CShareDataLockCounter::WaitLock( HWND hwndParent, CShareDataLockCounter** ppLock ){
	if( 0 < GetCountIf0Lock(ppLock) ){
		DWORD dwTime = ::GetTickCount();
		CWaitCursor cWaitCursor(hwndParent);
		CLockCancel* pDlg = nullptr;
		HWND hwndCancel = nullptr;
		::EnableWindow(hwndParent, FALSE);
		while( 0 < GetCountIf0Lock(ppLock) ){
			DWORD dwResult = MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLEVENTS);
			if( dwResult == 0xFFFFFFFF ){
				break;
			}
			if( !BlockingHook( hwndCancel ) ){
				break;
			}
			if( nullptr == pDlg ){
				DWORD dwTimeNow = ::GetTickCount();
				if( 2000 < dwTimeNow - dwTime ){
					pDlg = new CLockCancel();
					hwndCancel = pDlg->DoModeless(::GetModuleHandle( nullptr ), hwndParent, IDD_OPERATIONRUNNING);
				}
			}
		}
		if( pDlg ){
			pDlg->CloseDialog(0);
			delete pDlg;
		}
		::EnableWindow(hwndParent, TRUE);
	}
}
