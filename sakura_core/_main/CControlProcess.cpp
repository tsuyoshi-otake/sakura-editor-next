/*!	@file
	@brief コントロールプロセスクラス

	@author aroka
	@date 2002/01/07 Create
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2002, aroka CProcessより分離, YAZAKI
	Copyright (C) 2006, ryoji
	Copyright (C) 2007, ryoji
	Copyright (C) 2018-2026, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
#include "CControlProcess.h"
#include "CControlTray.h"
#include "env/DLLSHAREDATA.h"
#include "CCommandLine.h"
#include "env/CShareData_IO.h"
#include "debug/CRunningTimer.h"
#include "debug/StartupTrace.h"
#include "env/CShareData.h"
#include "sakura_rc.h"/// IDD_EXITTING 2002/2/10 aroka ヘッダー整理
#include "config/system_constants.h"
#include "apiwrap/DarkMode.h"
#include "platform/controlipc/ControlPlatformRuntime.h"
#include "platform/profiles/ProfileAuthorityStore.h"
#include "update/UpdateService.h"
#include "update/UpdateStagingStore.h"
#include "update/Win32UpdateLauncher.h"

//-------------------------------------------------

CControlProcess::CControlProcess(HINSTANCE hInstance, LPCWSTR lpCmdLine) :
	CProcess(hInstance, lpCmdLine)
{
}

/*!
	@brief iniファイルパスを取得する
 */
