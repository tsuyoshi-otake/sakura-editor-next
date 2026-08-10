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

#include <exception>

namespace {

class EditorAppStartupInputs final {
public:
	EditorAppStartupInputs(
		HINSTANCE hInst,
		int groupId,
		workbench::WorkbenchBootstrapContext bootstrap,
		workbench::WorkbenchRuntimeDependencies dependencies,
		std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> workingCopyStore,
		workbench::editor::persistence::WorkingCopyPersistenceScope workingCopyScope,
		std::filesystem::path profileDirectory,
		std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage)
		: m_hInst(hInst)
		, m_groupId(groupId)
		, m_bootstrap(std::move(bootstrap))
		, m_dependencies(std::move(dependencies))
		, m_workingCopyStore(std::move(workingCopyStore))
		, m_workingCopyScope(std::move(workingCopyScope))
		, m_profileDirectory(std::move(profileDirectory))
		, m_extensionSecretStorage(std::move(extensionSecretStorage))
	{
	}

	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_hInst && m_workingCopyStore && m_workingCopyScope.IsValid()
			&& !m_profileDirectory.empty() && m_extensionSecretStorage;
	}
	[[nodiscard]] HINSTANCE GetInstance() const noexcept { return m_hInst; }
	[[nodiscard]] int GetGroupId() const noexcept { return m_groupId; }
	[[nodiscard]] workbench::WorkbenchBootstrapContext TakeBootstrap() { return std::move(m_bootstrap); }
	[[nodiscard]] workbench::WorkbenchRuntimeDependencies TakeDependencies() { return std::move(m_dependencies); }
	[[nodiscard]] std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> TakeWorkingCopyStore()
	{
		return std::move(m_workingCopyStore);
	}
	[[nodiscard]] workbench::editor::persistence::WorkingCopyPersistenceScope TakeWorkingCopyScope()
	{
		return std::move(m_workingCopyScope);
	}
	[[nodiscard]] std::filesystem::path TakeProfileDirectory() { return std::move(m_profileDirectory); }
	[[nodiscard]] std::unique_ptr<IExtensionSecretSessionStorage> TakeExtensionSecretStorage()
	{
		return std::move(m_extensionSecretStorage);
	}

private:
	HINSTANCE m_hInst = nullptr;
	int m_groupId = 0;
	workbench::WorkbenchBootstrapContext m_bootstrap;
	workbench::WorkbenchRuntimeDependencies m_dependencies;
	std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> m_workingCopyStore;
	workbench::editor::persistence::WorkingCopyPersistenceScope m_workingCopyScope;
	std::filesystem::path m_profileDirectory;
	std::unique_ptr<IExtensionSecretSessionStorage> m_extensionSecretStorage;
};

using editor::lifecycle::EEditorAppLifecycleFinalizationOutcome;
using editor::lifecycle::EEditorAppLifecyclePhase;
using editor::lifecycle::EEditorAppLifecyclePhaseOutcome;
using editor::lifecycle::EditorAppLifecycleFinalizationResult;
using editor::lifecycle::EditorAppLifecyclePhaseDefinition;
using editor::lifecycle::EditorAppLifecyclePhaseResult;

constexpr EditorAppLifecyclePhaseResult LifecycleSucceeded() noexcept
{
	return EditorAppLifecyclePhaseResult(EEditorAppLifecyclePhaseOutcome::Succeeded);
}

constexpr EditorAppLifecyclePhaseResult LifecycleFailed() noexcept
{
	return EditorAppLifecyclePhaseResult(EEditorAppLifecyclePhaseOutcome::Failed);
}

EditorAppLifecycleFinalizationResult FinalizationResult(bool succeeded) noexcept
{
	return EditorAppLifecycleFinalizationResult(succeeded ? EEditorAppLifecycleFinalizationOutcome::Succeeded
		: EEditorAppLifecycleFinalizationOutcome::Failed);
}

} // namespace

CEditApp::CEditApp() = default;

void CEditApp::SetAppInstanceForTesting(HINSTANCE hInst) noexcept
{
	m_hInst = hInst;
}

CEditDoc* CEditApp::AdoptDocumentForTesting(std::unique_ptr<CEditDoc> document) noexcept
{
	m_pcEditDoc = std::move(document);
	return GetDocument();
}

CEditWnd* CEditApp::AdoptEditWindowForTesting(std::unique_ptr<CEditWnd> editWindow) noexcept
{
	m_pcEditWnd = std::move(editWindow);
	return GetEditWindow();
}

CLoadAgent* CEditApp::AdoptLoadAgentForTesting(std::unique_ptr<CLoadAgent> loadAgent) noexcept
{
	m_pcLoadAgent = std::move(loadAgent);
	return GetLoadAgent();
}

