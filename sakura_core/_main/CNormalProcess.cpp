/*!	@file
	@brief エディタプロセスクラス

	@author aroka
	@date 2002/01/07 Create
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000-2001, genta
	Copyright (C) 2002, aroka CProcessより分離
	Copyright (C) 2002, YAZAKI, Moca, genta
	Copyright (C) 2003, genta, Moca, MIK
	Copyright (C) 2004, Moca, naoh
	Copyright (C) 2007, ryoji
	Copyright (C) 2008, Uchi
	Copyright (C) 2009, syat, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
#include "CNormalProcess.h"

#include "_main/CProcessFactory.h"

#include "CCommandLine.h"
#include "CControlTray.h"
#include "window/CEditWnd.h" // 2002/2/3 aroka
#include "agent/CGrepAgent.h"
#include "doc/CEditDoc.h"
#include "doc/logic/CDocLine.h" // 2003/03/28 MIK
#include "debug/CRunningTimer.h"
#include "debug/StartupTrace.h"
#include "util/window.h"
#include "util/file.h"
#include "plugin/CPluginManager.h"
#include "plugin/CJackManager.h"
#include "CAppMode.h"
#include "apiwrap/DarkMode.h"
#include "env/CDocTypeManager.h"
#include "apiwrap/StdApi.h"
#include "CSelectLang.h"
#include "env/CShareData.h"
#include "config/system_constants.h"
#include "_main/ControlPlatformWorkbenchLayoutMementoStore.h"
#include "_main/ControlPlatformRecentlyOpenedWorkspaceStore.h"
#include "_main/ControlPlatformStatusbarVisibilityMementoStore.h"
#include "_main/ControlPlatformWorkingCopyPersistenceStore.h"
#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "platform/profiles/ProfileBootstrapSnapshot.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include <sakura/uri/UriIdentity.h>
#include "workbench/CWorkbenchRuntime.h"
#include "workbench/WorkbenchBootstrapContext.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceTypes.h"
#include "workbench/tasks/TaskTerminalSessionFactory.h"

#include <limits>

namespace
{
class CEditorReadyEventSignal final
{
public:
	explicit CEditorReadyEventSignal(HANDLE event) noexcept
		: m_event(event)
	{
	}

	~CEditorReadyEventSignal()
	{
		Signal();
	}

	void Signal() noexcept
	{
		if (m_signaled) {
			return;
		}
		m_signaled = true;
		CStartupTrace::Mark(CStartupTrace::Event::EditorReadyEventBegin);
		if (!m_event) {
			CStartupTrace::Mark(
				CStartupTrace::Event::EditorReadyEventEnd, -1, ERROR_INVALID_HANDLE);
			return;
		}

		::SetLastError(ERROR_SUCCESS);
		const BOOL result = ::SetEvent(m_event);
		const DWORD error = result ? ERROR_SUCCESS : ::GetLastError();
		CStartupTrace::Mark(
			CStartupTrace::Event::EditorReadyEventEnd, result ? 1 : 0, error);
	}

	CEditorReadyEventSignal(const CEditorReadyEventSignal&) = delete;
	CEditorReadyEventSignal& operator=(const CEditorReadyEventSignal&) = delete;

private:
	HANDLE m_event;
	bool m_signaled = false;
};

class CInitializeMutexGuard final
{
public:
	explicit CInitializeMutexGuard(HANDLE mutex) noexcept
		: m_mutex(mutex)
	{
	}

	~CInitializeMutexGuard()
	{
		Release();
	}

	explicit operator bool() const noexcept
	{
		return m_mutex != nullptr;
	}

	void Release() noexcept
	{
		if (!m_mutex) {
			return;
		}
		::ReleaseMutex(m_mutex);
		::CloseHandle(m_mutex);
		m_mutex = nullptr;
	}

	CInitializeMutexGuard(const CInitializeMutexGuard&) = delete;
	CInitializeMutexGuard& operator=(const CInitializeMutexGuard&) = delete;

private:
	HANDLE m_mutex;
};

std::optional<platform::uri::Uri> MakeAbsoluteFileUri(std::wstring_view input) noexcept
{
	if (input.empty()) return std::nullopt;
	try {
		std::error_code error;
		auto absolute = std::filesystem::absolute(std::filesystem::path(input), error);
		if (error) return std::nullopt;
		absolute = absolute.lexically_normal();
		if (absolute.empty() || !absolute.is_absolute()) return std::nullopt;
		for (const auto& component : absolute) {
			if (component == L"." || component == L"..") return std::nullopt;
		}
		auto uri = platform::uri::Uri::FromWindowsPath(absolute.native());
		return uri ? std::move(uri.value) : std::nullopt;
	}
	catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<std::string> EncodeWorkingCopyScopeId(std::wstring_view value) noexcept
{
	using namespace workbench::editor::persistence;
	if (value.empty() || value.find(L'\0') != std::wstring_view::npos
		|| value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return std::nullopt;
	}
	const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0
		|| static_cast<std::size_t>(required) > kMaximumWorkingCopyPersistenceIdBytes) {
		return std::nullopt;
	}
	try {
		std::string result(static_cast<std::size_t>(required), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required) {
			return std::nullopt;
		}
		return IsValidWorkingCopyPersistenceUtf8(
			result, false, kMaximumWorkingCopyPersistenceIdBytes)
			? std::optional{ std::move(result) } : std::nullopt;
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<workbench::editor::persistence::WorkingCopyPersistenceScope>
ResolveWorkingCopyPersistenceScope(std::string profileId,
	const config::WorkspaceContextSnapshot& workspace) noexcept
{
	using namespace workbench::editor::persistence;
	WorkingCopyPersistenceScope scope{ .profileId = std::move(profileId) };
	if (workspace.kind != config::EWorkspaceKind::Empty) {
		scope.workspaceId = EncodeWorkingCopyScopeId(workspace.workspaceIdentityKey);
		if (!scope.workspaceId) return std::nullopt;
	}
	return scope.IsValid() ? std::optional{ std::move(scope) } : std::nullopt;
}
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//               コンストラクタ・デストラクタ                  //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

CNormalProcess::CNormalProcess( HINSTANCE hInstance, LPCWSTR lpCmdLine )
: CProcess( hInstance, lpCmdLine )
{
}

CNormalProcess::~CNormalProcess()
{
	CJackManager::resetInstance();
	CPluginManager::resetInstance();

	CEditApp::resetInstance();
	// Workbench, plugin, and extension consumers must release their platform
	// references before the process-owned discovery/client/cache composition.
	StopEditorControlPlatform();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                     プロセスハンドラ                        //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!
	@brief エディタプロセスを初期化する
	
	CEditWndを作成する。
	
	@author aroka
	@date 2002/01/07

	@date 2002.2.17 YAZAKI CShareDataのインスタンスは、CProcessにひとつあるのみ。
	@date 2004.05.13 Moca CEditWnd::Create()に失敗した場合にfalseを返すように．
	@date 2007.06.26 ryoji グループIDを指定して編集ウィンドウを作成する
	@date 2012.02.25 novice 複数ファイル読み込み
*/
bool CNormalProcess::InitializeProcess()
{
	MY_RUNNINGTIMER( cRunningTimer, L"NormalProcess::Init" );

	/* プロセス初期化の目印 */
	bool initializeMutexAbandoned = false;
	CInitializeMutexGuard initializeMutex{ _GetInitializeMutex(initializeMutexAbandoned) };
	if( !initializeMutex ){
		return false;
	}

	// エディター初期化完了イベントを開く
	SFilePath initEventName{ std::format(GSTR_EVENT_SAKURA_EP_INITIALIZED, ::GetCurrentThreadId()) };
	using HandleHolder = cxx::ResourceHolder<&::CloseHandle>;
	HandleHolder hEvent{ ::OpenEventW(STANDARD_RIGHTS_REQUIRED | EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, initEventName) };

	// Every return path signals exactly once; the trace records success, failure,
	// or the explicit absence of the launcher-owned event.
	CEditorReadyEventSignal initEvent{ hEvent.get() };

	/* 共有メモリを初期化する */
	if (!CProcessFactory::IsExistControlProcess() && !CProcessFactory::StartControlProcess() || !CProcess::InitializeProcess()) {
		return false;
	}

	if (initializeMutexAbandoned) {
		const HWND trayWindow = GetDllShareData().m_sHandles.m_hwndTray;
		DWORD_PTR recoveryResult = 0;
		::SetLastError(ERROR_SUCCESS);
		const LRESULT sendResult = trayWindow
			? ::SendMessageTimeoutW(
				trayWindow,
				MYWM_RECOVER_APPNODE,
				0,
				0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK,
				5000,
				&recoveryResult)
			: 0;
		const DWORD recoveryError = sendResult
			? ERROR_SUCCESS
			: (trayWindow ? ::GetLastError() : ERROR_INVALID_WINDOW_HANDLE);
		if (!sendResult || recoveryResult != 1) {
			TopErrorMessage(
				nullptr,
				L"前回の異常終了後に共有ウィンドウ情報を修復できませんでした。\n"
				L"SendMessageTimeout() result=%lld recovery=%llu error=%lu",
				static_cast<long long>(sendResult),
				static_cast<unsigned long long>(recoveryResult),
				recoveryError);
			return false;
		}
	}

	/* ダークモード設定を反映する */
	ApplyDarkModeSetting(GetDllShareData().m_Common.m_sWindow.m_bDarkMode);

	/* 言語を選択する */
	CSelectLang::ChangeLang( GetDllShareData().m_Common.m_sWindow.m_szLanguageDll );

	/* コマンドラインオプション */
	bool			bViewMode = false;
	bool			bDebugMode;
	bool			bGrepMode;
	bool			bGrepDlg;
	EditInfo		fi;
	
	/* コマンドラインで受け取ったファイルが開かれている場合は */
	/* その編集ウィンドウをアクティブにする */
	CCommandLine::getInstance()->GetEditInfo(&fi); // 2002/2/8 aroka ここに移動
	if( fi.m_szPath[0] != L'\0' ){
		//	Oct. 27, 2000 genta
		//	MRUからカーソル位置を復元する操作はCEditDoc::FileLoadで
		//	行われるのでここでは必要なし．

		HWND hwndOwner;
		/* 指定ファイルが開かれているか調べる */
		// 2007.03.13 maru 文字コードが異なるときはワーニングを出すように
		if( GetShareData().ActiveAlreadyOpenedWindow( fi.m_szPath, &hwndOwner, fi.m_nCharCode ) ){
			//	From Here Oct. 19, 2001 genta
			//	カーソル位置が引数に指定されていたら指定位置にジャンプ
			if( fi.m_ptCursor.y >= 0 ){	//	行の指定があるか
				CLogicPoint& pt = GetDllShareData().m_sWorkBuffer.m_LogicPoint;
				if( fi.m_ptCursor.x < 0 ){
					//	桁の指定が無い場合
					::SendMessageAny( hwndOwner, MYWM_GETCARETPOS, 0, 0 );
				}
				else {
					pt.x = fi.m_ptCursor.x;
				}
				pt.y = fi.m_ptCursor.y;
				::SendMessageAny( hwndOwner, MYWM_SETCARETPOS, 0, 0 );
			}
			//	To Here Oct. 19, 2001 genta
			/* アクティブにする */
			ActivateFrameWindow( hwndOwner );
			initializeMutex.Release();

			// 複数ファイル読み込み
			OpenFiles( hwndOwner );

			return false;
		}
	}

	// A process that only forwarded an already-open file returns above and does
	// not acquire an editor platform session. A real editor process freezes the
	// control-owned endpoint before plugins or the workbench can observe state.
	if (!StartEditorControlPlatform()) {
		return false;
	}

	// Freeze all profile, workspace and launch identities before plugins or the
	// native workbench can observe them. A loose startup file remains a document
	// resource and never authorizes workspace or .vscode discovery.
	const auto platformIdentity = m_editorControlPlatformRuntime
		? m_editorControlPlatformRuntime->Identity() : std::nullopt;
	const auto profileDirectory = TryGetResolvedProfileDirectory();
	if (!platformIdentity || !profileDirectory) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"プロファイルの起動情報を確定できませんでした。");
		return false;
	}
	auto controlProfile = platform::profiles::ResolveProfileBootstrapSnapshot(
		platformIdentity->profileId,
		platformIdentity->minimumGeneration,
		profileDirectory->native());
	if (!controlProfile.Resolved()) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"制御プロファイルのリソース情報が無効です (status=%d)。",
			static_cast<int>(controlProfile.status));
		return false;
	}

	std::optional<platform::uri::Uri> explicitFolderUri;
	if (CCommandLine::getInstance()->IsSetWorkspaceFolder()) {
		explicitFolderUri = MakeAbsoluteFileUri(
			CCommandLine::getInstance()->GetWorkspaceFolder());
		if (!explicitFolderUri) {
			TopErrorMessage(nullptr,
				L"ワークベンチの初期化に失敗しました。\n"
				L"作業フォルダーを絶対リソースとして解決できませんでした。");
			return false;
		}
	}
	std::optional<platform::uri::Uri> explicitWorkspaceConfigUri;
	if (CCommandLine::getInstance()->IsSetWorkspaceConfig()) {
		explicitWorkspaceConfigUri = MakeAbsoluteFileUri(
			CCommandLine::getInstance()->GetWorkspaceConfig());
		if (!explicitWorkspaceConfigUri) {
			TopErrorMessage(nullptr,
				L"ワークベンチの初期化に失敗しました。\n"
				L"ワークスペース構成ファイルを絶対リソースとして解決できませんでした。");
			return false;
		}
	}
	std::optional<platform::uri::Uri> initialDocumentUri;
	if (fi.m_szPath[0] != L'\0') {
		initialDocumentUri = MakeAbsoluteFileUri(fi.m_szPath);
		if (!initialDocumentUri) {
			TopErrorMessage(nullptr,
				L"ワークベンチの初期化に失敗しました。\n"
				L"起動ドキュメントをリソースとして解決できませんでした。");
			return false;
		}
	}
	auto terminalLaunchDirectoryUri = MakeAbsoluteFileUri(L".");
	if (!terminalLaunchDirectoryUri) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"ターミナルの起動ディレクトリを確定できませんでした。");
		return false;
	}

	// Fetch one immutable, generation-pinned registry document through the
	// editor runtime facade. The editor never opens the control-owned registry
	// or its storage directly.
	platform::controlipc::ControlProfileRpcRequest profileSnapshotRequest;
	profileSnapshotRequest.operation = platform::controlipc::EControlProfileRpcOperation::Snapshot;
	const auto profileSnapshot = m_editorControlPlatformRuntime->ExecuteProfile(profileSnapshotRequest);
	if (profileSnapshot.code != platform::controlipc::EEditorControlProfileExecuteCode::Succeeded
		|| !profileSnapshot.response
		|| profileSnapshot.response->terminalStatus != platform::controlipc::EControlIpcTerminalStatus::Succeeded
		|| !profileSnapshot.response->result.Succeeded()
		|| profileSnapshot.response->snapshotDocument.empty()) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"ユーザーデータプロファイルのスナップショットを取得できませんでした (status=%d)。",
			static_cast<int>(profileSnapshot.code));
		return false;
	}

	platform::profiles::UserDataProfileBootstrapRequest userDataRequest {
		.controlAuthority = { platformIdentity->profileId, platformIdentity->minimumGeneration },
		.controlProfileRoot = profileDirectory->native(),
		.resourceRootMode = platform::profiles::UserDataProfileResourceRootMode::ProfileIdNamespace,
	};
	if (explicitFolderUri) {
		userDataRequest.selection.workspaceUri = *explicitFolderUri;
	} else if (explicitWorkspaceConfigUri) {
		userDataRequest.selection.workspaceUri = *explicitWorkspaceConfigUri;
	} else {
		// This fixed compatibility token is stable across process restarts and
		// deliberately distinct from the process-local window instance identity.
		// A future control-owned window identity store can replace it when
		// multiple independently associated empty windows are supported.
		userDataRequest.selection.emptyWindowId = L"empty-window:default-window";
	}
	auto userDataProfile = platform::profiles::ResolveUserDataProfileBootstrap(
		userDataRequest, profileSnapshot.response->snapshotDocument);
	if (userDataProfile.Resolved()
		&& userDataProfile.snapshot->SelectedProfile().kind == platform::profiles::UserDataProfileKind::Default) {
		// Preserve the existing default-profile resources until a durable,
		// marker-backed namespace migration has completed. Named and transient
		// profiles never use this compatibility bridge.
		userDataRequest.resourceRootMode =
			platform::profiles::UserDataProfileResourceRootMode::LegacyControlRootForDefault;
		userDataProfile = platform::profiles::ResolveUserDataProfileBootstrap(
			userDataRequest, profileSnapshot.response->snapshotDocument);
	}
	if (!userDataProfile.Resolved()) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"ユーザーデータプロファイルの選択またはリソース情報が無効です (status=%d)。",
			static_cast<int>(userDataProfile.status));
		return false;
	}

	workbench::WorkbenchBootstrapRequest bootstrapRequest {
		.controlProfile = std::move(*controlProfile.snapshot),
		.userDataProfile = std::move(*userDataProfile.snapshot),
		.windowInstanceIdentity = L"editor-process:" + std::to_wstring(::GetCurrentProcessId()),
		.explicitFolderUri = std::move(explicitFolderUri),
		.explicitWorkspaceConfigUri = std::move(explicitWorkspaceConfigUri),
		.initialDocumentUri = std::move(initialDocumentUri),
		.terminalLaunchDirectoryUri = std::move(terminalLaunchDirectoryUri),
	};
	auto bootstrap = workbench::ResolveWorkbenchBootstrapContext(std::move(bootstrapRequest));
	if (!bootstrap.Resolved()) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"起動コンテキストが無効です (status=%d)。",
			static_cast<int>(bootstrap.status));
		return false;
	}
	auto workingCopyScope = ResolveWorkingCopyPersistenceScope(
		platformIdentity->profileId, bootstrap.context->Workspace());
	if (!workingCopyScope) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"作業コピーの永続化スコープを確定できませんでした。");
		return false;
	}
	// プラグイン読み込み
	MY_TRACETIME( cRunningTimer, L"Before Init Jack" );
	/* ジャック初期化 */
	CJackManager::getInstance();
	MY_TRACETIME( cRunningTimer, L"After Init Jack" );

	MY_TRACETIME( cRunningTimer, L"Before Load Plugins" );
	/* プラグイン読み込み */
	CPluginManager::getInstance()->LoadAllPlugin();
	MY_TRACETIME( cRunningTimer, L"After Load Plugins" );

	// エディタアプリケーションを作成。2007.10.23 kobake
	// グループIDを取得
	int nGroupId = CCommandLine::getInstance()->GetGroupId();
	if( GetDllShareData().m_Common.m_sTabBar.m_bNewWindow && nGroupId == -1 ){
		nGroupId = CAppNodeManager::getInstance()->GetFreeGroupId();
	}
	// CEditAppを作成
	m_pcEditApp = CEditApp::getInstance();
	workbench::WorkbenchRuntimeDependencies workbenchDependencies;
	workbenchDependencies.layoutMementoStore =
		std::make_unique<CControlPlatformWorkbenchLayoutMementoStore>(
			*m_editorControlPlatformRuntime, platformIdentity->profileId);
	workbenchDependencies.recentlyOpenedWorkspaceStore =
		std::make_unique<CControlPlatformRecentlyOpenedWorkspaceStore>(
			*m_editorControlPlatformRuntime, platformIdentity->profileId);
	workbenchDependencies.statusbarVisibilityMementoStore =
		std::make_unique<CControlPlatformStatusbarVisibilityMementoStore>(
			*m_editorControlPlatformRuntime, platformIdentity->profileId);
	workbenchDependencies.taskExecutionSessionFactory =
		workbench::tasks::CreateDefaultTaskTerminalSessionFactory();
	auto workingCopyStore = std::make_unique<CControlPlatformWorkingCopyPersistenceStore>(
		*m_editorControlPlatformRuntime, platformIdentity->profileId);
	if (!m_pcEditApp->Create(
		GetProcessInstance(), nGroupId, std::move(*bootstrap.context), std::move(workbenchDependencies),
		std::move(workingCopyStore), std::move(*workingCopyScope))) {
		TopErrorMessage(nullptr,
			L"ワークベンチの初期化に失敗しました。\n"
			L"設定またはワークスペースサービスを開始できませんでした。");
		return false;
	}
	CEditWnd* pEditWnd = m_pcEditApp->GetEditWindow();

	const auto hEditWnd = pEditWnd->GetHwnd();
	if (!hEditWnd) {
		return false;	// 2009.06.23 ryoji CEditWnd::Create()失敗のため終了
	}

	/* コマンドラインの解析 */	 // 2002/2/8 aroka ここに移動
	bDebugMode = CCommandLine::getInstance()->IsDebugMode();
	bGrepMode  = CCommandLine::getInstance()->IsGrepMode();
	bGrepDlg   = CCommandLine::getInstance()->IsGrepDlg();
	const auto restoreResult = m_pcEditApp->RestoreWorkingCopies({
		.explicitCommandLine = fi.m_szPath[0] != L'\0',
		.multipleFiles = CCommandLine::getInstance()->GetFileNum() > 0,
		.debugOrGrep = bDebugMode || bGrepMode,
	});
	if (restoreResult.status != workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::Succeeded
		&& restoreResult.status != workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::NotApplicable
		&& restoreResult.status != workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::Suppressed) {
		if (restoreResult.reason
			== workbench::editor::persistence::EEditorWorkingCopyLifecycleReason::CoreActivationFailed
			|| restoreResult.reason
			== workbench::editor::persistence::EEditorWorkingCopyLifecycleReason::NativeProjectionFailed) {
			TopErrorMessage(nullptr,
				L"作業コピーの回復を画面へ反映できませんでした。\n"
				L"回復バックアップは保持されています (status=%d reason=%d)。",
				static_cast<int>(restoreResult.status), static_cast<int>(restoreResult.reason));
			return false;
		}
		wchar_t diagnostic[160]{};
		::swprintf_s(diagnostic,
			L"Working-copy restore completed without adoption (status=%d reason=%d).\n",
			static_cast<int>(restoreResult.status), static_cast<int>(restoreResult.reason));
		::OutputDebugStringW(diagnostic);
	}

	MY_TRACETIME( cRunningTimer, L"CheckFile" );

	// -1: SetDocumentTypeWhenCreate での強制指定なし
	const CTypeConfig nType = (fi.m_szDocType[0] == '\0' ? CTypeConfig(-1) : CDocTypeManager().GetDocumentTypeOfExt(fi.m_szDocType));

	if( bDebugMode ){
		/* デバッグモニタモードに設定 */
		pEditWnd->GetDocument()->SetCurDirNotitle();
		CAppMode::getInstance()->SetDebugModeON();
		if( !CAppMode::getInstance()->IsDebugMode() ){
			// デバッグではなくて(無題)
			CAppNodeManager::getInstance()->GetNoNameNumber( pEditWnd->GetHwnd() );
			pEditWnd->UpdateCaption();
		}
		// 2004.09.20 naoh アウトプット用タイプ別設定
		// 文字コードを有効とする Uchi 2008/6/8
		// 2010.06.16 Moca アウトプットは CCommnadLineで -TYPE=output 扱いとする
		pEditWnd->SetDocumentTypeWhenCreate( fi.m_nCharCode, false, nType );
		if (!pEditWnd->AdoptLegacyUntitledInput("debug")) return false;
		pEditWnd->m_cDlgFuncList.Refresh();	// アウトラインを表示する
	}
	else if( bGrepMode ){
		/* GREP */
		// 2010.06.16 Moca Grepでもオプション指定を適用
		pEditWnd->SetDocumentTypeWhenCreate( fi.m_nCharCode, false, nType );
		pEditWnd->m_cDlgFuncList.Refresh();	// アウトラインを予め表示しておく
		if( !::IsIconic( hEditWnd ) && pEditWnd->m_cDlgFuncList.GetHwnd() ){
			RECT rc;
			::GetClientRect( hEditWnd, &rc );
			::SendMessageAny( hEditWnd, WM_SIZE, ::IsZoomed( hEditWnd )? SIZE_MAXIMIZED: SIZE_RESTORED, MAKELONG( rc.right - rc.left, rc.bottom - rc.top ) );
		}
		GrepInfo gi;
		CCommandLine::getInstance()->GetGrepInfo(&gi); // 2002/2/8 aroka ここに移動
		if (!pEditWnd->AdoptLegacyUntitledInput("grep")) return false;
		// Grep can run for a long time. Present the initialized editor before it starts.
		pEditWnd->CommitStartupDrawTransaction();
		if( !bGrepDlg ){
			// Grepでは対象パス解析に現在のカレントディレクトリを必要とする
			// pEditWnd->GetDocument()->SetCurDirNotitle();
			// 2003.06.23 Moca GREP実行前にMutexを解放
			//	こうしないとGrepが終わるまで新しいウィンドウを開けない
			SetMainWindow( pEditWnd->GetHwnd() );
			initializeMutex.Release();
			this->m_pcEditApp->GetGrepAgent()->DoGrep(
				&pEditWnd->GetActiveView(),
				gi.bGrepReplace,
				&gi.cmGrepKey,
				&gi.cmGrepRep,
				&gi.cmGrepFile,
				&gi.cmGrepFolder,
				gi.bGrepCurFolder,
				gi.bGrepSubFolder,
				gi.bGrepStdout,
				gi.bGrepHeader,
				gi.sGrepSearchOption,
				gi.nGrepCharSet,	//	2002/09/21 Moca
				gi.nGrepOutputLineType,
				gi.nGrepOutputStyle,
				gi.bGrepOutputFileOnly,
				gi.bGrepOutputBaseFolder,
				gi.bGrepSeparateFolder,
				gi.bGrepPaste,
				gi.bGrepBackup
			);
			pEditWnd->m_cDlgFuncList.Refresh();	// アウトラインを再解析する
		}
		else{
			CAppNodeManager::getInstance()->GetNoNameNumber( pEditWnd->GetHwnd() );
			pEditWnd->UpdateCaption();
			
			//-GREPDLGでダイアログを出す。　引数も反映（2002/03/24 YAZAKI）
			if( gi.cmGrepKey.GetStringLength() < _MAX_PATH ){
				CSearchKeywordManager().AddToSearchKeyArr( gi.cmGrepKey.GetStringPtr() );
			}
			if( gi.cmGrepFile.GetStringLength() < MAX_GREP_PATH ){
				CSearchKeywordManager().AddToGrepFileArr( gi.cmGrepFile.GetStringPtr() );
			}
			CNativeW cmemGrepFolder = gi.cmGrepFolder;
			if( gi.cmGrepFolder.GetStringLength() < MAX_GREP_PATH ){
				CSearchKeywordManager().AddToGrepFolderArr( gi.cmGrepFolder.GetStringPtr() );
				// 2013.05.21 指定なしの場合はカレントフォルダーにする
				if( cmemGrepFolder.GetStringLength() == 0 ){
					WCHAR szCurDir[_MAX_PATH];
					::GetCurrentDirectory( int(std::size(szCurDir)), szCurDir );
					cmemGrepFolder.SetString( szCurDir );
				}
			}
			GetDllShareData().m_Common.m_sSearch.m_bGrepSubFolder = gi.bGrepSubFolder;
			GetDllShareData().m_Common.m_sSearch.m_sSearchOption = gi.sGrepSearchOption;
			GetDllShareData().m_Common.m_sSearch.m_nGrepCharSet = gi.nGrepCharSet;
			GetDllShareData().m_Common.m_sSearch.m_nGrepOutputLineType = gi.nGrepOutputLineType;
			GetDllShareData().m_Common.m_sSearch.m_nGrepOutputStyle = gi.nGrepOutputStyle;
			// 2003.06.23 Moca GREPダイアログ表示前にMutexを解放
			//	こうしないとGrepが終わるまで新しいウィンドウを開けない
			SetMainWindow( pEditWnd->GetHwnd() );
			initializeMutex.Release();
			
			//	Oct. 9, 2003 genta コマンドラインからGERPダイアログを表示させた場合に
			//	引数の設定がBOXに反映されない
			pEditWnd->m_cDlgGrep.m_strText = gi.cmGrepKey.GetStringPtr();		/* 検索文字列 */
			pEditWnd->m_cDlgGrep.m_bSetText = true;
			int nSize = std::size(pEditWnd->m_cDlgGrep.m_szFile);
			wcsncpy( pEditWnd->m_cDlgGrep.m_szFile, gi.cmGrepFile.GetStringPtr(), nSize );	/* 検索ファイル */
			pEditWnd->m_cDlgGrep.m_szFile[nSize-1] = L'\0';
			nSize = std::size(pEditWnd->m_cDlgGrep.m_szFolder);
			wcsncpy( pEditWnd->m_cDlgGrep.m_szFolder, cmemGrepFolder.GetStringPtr(), nSize );	/* 検索フォルダー */
			pEditWnd->m_cDlgGrep.m_szFolder[nSize-1] = L'\0';

			// Feb. 23, 2003 Moca Owner windowが正しく指定されていなかった
			if (pEditWnd->m_cDlgGrep.DoModal(GetProcessInstance(), hEditWnd,  LPCWSTR(nullptr))) {
				pEditWnd->GetActiveView().GetCommander().HandleCommand(F_GREP, true, 0, 0, 0, 0);
			}else{
				// 自分はGrepでない
				pEditWnd->GetDocument()->SetCurDirNotitle();
			}
			pEditWnd->m_cDlgFuncList.Refresh();	// アウトラインを再解析する
		}

		//プラグイン：EditorStartイベント実行
		CJackManager::getInstance()->InvokePlugins( PP_EDITOR_START, &pEditWnd->GetActiveView() );

		//プラグイン：DocumentOpenイベント実行
		if (pEditWnd->HasActiveEditorInput()) {
			CJackManager::getInstance()->InvokePlugins(PP_DOCUMENT_OPEN, &pEditWnd->GetActiveView());
		}

		if( !bGrepDlg && gi.bGrepStdout ){
			// 即時終了
			PostMessageCmd( pEditWnd->GetHwnd(), MYWM_CLOSE, PM_CLOSE_GREPNOCONFIRM | PM_CLOSE_EXIT, (LPARAM)nullptr );
		}

		return true; // 2003.06.23 Moca
	}
	else{
		// 2004.05.13 Moca さらにif分の中から前に移動
		// ファイル名が与えられなくてもReadOnly指定を有効にするため．
		bViewMode = CCommandLine::getInstance()->IsViewMode(); // 2002/2/8 aroka ここに移動
		if( fi.m_szPath[0] != L'\0' ){
			//	Mar. 9, 2002 genta 文書タイプ指定
			pEditWnd->OpenDocumentWhenStart(
				SLoadInfo(
					fi.m_szPath,
					fi.m_nCharCode,
					bViewMode,
					nType
				)
			);
			// 読み込み中断して「(無題)」になった時（他プロセスからのロックなど）もオプション指定を有効にする
			// Note. fi.m_nCharCode で文字コードが明示指定されていても、読み込み中断しない場合は別の文字コードが選択されることがある。
			//       以前は「(無題)」にならない場合でも無条件に SetDocumentTypeWhenCreate() を呼んでいたが、
			//       「前回と異なる文字コード」の問い合わせで前回の文字コードが選択された場合におかしくなっていた。
			if( !pEditWnd->GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath() ){
				// 読み込み中断して「(無題)」になった
				// ---> 無効になったオプション指定を有効にする
				pEditWnd->SetDocumentTypeWhenCreate(
					fi.m_nCharCode,
					bViewMode,
					nType
				);
			}
			//	Nov. 6, 2000 genta
			//	キャレット位置の復元のため
			//	オプション指定がないときは画面移動を行わないようにする
			//	Oct. 19, 2001 genta
			//	未設定＝-1になるようにしたので，安全のため両者が指定されたときだけ
			//	移動するようにする． || → &&
			if( ( CLayoutInt(0) <= fi.m_nViewTopLine && CLayoutInt(0) <= fi.m_nViewLeftCol )
				&& fi.m_nViewTopLine < pEditWnd->GetDocument()->m_cLayoutMgr.GetLineCount() ){
				pEditWnd->GetActiveView().GetTextArea().SetViewTopLine( fi.m_nViewTopLine );
				pEditWnd->GetActiveView().GetTextArea().SetViewLeftCol( fi.m_nViewLeftCol );
			}

			//	オプション指定がないときはカーソル位置設定を行わないようにする
			//	Oct. 19, 2001 genta
			//	0も位置としては有効な値なので判定に含めなくてはならない
			if( 0 <= fi.m_ptCursor.x || 0 <= fi.m_ptCursor.y ){
				/*
				  カーソル位置変換
				  物理位置(行頭からのバイト数、折り返し無し行位置)
				  →
				  レイアウト位置(行頭からの表示桁位置、折り返しあり行位置)
				*/
				CLayoutPoint ptPos;
				pEditWnd->GetDocument()->m_cLayoutMgr.LogicToLayout(
					fi.m_ptCursor,
					&ptPos
				);

				// From Here Mar. 28, 2003 MIK
				// 改行の真ん中にカーソルが来ないように。
				// 2008.08.20 ryoji 改行単位の行番号を渡すように修正
				const CDocLine *pTmpDocLine = pEditWnd->GetDocument()->m_cDocLineMgr.GetLine( fi.m_ptCursor.GetY2() );
				if( pTmpDocLine ){
					if( pTmpDocLine->GetLengthWithoutEOL() < fi.m_ptCursor.x ) ptPos.x--;
				}
				// To Here Mar. 28, 2003 MIK

				pEditWnd->GetActiveView().GetCaret().MoveCursor( ptPos, true );
				pEditWnd->GetActiveView().GetCaret().m_nCaretPosX_Prev =
					pEditWnd->GetActiveView().GetCaret().GetCaretLayoutPos().GetX2();
			}
		}
		else{
			// No startup resource is a genuine empty editor group. The legacy CEditDoc
			// remains an inert backing object until an explicit New/Open operation adopts it.
			pEditWnd->UpdateCaption();
		}
	}

	SetMainWindow( pEditWnd->GetHwnd() );

	//	YAZAKI 2002/05/30 IMEウィンドウの位置がおかしいのを修正。
	if (pEditWnd->HasActiveEditorInput()) pEditWnd->GetActiveView().SetIMECompFormPos();

	// Coalesce startup WM_SIZE/redraw work and reveal the final document once.
	pEditWnd->CommitStartupDrawTransaction();

	initializeMutex.Release();

	//プラグイン：EditorStartイベント実行
	CJackManager::getInstance()->InvokePlugins(PP_EDITOR_START, &pEditWnd->GetActiveView());

	// 2006.09.03 ryoji オープン後自動実行マクロを実行する
	if( pEditWnd->HasActiveEditorInput() && !( bDebugMode || bGrepMode ) )
		pEditWnd->GetDocument()->RunAutoMacro( GetDllShareData().m_Common.m_sMacro.m_nMacroOnOpened );

	// 起動時マクロオプション
	if (const auto pszMacro = CCommandLine::getInstance()->GetMacro();
		pEditWnd->HasActiveEditorInput() && pszMacro && pszMacro[0] != L'\0')
	{
		LPCWSTR pszMacroType = CCommandLine::getInstance()->GetMacroType();
		if( pszMacroType == nullptr || pszMacroType[0] == L'\0' || _wcsicmp(pszMacroType, L"file") == 0 ){
			pszMacroType = nullptr;
		}
		CEditView& view = pEditWnd->GetActiveView();
		view.GetCommander().HandleCommand( F_EXECEXTMACRO, true, (LPARAM)pszMacro, (LPARAM)pszMacroType, 0, 0 );
	}

	//プラグイン：DocumentOpenイベント実行
	if (pEditWnd->HasActiveEditorInput()) {
		CJackManager::getInstance()->InvokePlugins(PP_DOCUMENT_OPEN, &pEditWnd->GetActiveView());
	}

	// 複数ファイル読み込み
	OpenFiles( pEditWnd->GetHwnd() );

	initEvent.Signal();
	// The successor owns MRU promotion after crossing the same ready boundary
	// observed by the predecessor.  This keeps the live window's process-local
	// snapshot and CAS coordinates coherent with the durable mutation.
	pEditWnd->RecordCurrentWorkspaceAfterReady();

	return pEditWnd->GetHwnd() ? true : false;
}