std::filesystem::path CControlProcess::GetIniFileName() const
{
	if (GetShareDataPtr()->IsPrivateSettings()) {
		return CProcess::GetIniFileName();
	}

	// exe基準のiniファイルパスを得る
	auto iniPath = GetExeFileName().replace_extension(L".ini");

	// exeと同じフォルダーに置かれたマルチユーザー構成設定ファイル
	// (sakura.exe.ini) がある場合だけ、その内容に従って保存先を決める。
	// 構成ファイルがない状態は、実行ファイルの場所に依存しない既定の
	// ユーザープロファイルとして扱う。これにより Debug/Release の切替や
	// アップデート後も、設定・拡張機能・各種プロファイルを引き継げる。
	auto exeIniPath = GetExeFileName().concat(L".ini");
	const auto filename = iniPath.filename();
	const auto exeIniAttributes = ::GetFileAttributesW(exeIniPath.c_str());
	const bool hasExeIniSettings = exeIniAttributes != INVALID_FILE_ATTRIBUTES
		&& (exeIniAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (hasExeIniSettings) {
		if (bool isMultiUserSettings = ::GetPrivateProfileInt(L"Settings", L"MultiUser", 0, exeIniPath.c_str()); isMultiUserSettings) {
			return GetPrivateIniFileName(exeIniPath, filename.wstring());
		}

		// MultiUser=0 を明示した場合は、互換用のポータブルモードとして
		// 実行ファイル隣接プロファイルを維持する。
		iniPath.remove_filename();
		if (const auto pszProfileName = GetProfileName(); *pszProfileName) {
			iniPath.append(pszProfileName);
		}
		return iniPath.append(filename.c_str());
	}

	// サイドカー設定がない場合の既定値は per-user プロファイル。
	return GetPrivateIniFileName(exeIniPath, filename.wstring());
}

/*!
	@brief マルチユーザー用のiniファイルパスを取得する
 */
std::filesystem::path CControlProcess::GetPrivateIniFileName(const std::wstring& exeIniPath, const std::wstring& filename) const
{
	const auto nFolder = ::GetPrivateProfileInt(L"Settings", L"UserRootFolder", 0, exeIniPath.c_str());
	KNOWNFOLDERID refFolderId;
	switch (nFolder) {
	case 1:
	case 3:
		refFolderId = FOLDERID_Profile;			// ユーザーのルートフォルダー
		break;
	case 2:
		refFolderId = FOLDERID_Documents;		// ユーザーのドキュメントフォルダー
		break;

	default:
		refFolderId = FOLDERID_RoamingAppData;	// ユーザーのアプリケーションデータフォルダー
		break;
	}

	PWSTR pFolderPath = nullptr;
	::SHGetKnownFolderPath(refFolderId, KF_FLAG_DEFAULT_PATH, nullptr, &pFolderPath);
	std::filesystem::path privateIniPath(pFolderPath);
	::CoTaskMemFree(pFolderPath);

	std::wstring subFolder(_MAX_DIR, L'\0');
	::GetPrivateProfileString(L"Settings", L"UserSubFolder", L"sakura", subFolder.data(), (DWORD)subFolder.capacity(), exeIniPath.c_str());
	subFolder.assign(subFolder.data());
	if (subFolder.empty())
	{
		subFolder = L"sakura";
	}
	if (nFolder == 3) {
		privateIniPath.append("Desktop");
	}
	privateIniPath.append(subFolder);

	if (const auto pszProfileName = GetProfileName(); *pszProfileName) {
		privateIniPath.append(pszProfileName);
	}

	return privateIniPath.append(filename.c_str());
}

/*!
	@brief コントロールプロセスを初期化する
	
	MutexCPを作成・ロックする。
	CControlTrayを作成する。
	
	@author aroka
	@date 2002/01/07
	@date 2002/02/17 YAZAKI 共有メモリを初期化するのはCProcessに移動。
	@date 2006/04/10 ryoji 初期化完了イベントの処理を追加、異常時の後始末はデストラクタに任せる
	@date 2013.03.20 novice コントロールプロセスのカレントディレクトリをシステムディレクトリに変更
*/
bool CControlProcess::InitializeProcess()
{
	MY_RUNNINGTIMER( cRunningTimer, L"CControlProcess::InitializeProcess" );
	CStartupTrace::SetRole(CStartupTrace::Role::Control);
	CStartupTrace::Mark(CStartupTrace::Event::ControlInitializeBegin);
	const auto pszProfileName = GetProfileName();

	// The launcher creates this event before spawning Control. Its presence also
	// transfers ownership of startup-error presentation to that launcher.
	std::wstring strInitEvent = GSTR_EVENT_SAKURA_CP_INITIALIZED;
	strInitEvent += pszProfileName;
	using HandleHolder = cxx::ResourceHolder<&::CloseHandle>;
	HandleHolder hEvent{ ::OpenEventW(EVENT_MODIFY_STATE, FALSE, std::data(strInitEvent)) };
	m_launcherOwnsStartupError = static_cast<bool>(hEvent);

	// アプリケーション実行検出用(インストーラで使用)
	m_hMutex = ::CreateMutex( nullptr, FALSE, GSTR_MUTEX_SAKURA );
	if( nullptr == m_hMutex ){
		ErrorBeep();
		if (!m_launcherOwnsStartupError) TopErrorMessage( nullptr, L"CreateMutex()失敗。\n終了します。" );
		return false;
	}

	/* コントロールプロセスの目印 */
	std::wstring strCtrlProcEvent = GSTR_MUTEX_SAKURA_CP;
	strCtrlProcEvent += pszProfileName;
	m_hMutexCP = ::CreateMutex( nullptr, TRUE, strCtrlProcEvent.c_str() );
	if( nullptr == m_hMutexCP ){
		ErrorBeep();
		if (!m_launcherOwnsStartupError) TopErrorMessage( nullptr, L"CreateMutex()失敗。\n終了します。" );
		return false;
	}
	if( ERROR_ALREADY_EXISTS == ::GetLastError() ){
		return false;
	}
	
	/* 共有メモリを初期化 */
	if( !CProcess::InitializeProcess() ){
		return false;
	}
	CStartupTrace::Mark(CStartupTrace::Event::ControlSharedDataReady);

	// コントロールプロセスのカレントディレクトリをシステムディレクトリに変更
	WCHAR szDir[_MAX_PATH];
	::GetSystemDirectory( szDir, int(std::size(szDir)) );
	::SetCurrentDirectory( szDir );

	/* 共有データのロード */
	if( !CShareData_IO::LoadShareData() ){
		/* レジストリ項目 作成 */
		CShareData_IO::SaveShareData();
	}

	// The legacy settings path is resolved before the durable platform runtime.
	// Runtime::Start reaches Running only after authority commit, storage open,
	// endpoint publication, and a successful pipe bind.  The launcher-ready event
	// below must never advertise an intermediate platform state.
	if (!StartControlPlatform()) {
		return false;
	}
	/* ダークモード設定を反映する */
	ApplyDarkModeSetting(GetDllShareData().m_Common.m_sWindow.m_bDarkMode);

	/* 言語を選択する */
	CSelectLang::ChangeLang( GetDllShareData().m_Common.m_sWindow.m_szLanguageDll );
	RefreshString();

	MY_TRACETIME( cRunningTimer, L"Before new CControlTray" );

	/* タスクトレイにアイコン作成 */
	m_pcTray = new CControlTray();

	MY_TRACETIME( cRunningTimer, L"After new CControlTray" );

	HWND hwnd = m_pcTray->Create( GetProcessInstance() );
	if( !hwnd ){
		ErrorBeep();
		if (!m_launcherOwnsStartupError) TopErrorMessage( nullptr, LS(STR_ERR_CTRLMTX3) );
		return false;
	}
	SetMainWindow(hwnd);
	GetDllShareData().m_sHandles.m_hwndTray = hwnd;
	CStartupTrace::Mark(CStartupTrace::Event::ControlTrayCreated);

	// 初期化完了イベントをシグナル状態にする。ランチャー所有イベントが
	// 無い場合も trace 上では明示的な終端として記録する。
	CStartupTrace::Mark(CStartupTrace::Event::ControlReadyEventBegin);
	if (hEvent) {
		::SetLastError(ERROR_SUCCESS);
		const BOOL setEventResult = ::SetEvent(hEvent);
		const DWORD setEventError = setEventResult ? ERROR_SUCCESS : ::GetLastError();
		CStartupTrace::Mark(
			CStartupTrace::Event::ControlReadyEventEnd,
			setEventResult ? 1 : 0,
			setEventError);
	} else {
		CStartupTrace::Mark(
			CStartupTrace::Event::ControlReadyEventEnd,
			-1,
			ERROR_INVALID_HANDLE);
	}

	return true;
}

/*!
	@brief コントロールプロセスのメッセージループ
	
	@author aroka
	@date 2002/01/07
*/
bool CControlProcess::MainLoop()
{
	if( m_pcTray && GetMainWindow() ){
		m_pcTray->MessageLoop();	/* メッセージループ */
		return true;
	}
	return false;
}

/*!
	@brief コントロールプロセスを終了する
	
	@author aroka
	@date 2002/01/07
	@date 2006/07/02 ryoji 共有データ保存を CControlTray へ移動
*/
void CControlProcess::OnExitProcess()
{
	StopControlPlatform();
	GetDllShareData().m_sHandles.m_hwndTray = nullptr;
	RunPendingUpdateInstaller();
}

/*!
	@brief 予約済みアップデートがあればインストーラーを起動する

	The control process is the last process of the application to exit, so this is
	the only place where "replace the running installation" is a possible request
	rather than a request to overwrite files that are still open. It runs after
	the editor processes and the control platform have stopped, so no storage lock
	survives into the install, and before the destructor releases
	`MutexSakuraEditor` — the installer waits that mutex out; see
	`installer/sakura-common.iss`.

	No update staged is by far the common case and costs one absent-file check.
*/
void CControlProcess::RunPendingUpdateInstaller() noexcept
{
	try {
		auto root = update::UpdateStagingStore::DefaultRoot();
		if (root.empty()) return;

		update::UpdateStagingStore stagingStore(std::move(root));
		update::Win32UpdateLauncher launcher;
		const auto outcome = update::RunPendingUpdate(stagingStore, launcher);
		if (outcome == update::EPendingUpdateOutcome::LaunchFailed) {
			// The failure is already recorded in the manifest, which is what the
			// next session reports. There is no UI left to show it in here.
			::OutputDebugStringW(L"The staged update installer could not be started.\n");
		}
	}
	catch (...) {
		::OutputDebugStringW(L"Running the staged update installer raised an unexpected exception.\n");
	}
}

CControlProcess::~CControlProcess()
{
	// InitializeProcess failures do not reach OnExitProcess. Keep destruction as
	// the idempotent rollback owner for every partial startup branch.
	StopControlPlatform();
	delete m_pcTray;

	if( m_hMutexCP ){
		::ReleaseMutex( m_hMutexCP );
	}
	::CloseHandle( m_hMutexCP );
	// 旧バージョン（1.2.104.1以前）との互換性：「異なるバージョン...」が二回出ないように
	if( m_hMutex ){
		::ReleaseMutex( m_hMutex );
	}
	::CloseHandle( m_hMutex );
};

DWORD CControlProcess::StartupExitCodeFor(
	const platform::controlipc::ControlPlatformRuntimeResult& result) noexcept
{
	using platform::controlipc::EControlPlatformRuntimeResultCode;
	using platform::storage::EStorageAuthorityOpenStatus;
	using ExitCode = EControlProcessStartupExitCode;
	if (result.code == EControlPlatformRuntimeResultCode::AuthorityFailed) {
		return static_cast<DWORD>(ExitCode::ControlPlatformAuthorityFailed);
	}
	if (result.code == EControlPlatformRuntimeResultCode::StorageCreateFailed) {
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageCreateFailed);
	}
	if (result.code != EControlPlatformRuntimeResultCode::StorageOpenFailed
		|| !result.storageOpenResult) {
		return static_cast<DWORD>(ExitCode::ControlPlatformFailed);
	}
	switch (result.storageOpenResult->status) {
	case EStorageAuthorityOpenStatus::WriterBusy:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageWriterBusy);
	case EStorageAuthorityOpenStatus::IoError:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageIoError);
	case EStorageAuthorityOpenStatus::CorruptData:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageCorruptData);
	case EStorageAuthorityOpenStatus::UnsupportedFormat:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageUnsupportedFormat);
	case EStorageAuthorityOpenStatus::GenerationRollback:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageGenerationRollback);
	case EStorageAuthorityOpenStatus::OrphanedState:
		return static_cast<DWORD>(ExitCode::ControlPlatformStorageOrphanedState);
	case EStorageAuthorityOpenStatus::Opened:
	case EStorageAuthorityOpenStatus::AlreadyOpen:
	case EStorageAuthorityOpenStatus::InvalidArgument:
	default:
		return static_cast<DWORD>(ExitCode::ControlPlatformFailed);
	}
}

