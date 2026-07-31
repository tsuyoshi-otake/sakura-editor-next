/*! @file */
/*
	Copyright (C) 2007, kobake
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "CEditApp.h"
#include "doc/CEditDoc.h"
#include "window/CEditWnd.h"
#include "agent/CLoadAgent.h"
#include "agent/CSaveAgent.h"
#include "uiparts/CVisualProgress.h"
#include "recent/CMruListener.h"
#include "macro/CSMacroMgr.h"
#include "env/CPropertyManager.h"
#include "extension/IExtensionSecretStorage.h"
#include "agent/CGrepAgent.h"
#include "_main/CAppMode.h"
#include "_main/CCommandLine.h"
#include "util/module.h"
#include "util/shell.h"
#include "workbench/editor/CEditDocLegacyEditorBackend.h"
#include "workbench/editor/CEditDocWorkingCopyBackend.h"
#include "workbench/editor/CEditorServiceLegacyAdapter.h"
#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/EditorWorkingCopyCoordinator.h"
#include "workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.h"
#include "workbench/editor/persistence/EditorCoreRecoveredInputAdopter.h"
#include "workbench/editor/persistence/EditorCoreWorkingCopyCaptureContextSource.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycle.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"
#include "workbench/editor/persistence/IWorkingCopyPersistenceStore.h"
#include "config/BuiltinConfigurationDescriptors.h"
#include "workbench/CWorkbenchRuntime.h"

CEditApp::CEditApp() = default;

bool CEditApp::Create(
	HINSTANCE hInst,
	int nGroupId,
	workbench::WorkbenchBootstrapContext bootstrap,
	workbench::WorkbenchRuntimeDependencies dependencies,
	std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> workingCopyStore,
	workbench::editor::persistence::WorkingCopyPersistenceScope workingCopyScope,
	std::filesystem::path profileDirectory,
	std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage)
{
	m_hInst = hInst;
	if (!workingCopyStore || !workingCopyScope.IsValid() || profileDirectory.empty()
		|| !extensionSecretStorage) return false;
	try {
		auto runtime = std::make_unique<workbench::CWorkbenchRuntime>(
			std::move(bootstrap), config::BuiltinConfigurationDescriptors(), std::move(dependencies));
		const auto started = runtime->Start();
		if (!started.IsUsable()) return false;
		m_workbenchRuntime = std::move(runtime);
	}
	catch (...) {
		return false;
	}
	m_editorCoreService = std::make_unique<workbench::editor::EditorCoreService>();

	//ヘルパ作成
	m_cIcons.Create( m_hInst );	//	CreateImage List

	//ドキュメントの作成
	m_pcEditDoc = new CEditDoc(this);

	//IO管理
	m_pcLoadAgent = new CLoadAgent();
	m_pcSaveAgent = new CSaveAgent();
	m_pcVisualProgress = new CVisualProgress();

	//GREPモード管理
	m_pcGrepAgent = new CGrepAgent();

	//編集モード
	CAppMode::getInstance();	//ウィンドウよりも前にイベントを受け取るためにここでインスタンス作成

	//マクロ
	m_pcSMacroMgr = new CSMacroMgr();

	//ドキュメントの作成
	m_pcEditDoc->Create();
	m_legacyEditorBackend = std::make_unique<workbench::editor::CEditDocLegacyEditorBackend>(*m_pcEditDoc);
	m_editorServiceLegacyAdapter = std::make_unique<workbench::editor::CEditorServiceLegacyAdapter>(
		*m_editorCoreService, *m_legacyEditorBackend);
	m_workingCopyBackend = std::make_unique<workbench::editor::CEditDocWorkingCopyBackend>(*m_pcEditDoc);
	m_workingCopyCoordinator = std::make_unique<workbench::editor::EditorWorkingCopyCoordinator>(
		*m_editorCoreService, *m_workingCopyBackend);
	m_workingCopyPersistenceStore = std::move(workingCopyStore);
	m_workingCopyContextSource =
		std::make_unique<workbench::editor::persistence::EditorCoreWorkingCopyCaptureContextSource>(
			*m_editorCoreService);
	m_workingCopyPersistenceAdapter =
		std::make_unique<workbench::editor::persistence::CEditDocWorkingCopyPersistenceAdapter>(
			*m_pcEditDoc, *m_workingCopyContextSource);
	m_workingCopyRecoveredInputAdopter =
		std::make_unique<workbench::editor::persistence::EditorCoreRecoveredInputAdopter>(
			*m_editorCoreService);
	m_workingCopyLifecycle =
		std::make_unique<workbench::editor::persistence::EditorWorkingCopyLifecycle>(
			*m_workingCopyPersistenceStore, *m_workingCopyPersistenceStore,
			*m_workingCopyPersistenceAdapter, *m_workingCopyPersistenceAdapter,
			*m_workingCopyRecoveredInputAdopter,
			workbench::editor::persistence::EditorWorkingCopyLifecycleOptions{
				.debounceTicks = 1000,
				.maximumAgeTicks = 10000,
			});
	m_workingCopyLifecycleBridge =
		std::make_unique<workbench::editor::persistence::EditorWorkingCopyLifecycleBridge>(
			std::move(workingCopyScope), *m_workingCopyLifecycle, *m_workingCopyContextSource);

	//ウィンドウの作成
	m_pcEditWnd = new CEditWnd(
		*m_editorServiceLegacyAdapter, *m_legacyEditorBackend, *m_workingCopyCoordinator,
		*m_workingCopyLifecycleBridge, *m_workbenchRuntime, std::move(profileDirectory),
		std::move(extensionSecretStorage));
	if (!m_pcEditWnd->Create(m_pcEditDoc, &m_cIcons, nGroupId)) return false;

	//MRU管理
	m_pcMruListener = new CMruListener();

	//プロパティ管理
	m_pcPropertyManager = new CPropertyManager();
	m_pcPropertyManager->Create(
		m_pcEditWnd->GetHwnd(),
		&GetIcons(),
		&m_pcEditWnd->GetMenuDrawer()
	);
	return true;
}

CEditApp::~CEditApp() noexcept
{
	if (m_workingCopyLifecycleBridge) {
		m_workingCopyLifecycleBridge->BeginShutdown();
		(void)m_workingCopyLifecycleBridge->Flush(::GetTickCount64(), true);
		m_workingCopyLifecycleBridge->WillShutdown();
	}
	delete m_pcSMacroMgr;
	delete m_pcPropertyManager;
	delete m_pcMruListener;
	delete m_pcGrepAgent;
	delete m_pcVisualProgress;
	delete m_pcSaveAgent;
	delete m_pcLoadAgent;
	delete m_pcEditWnd;
	m_pcEditWnd = nullptr;
	if (m_workingCopyLifecycleBridge) m_workingCopyLifecycleBridge->Stop();
	m_workingCopyLifecycleBridge.reset();
	m_workingCopyLifecycle.reset();
	m_workingCopyRecoveredInputAdopter.reset();
	m_workingCopyPersistenceAdapter.reset();
	m_workingCopyContextSource.reset();
	m_workingCopyPersistenceStore.reset();
	if (m_workbenchRuntime) {
		const auto stopped = m_workbenchRuntime->Stop();
		if (stopped.snapshot.state != workbench::EWorkbenchRuntimeState::Stopped) {
			::OutputDebugStringW(L"Workbench runtime did not reach Stopped during editor shutdown.\n");
		}
		m_workbenchRuntime.reset();
	}
	m_editorServiceLegacyAdapter.reset();
	m_workingCopyCoordinator.reset();
	m_workingCopyBackend.reset();
	m_legacyEditorBackend.reset();
	delete m_pcEditDoc;
	m_pcEditDoc = nullptr;
	m_editorCoreService.reset();
}

workbench::editor::persistence::EditorWorkingCopyLifecycleResult CEditApp::RestoreWorkingCopies(
	const workbench::editor::persistence::EditorWorkingCopyRestorePolicy& policy)
{
	using namespace workbench::editor::persistence;
	if (!m_workingCopyLifecycleBridge || !m_pcEditDoc || !m_pcEditWnd) {
		return {
			.status = EEditorWorkingCopyLifecycleStatus::Failed,
			.reason = EEditorWorkingCopyLifecycleReason::InvalidSnapshot,
		};
	}
	auto result = m_workingCopyLifecycleBridge->Restore(policy, true);
	if (result.status == EEditorWorkingCopyLifecycleStatus::Succeeded) {
		// Recovery commits native content without loading a file.  Its first core
		// adoption is intentionally inactive; now rebuild native layout and make
		// the persisted (or explicit legacy one-input migrated) selection visible.
		// Any failure after that native commit is terminal for startup: the durable
		// backup is deliberately left intact for a later recovery attempt.
		if (!result.restoredInputId || !result.effectiveActiveInputId) {
			result.status = EEditorWorkingCopyLifecycleStatus::Failed;
			result.reason = EEditorWorkingCopyLifecycleReason::NativeProjectionFailed;
			return result;
		}
		try {
			m_pcEditDoc->OnChangeSetting(true, false, false);
		}
		catch (...) {
			result.status = EEditorWorkingCopyLifecycleStatus::Failed;
			result.reason = EEditorWorkingCopyLifecycleReason::NativeProjectionFailed;
			return result;
		}
		switch (m_pcEditWnd->ReconcileRecoveredEditorInput(
			*result.restoredInputId, *result.effectiveActiveInputId)) {
		case ERecoveredEditorProjectionResult::Succeeded:
			break;
		case ERecoveredEditorProjectionResult::CoreActivationFailed:
			result.status = EEditorWorkingCopyLifecycleStatus::Failed;
			result.reason = EEditorWorkingCopyLifecycleReason::CoreActivationFailed;
			break;
		case ERecoveredEditorProjectionResult::InvalidRecovery:
		case ERecoveredEditorProjectionResult::NativeProjectionFailed:
			result.status = EEditorWorkingCopyLifecycleStatus::Failed;
			result.reason = EEditorWorkingCopyLifecycleReason::NativeProjectionFailed;
			break;
		}
	}
	return result;
}

workbench::editor::EditorCoreService* CEditApp::GetEditorCoreService() noexcept
{
	return m_editorCoreService.get();
}

const workbench::editor::EditorCoreService* CEditApp::GetEditorCoreService() const noexcept
{
	return m_editorCoreService.get();
}

/*! 共通設定 プロパティシート */
bool CEditApp::OpenPropertySheet( int nPageNum )
{
	/* プロパティシートの作成 */
	bool bRet = m_pcPropertyManager->OpenPropertySheet( m_pcEditWnd->GetHwnd(), nPageNum, false );
	if( bRet ){
		// 2007.10.19 genta マクロ登録変更を反映するため，読み込み済みのマクロを破棄する
		m_pcSMacroMgr->UnloadAll();
	}

	return bRet;
}

/*! タイプ別設定 プロパティシート */
bool CEditApp::OpenPropertySheetTypes( int nPageNum, CTypeConfig nSettingType )
{
	bool bRet = m_pcPropertyManager->OpenPropertySheetTypes( m_pcEditWnd->GetHwnd(), nPageNum, nSettingType );

	return bRet;
}