/*!
	@brief エディタプロセスのメッセージループ
	
	@author aroka
	@date 2002/01/07
*/
bool CNormalProcess::MainLoop()
{
	if( GetMainWindow() ){
		m_pcEditApp->GetEditWindow()->MessageLoop();	/* メッセージループ */
		return true;
	}
	return false;
}

/*!
	@brief エディタプロセスを終了する
	
	@author aroka
	@date 2002/01/07
	こいつはなにもしない。後始末はdtorで。
*/
void CNormalProcess::OnExitProcess()
{
	/* プラグイン解放 */
	CPluginManager::getInstance()->UnloadAllPlugin();		// Mpve here	2010/7/11 Uchi
}

bool CNormalProcess::StartEditorControlPlatform()
{
	using namespace platform::controlipc;
	try {
		if (m_editorControlPlatformRuntime) {
			const auto result = m_editorControlPlatformRuntime->Start();
			return result.code == EEditorControlPlatformRuntimeResultCode::Ready ||
				result.code == EEditorControlPlatformRuntimeResultCode::AlreadyReady;
		}

		const auto profileDirectory = TryGetResolvedProfileDirectory();
		if (!profileDirectory) {
			TopErrorMessage(nullptr,
				L"プラットフォームサービスへの接続に失敗しました。\n"
				L"共有設定のプロファイルディレクトリを取得できませんでした。");
			return false;
		}

		EditorControlPlatformRuntimeOptions options;
		options.profileDirectory = *profileDirectory;
		options.allowDegradedUnavailable = false;
		options.clientOptions.retryJitterSalt = ::GetCurrentProcessId();
		auto runtime = std::make_unique<CEditorControlPlatformRuntime>(std::move(options));
		const auto result = runtime->Start();
		if (result.code != EEditorControlPlatformRuntimeResultCode::Ready &&
			result.code != EEditorControlPlatformRuntimeResultCode::AlreadyReady) {
			const int clientOutcome = result.clientResult
				? static_cast<int>(result.clientResult->outcome) : -1;
			const int terminalStatus = result.clientResult
				? static_cast<int>(result.clientResult->terminalStatus) : -1;
			const int discoveryStatus = result.clientResult
				? static_cast<int>(result.clientResult->discoveryDisposition) : -1;
			const int transportStatus = result.clientResult
				? static_cast<int>(result.clientResult->transportReason) : -1;
			TopErrorMessage(nullptr,
				L"プラットフォームサービスへの接続に失敗しました。\n"
				L"runtime=%d state=%d client=%d terminal=%d discovery=%d transport=%d",
				static_cast<int>(result.code), static_cast<int>(result.state), clientOutcome,
				terminalStatus, discoveryStatus, transportStatus);
			return false;
		}

		m_editorControlPlatformRuntime = std::move(runtime);
		return true;
	}
	catch (...) {
		TopErrorMessage(nullptr, L"プラットフォームサービスへの接続中に予期しないエラーが発生しました。");
		return false;
	}
}