std::wstring_view CControlProcess::StartupFailureMessage(DWORD exitCode) noexcept
{
	using ExitCode = EControlProcessStartupExitCode;
	switch (static_cast<ExitCode>(exitCode)) {
	case ExitCode::ControlPlatformAuthorityFailed:
		return L"プロファイルの管理情報を安全に読み書きできなかったため、起動を中止しました。\n"
			L"プロファイル内の .sakura-platform を削除せず、バックアップしてから確認してください。";
	case ExitCode::ControlPlatformStorageCreateFailed:
	case ExitCode::ControlPlatformStorageIoError:
		return L"プラットフォームの保存領域を読み書きできなかったため、起動を中止しました。\n"
			L"空き容量とアクセス権を確認してください。保存データは変更していません。";
	case ExitCode::ControlPlatformStorageWriterBusy:
		return L"同じプロファイルのプラットフォーム保存領域が別のプロセスで使用中です。\n"
			L"Sakura Editor NEXT をすべて終了してから、もう一度起動してください。";
	case ExitCode::ControlPlatformStorageCorruptData:
		return L"プラットフォームの保存状態を正しく読み取れなかったため、安全のため起動を中止しました。\n\n"
			L"復旧するには、Sakura Editor NEXT をすべて終了し、プロファイル内の\n"
			L".sakura-platform\\storage-v1.bin を削除せず別名でバックアップしてから再起動してください。\n"
			L"sakura.ini は移動・削除しないでください。エディター設定は保持されます。";
	case ExitCode::ControlPlatformStorageUnsupportedFormat:
		return L"このバージョンではプラットフォームの保存形式を読み取れないため、起動を中止しました。\n"
			L"アプリのバージョンを確認し、保存ファイルを削除せずバックアップしてください。";
	case ExitCode::ControlPlatformStorageGenerationRollback:
		return L"保存されたプラットフォーム状態の世代が、現在のプロファイル管理情報より新しいため、\n"
			L"安全のため起動を中止しました。保存データは変更していません。\n\n"
			L"復旧するには、Sakura Editor NEXT をすべて終了し、プロファイル内の\n"
			L".sakura-platform\\storage-v1.bin を削除せず別名でバックアップしてから再起動してください。\n"
			L"sakura.ini は移動・削除しないでください。エディター設定は保持されます。\n"
			L"ワークベンチ配置や最近使った項目などのプラットフォーム状態は再作成されます。";
	case ExitCode::ControlPlatformStorageOrphanedState:
		return L"プロファイル管理情報がない状態で既存のプラットフォーム保存データが見つかったため、\n"
			L"別のプロファイルとして誤って開かないよう起動を中止しました。保存データは変更していません。\n\n"
			L".sakura-platform\\storage-v1.bin を削除せず別名でバックアップしてから再起動してください。\n"
			L"sakura.ini は移動・削除しないでください。";
	case ExitCode::ControlPlatformFailed:
		return L"プラットフォームサービスの初期化に失敗したため、起動を中止しました。";
	case ExitCode::InitializationFailed:
	default:
		return L"コントロールプロセスの初期化に失敗したため、起動を中止しました。";
	}
}