CSaveAgent* CEditApp::AdoptSaveAgentForTesting(std::unique_ptr<CSaveAgent> saveAgent) noexcept
{
	m_pcSaveAgent = std::move(saveAgent);
	return GetSaveAgent();
}

CVisualProgress* CEditApp::AdoptVisualProgressForTesting(std::unique_ptr<CVisualProgress> visualProgress) noexcept
{
	m_pcVisualProgress = std::move(visualProgress);
	return GetVisualProgress();
}

CMruListener* CEditApp::AdoptMruListenerForTesting(std::unique_ptr<CMruListener> mruListener) noexcept
{
	m_pcMruListener = std::move(mruListener);
	return GetMruListener();
}

CSMacroMgr* CEditApp::AdoptMacroManagerForTesting(std::unique_ptr<CSMacroMgr> macroManager) noexcept
{
	m_pcSMacroMgr = std::move(macroManager);
	return GetMacroManager();
}

CPropertyManager* CEditApp::AdoptPropertyManagerForTesting(std::unique_ptr<CPropertyManager> propertyManager) noexcept
{
	m_pcPropertyManager = std::move(propertyManager);
	return GetPropertyManager();
}

CGrepAgent* CEditApp::AdoptGrepAgentForTesting(std::unique_ptr<CGrepAgent> grepAgent) noexcept
{
	m_pcGrepAgent = std::move(grepAgent);
	return GetGrepAgent();
}

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
	if (m_editorLifecycle) return false;
	auto inputs = std::make_shared<EditorAppStartupInputs>(
		hInst, nGroupId, std::move(bootstrap), std::move(dependencies), std::move(workingCopyStore),
		std::move(workingCopyScope), std::move(profileDirectory), std::move(extensionSecretStorage));

	try {
		m_editorLifecycle = std::make_unique<editor::lifecycle::EditorAppLifecycle>(
			std::vector<EditorAppLifecyclePhaseDefinition>{
				EditorAppLifecyclePhaseDefinition(
					EEditorAppLifecyclePhase::ProfileResolution,
					[this, inputs] {
						if (!inputs->IsValid()) {
							return LifecycleFailed();
						}
						m_hInst = inputs->GetInstance();
						return LifecycleSucceeded();
					},
					[this] {
						m_hInst = nullptr;
						return FinalizationResult(true);
					}),
				EditorAppLifecyclePhaseDefinition(
					EEditorAppLifecyclePhase::PlatformServices,
					[this, inputs] {
						try {
							auto runtime = std::make_unique<workbench::CWorkbenchRuntime>(
								inputs->TakeBootstrap(), config::BuiltinConfigurationDescriptors(), inputs->TakeDependencies());
							const auto started = runtime->Start();
							if (!started.IsUsable()) return LifecycleFailed();
							m_workbenchRuntime = std::move(runtime);
							return LifecycleSucceeded();
						}
						catch (const std::exception&) {
							return LifecycleFailed();
						}
					},
					[this] { return FinalizationResult(FinalizePlatformServices()); }),
				EditorAppLifecyclePhaseDefinition(
					EEditorAppLifecyclePhase::WorkbenchCreation,
					[this, inputs] {
						try {
							m_editorCoreService = std::make_unique<workbench::editor::EditorCoreService>();

							// Native/core ownership belongs to this phase and is released
							// before the platform runtime's reverse finalizer runs.
							m_cIcons.Create(m_hInst);
							m_pcEditDoc = std::make_unique<CEditDoc>(this);
							m_pcLoadAgent = std::make_unique<CLoadAgent>();
							m_pcSaveAgent = std::make_unique<CSaveAgent>();
							m_pcVisualProgress = std::make_unique<CVisualProgress>();
							m_pcGrepAgent = std::make_unique<CGrepAgent>();
							CAppMode::getInstance();
							m_pcSMacroMgr = std::make_unique<CSMacroMgr>();
							m_pcEditDoc->Create();
							m_legacyEditorBackend = std::make_unique<workbench::editor::CEditDocLegacyEditorBackend>(*m_pcEditDoc);
							m_editorServiceLegacyAdapter = std::make_unique<workbench::editor::CEditorServiceLegacyAdapter>(
								*m_editorCoreService, *m_legacyEditorBackend);
							m_workingCopyBackend = std::make_unique<workbench::editor::CEditDocWorkingCopyBackend>(*m_pcEditDoc);
							m_workingCopyCoordinator = std::make_unique<workbench::editor::EditorWorkingCopyCoordinator>(
								*m_editorCoreService, *m_workingCopyBackend);
							m_workingCopyPersistenceStore = inputs->TakeWorkingCopyStore();
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
								inputs->TakeWorkingCopyScope(), *m_workingCopyLifecycle, *m_workingCopyContextSource);
							m_pcEditWnd = std::make_unique<CEditWnd>(
								*m_editorServiceLegacyAdapter, *m_legacyEditorBackend, *m_workingCopyCoordinator,
								*m_workingCopyLifecycleBridge, *m_workbenchRuntime, inputs->TakeProfileDirectory(),
								inputs->TakeExtensionSecretStorage());
							return m_pcEditWnd->Create(m_pcEditDoc.get(), &m_cIcons, inputs->GetGroupId())
								? LifecycleSucceeded() : LifecycleFailed();
						}
						catch (const std::exception&) {
							return LifecycleFailed();
						}
					},
					[this] { return FinalizationResult(FinalizeWorkbenchResources()); }),
				EditorAppLifecyclePhaseDefinition(
					EEditorAppLifecyclePhase::ExtensionSession,
					[this] {
						try {
							m_pcMruListener = std::make_unique<CMruListener>();
							m_pcPropertyManager = std::make_unique<CPropertyManager>();
							m_pcPropertyManager->Create(m_pcEditWnd->GetHwnd(), &GetIcons(),
								&m_pcEditWnd->GetMenuDrawer());
							return LifecycleSucceeded();
						}
						catch (const std::exception&) {
							return LifecycleFailed();
						}
					},
					[this] {
						m_pcPropertyManager.reset();
						m_pcMruListener.reset();
						return FinalizationResult(true);
					}),
				EditorAppLifecyclePhaseDefinition(
					EEditorAppLifecyclePhase::Ready,
					[] { return LifecycleSucceeded(); },
					[] { return FinalizationResult(true); }),
			});
		return m_editorLifecycle->Start().IsRunning();
	}
	catch (const std::exception&) {
		return false;
	}
}