void CNormalProcess::StopEditorControlPlatform() noexcept
{
	if (!m_editorControlPlatformRuntime) return;
	try {
		const auto result = m_editorControlPlatformRuntime->Stop();
		if (result.state != platform::controlipc::EEditorControlPlatformRuntimeState::Stopped) {
			::OutputDebugStringW(L"Editor control platform runtime did not reach Stopped during process shutdown.\n");
		}
	}
	catch (...) {
		::OutputDebugStringW(L"Editor control platform runtime shutdown raised an unexpected exception.\n");
	}
	m_editorControlPlatformRuntime.reset();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                         実装補助                            //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!
	@brief Mutex(プロセス初期化の目印)を取得する

	多数同時に起動するとウィンドウが表に出てこないことがある。
	
	@date 2002/2/8 aroka InitializeProcessから移動
	@retval Mutex のハンドルを返す
	@retval 失敗した時はリリースしてから NULL を返す
*/
HANDLE CNormalProcess::_GetInitializeMutex(bool& abandoned) const
{
	MY_RUNNINGTIMER( cRunningTimer, L"NormalProcess::_GetInitializeMutex" );
	abandoned = false;
	HANDLE hMutex;
	const auto pszProfileName = GetProfileName();
	std::wstring strMutexInitName = GSTR_MUTEX_SAKURA_INIT;
	strMutexInitName += pszProfileName;
	hMutex = ::CreateMutex( nullptr, TRUE, strMutexInitName.c_str() );
	if( nullptr == hMutex ){
		ErrorBeep();
		TopErrorMessage( nullptr, L"CreateMutex()失敗。\n終了します。" );
		return nullptr;
	}
	if( ::GetLastError() == ERROR_ALREADY_EXISTS ){
		DWORD dwRet = ::WaitForSingleObject( hMutex, 15000 );	// 2002/2/8 aroka 少し長くした
		if( WAIT_TIMEOUT == dwRet ){// 別の誰かが起動中
			TopErrorMessage( nullptr, L"エディタまたはシステムがビジー状態です。\nしばらく待って開きなおしてください。" );
			::CloseHandle( hMutex );
			return nullptr;
		}
		if( WAIT_FAILED == dwRet ){
			const DWORD error = ::GetLastError();
			TopErrorMessage( nullptr,
				L"起動同期オブジェクトの待機に失敗しました。\n"
				L"WaitForSingleObject() result=0x%08lX error=%lu",
				dwRet, error );
			::CloseHandle( hMutex );
			return nullptr;
		}
		if( WAIT_OBJECT_0 != dwRet && WAIT_ABANDONED != dwRet ){
			TopErrorMessage( nullptr,
				L"起動同期オブジェクトから予期しない結果が返されました。\n"
				L"WaitForSingleObject() result=0x%08lX",
				dwRet );
			::CloseHandle( hMutex );
			return nullptr;
		}
		// WAIT_ABANDONED transfers ownership after the previous owner exited.  The
		// caller repairs stale AppNode entries through the control process while it
		// still owns this mutex, before creating or querying an editor window.
		abandoned = (WAIT_ABANDONED == dwRet);
	}
	return hMutex;
}

/*!
	@brief 複数ファイル読み込み

	@date 2015.03.14 novice 新規作成
*/
void CNormalProcess::OpenFiles(HWND hwnd) const
{
	EditInfo fi;
	CCommandLine::getInstance()->GetEditInfo( &fi );
	bool bViewMode = CCommandLine::getInstance()->IsViewMode();

	if (auto fileNum = CCommandLine::getInstance()->GetFileNum();
		0 < fileNum)
	{
		// ファイルドロップ数の上限に合わせる
		if (const auto nDropFileNumMax = GetDllShareData().m_Common.m_sFile.m_nDropFileNumMax - 1;
			nDropFileNumMax < fileNum) {
			fileNum = nDropFileNumMax;
		}

		for (int i = 0; i < fileNum; ++i) {
			// ファイル名差し替え
			::wcscpy_s(fi.m_szPath, CCommandLine::getInstance()->GetFileName(i));

			// 同期モードでファイルを開いていく
			if (!CControlTray::OpenNewEditor2(GetProcessInstance(), hwnd, &fi, bViewMode, true)) {
				break;
			}
		}

		// 用済みなので削除
		CCommandLine::getInstance()->ClearFile();
	}
}