DWORD CControlProcess::StartupFailureExitCode() const noexcept
{
	return m_startupExitCode;
}

bool CControlProcess::StartControlPlatform()
{
	using namespace platform::controlipc;
	try {
		if (m_controlPlatformRuntime) {
			return m_controlPlatformRuntime->State() == EControlPlatformRuntimeState::Running;
		}

		const auto profileDirectory = TryGetResolvedProfileDirectory();
		if (!profileDirectory) {
			m_startupExitCode = static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformFailed);
			if (!m_launcherOwnsStartupError) {
				TopErrorMessage(nullptr, L"%ls", StartupFailureMessage(m_startupExitCode).data());
			}
			return false;
		}
		ControlPlatformRuntimeOptions options;
		options.profileDirectory = *profileDirectory;
		options.storageDirectory = platform::profiles::BuildProfilePlatformMetadataDirectory(*profileDirectory);
		options.legacyProfileAlias = GetProfileName();

		auto runtime = std::make_unique<CControlPlatformRuntime>(std::move(options));
		const auto result = runtime->Start();
		if (result.code != EControlPlatformRuntimeResultCode::Running &&
			result.code != EControlPlatformRuntimeResultCode::AlreadyRunning) {
			m_startupExitCode = StartupExitCodeFor(result);
			if (!m_launcherOwnsStartupError) {
				TopErrorMessage(nullptr, L"%ls", StartupFailureMessage(m_startupExitCode).data());
			}
			return false;
		}

		m_controlPlatformRuntime = std::move(runtime);
		return true;
	}
	catch (...) {
		m_startupExitCode = static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformFailed);
		if (!m_launcherOwnsStartupError) {
			TopErrorMessage(nullptr, L"%ls", StartupFailureMessage(m_startupExitCode).data());
		}
		return false;
	}
}

void CControlProcess::StopControlPlatform() noexcept
{
	if (!m_controlPlatformRuntime) return;
	try {
		const auto result = m_controlPlatformRuntime->Stop();
		if (result.state != platform::controlipc::EControlPlatformRuntimeState::Stopped) {
			::OutputDebugStringW(L"Control platform runtime did not reach Stopped during process shutdown.\n");
		}
	}
	catch (...) {
		::OutputDebugStringW(L"Control platform runtime shutdown raised an unexpected exception.\n");
	}
	m_controlPlatformRuntime.reset();
}