CEditApp::~CEditApp() noexcept
{
	const auto stopped = Stop();
	if (stopped.ForcedShutdown()) {
		::OutputDebugStringW(L"Editor application lifecycle required forced shutdown.\n");
	}
	m_editorLifecycle.reset();
}

editor::lifecycle::EditorAppLifecycleResult CEditApp::Stop() noexcept
{
	if (!m_editorLifecycle) {
		return editor::lifecycle::EditorAppLifecycleResult(
			editor::lifecycle::EEditorAppLifecycleResultCode::AlreadyStopped,
			editor::lifecycle::EEditorAppLifecycleState::Stopped);
	}
	return m_editorLifecycle->Stop();
}

bool CEditApp::FinalizeWorkbenchResources()
{
	bool finalized = true;
	if (m_workingCopyLifecycleBridge) {
		m_workingCopyLifecycleBridge->BeginShutdown();
		const auto flushed = m_workingCopyLifecycleBridge->Flush(::GetTickCount64(), true);
		if (flushed.status == workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::Failed
			|| flushed.status == workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::Conflict
			|| flushed.status == workbench::editor::persistence::EEditorWorkingCopyLifecycleStatus::Unsupported) {
			finalized = false;
		}
		m_workingCopyLifecycleBridge->WillShutdown();
	}
	m_pcSMacroMgr.reset();
	m_pcGrepAgent.reset();
	m_pcVisualProgress.reset();
	m_pcSaveAgent.reset();
	m_pcLoadAgent.reset();
	m_pcEditWnd.reset();
	if (m_workingCopyLifecycleBridge) m_workingCopyLifecycleBridge->Stop();
	m_workingCopyLifecycleBridge.reset();
	m_workingCopyLifecycle.reset();
	m_workingCopyRecoveredInputAdopter.reset();
	m_workingCopyPersistenceAdapter.reset();
	m_workingCopyContextSource.reset();
	m_workingCopyPersistenceStore.reset();
	m_editorServiceLegacyAdapter.reset();
	m_workingCopyCoordinator.reset();
	m_workingCopyBackend.reset();
	m_legacyEditorBackend.reset();
	m_pcEditDoc.reset();
	m_editorCoreService.reset();
	return finalized;
}

bool CEditApp::FinalizePlatformServices()
{
	bool finalized = true;
	if (m_workbenchRuntime) {
		const auto stopped = m_workbenchRuntime->Stop();
		if (stopped.snapshot.state != workbench::EWorkbenchRuntimeState::Stopped) {
			::OutputDebugStringW(L"Workbench runtime did not reach Stopped during editor shutdown.\n");
			finalized = false;
		}
		m_workbenchRuntime.reset();
	}
	return finalized;
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
		catch (const std::exception&) {
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
