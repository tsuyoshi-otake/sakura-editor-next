/*! @file */
/*
	Copyright (C) 2007, kobake
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_
#define SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_
#pragma once

//2007.10.23 kobake 作成

#include "util/design_template.h"
#include "uiparts/CSoundSet.h"
#include "uiparts/CImageListMgr.h"
#include "types/CType.h"

#include <sakura/editor/lifecycle/EditorAppLifecycle.h>

#include <filesystem>
#include <memory>

namespace workbench::editor {
	class CEditDocLegacyEditorBackend;
	class CEditDocWorkingCopyBackend;
	class CEditorServiceLegacyAdapter;
	class EditorCoreService;
	class EditorWorkingCopyCoordinator;
}

namespace workbench::editor::persistence {
	class CEditDocWorkingCopyPersistenceAdapter;
	class EditorCoreRecoveredInputAdopter;
	class EditorCoreWorkingCopyCaptureContextSource;
	class EditorWorkingCopyLifecycle;
	class EditorWorkingCopyLifecycleBridge;
	class IWorkingCopyPersistenceStore;
	struct EditorWorkingCopyLifecycleResult;
	struct EditorWorkingCopyRestorePolicy;
	struct WorkingCopyPersistenceScope;
}

namespace workbench {
class CWorkbenchRuntime;
class WorkbenchBootstrapContext;
struct WorkbenchRuntimeDependencies;
}

class CEditDoc;
class CEditWnd;
class CLoadAgent;
class CSaveAgent;
class CVisualProgress;
class CMruListener;
class CSMacroMgr;
class CPropertyManager;
class CGrepAgent;
class IExtensionSecretSessionStorage;
enum EFunctionCode;

//!エディタ部分アプリケーションクラス。CNormalProcess1個につき、1個存在。
class CEditApp final : public TSakuraSingleton<CEditApp> {
public:
	CEditApp();
	~CEditApp() noexcept;

	//! Creates the process-local workbench services before the native window.
	//! A false result is terminal for this editor-process startup attempt.
	[[nodiscard]] bool Create(
		HINSTANCE hInst,
		int groupId,
		workbench::WorkbenchBootstrapContext bootstrap,
		workbench::WorkbenchRuntimeDependencies dependencies,
		std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> workingCopyStore,
		workbench::editor::persistence::WorkingCopyPersistenceScope workingCopyScope,
		std::filesystem::path profileDirectory,
		std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage);
	//! Restore is invoked after the native layout/group exists and startup policy is known.
	[[nodiscard]] workbench::editor::persistence::EditorWorkingCopyLifecycleResult RestoreWorkingCopies(
		const workbench::editor::persistence::EditorWorkingCopyRestorePolicy& policy);
	//! Idempotently releases process-local editor ownership. Each startup phase
	//! declares its own reverse finalizer through m_editorLifecycle.
	[[nodiscard]] editor::lifecycle::EditorAppLifecycleResult Stop() noexcept;

	//モジュール情報
	HINSTANCE GetAppInstance() const{ return m_hInst; }	//!< インスタンスハンドル取得

	//ウィンドウ情報
	CEditWnd* GetEditWindow(){ return m_pcEditWnd.get(); }		//!< ウィンドウ取得

	CEditDoc*		GetDocument(){ return m_pcEditDoc.get(); }
	CLoadAgent*		GetLoadAgent(){ return m_pcLoadAgent.get(); }
	CSaveAgent*		GetSaveAgent(){ return m_pcSaveAgent.get(); }
	CVisualProgress*	GetVisualProgress(){ return m_pcVisualProgress.get(); }
	CMruListener*		GetMruListener(){ return m_pcMruListener.get(); }
	CSMacroMgr*		GetMacroManager(){ return m_pcSMacroMgr.get(); }
	CPropertyManager*	GetPropertyManager(){ return m_pcPropertyManager.get(); }
	CGrepAgent*		GetGrepAgent(){ return m_pcGrepAgent.get(); }
	// Test fixtures transfer ownership explicitly, then retain only borrowed
	// pointers through the accessors above.
	void SetAppInstanceForTesting(HINSTANCE hInst) noexcept;
	[[nodiscard]] CEditDoc* AdoptDocumentForTesting(std::unique_ptr<CEditDoc> document) noexcept;
	[[nodiscard]] CEditWnd* AdoptEditWindowForTesting(std::unique_ptr<CEditWnd> editWindow) noexcept;
	[[nodiscard]] CLoadAgent* AdoptLoadAgentForTesting(std::unique_ptr<CLoadAgent> loadAgent) noexcept;
	[[nodiscard]] CSaveAgent* AdoptSaveAgentForTesting(std::unique_ptr<CSaveAgent> saveAgent) noexcept;
	[[nodiscard]] CVisualProgress* AdoptVisualProgressForTesting(std::unique_ptr<CVisualProgress> visualProgress) noexcept;
	[[nodiscard]] CMruListener* AdoptMruListenerForTesting(std::unique_ptr<CMruListener> mruListener) noexcept;
	[[nodiscard]] CSMacroMgr* AdoptMacroManagerForTesting(std::unique_ptr<CSMacroMgr> macroManager) noexcept;
	[[nodiscard]] CPropertyManager* AdoptPropertyManagerForTesting(std::unique_ptr<CPropertyManager> propertyManager) noexcept;
	[[nodiscard]] CGrepAgent* AdoptGrepAgentForTesting(std::unique_ptr<CGrepAgent> grepAgent) noexcept;
	CImageListMgr&	GetIcons(){ return m_cIcons; }
	[[nodiscard]] workbench::editor::EditorCoreService* GetEditorCoreService() noexcept;
	[[nodiscard]] const workbench::editor::EditorCoreService* GetEditorCoreService() const noexcept;

	bool OpenPropertySheet( int nPageNum );
	bool OpenPropertySheetTypes( int nPageNum, CTypeConfig nSettingType );

	// The legacy presentation members remain public only while their value-type
	// APIs are used directly. Process-owned resources stay private below.
	CSoundSet			m_cSoundSet;					//!< サウンド管理

	//GUIオブジェクト
	CImageListMgr		m_cIcons;					//!< Image List

private:
	HINSTANCE			m_hInst = nullptr;

	// Process-owned legacy resources. Other subsystems borrow them solely
	// through the accessors above; lifecycle finalizers retain exclusive ownership.
	std::unique_ptr<CEditDoc>	m_pcEditDoc;
	std::unique_ptr<CEditWnd>	m_pcEditWnd;
	std::unique_ptr<CLoadAgent>		m_pcLoadAgent;
	std::unique_ptr<CSaveAgent>		m_pcSaveAgent;
	std::unique_ptr<CVisualProgress>	m_pcVisualProgress;
	std::unique_ptr<CMruListener>	m_pcMruListener;
	std::unique_ptr<CSMacroMgr>	m_pcSMacroMgr;
	std::unique_ptr<CPropertyManager>	m_pcPropertyManager;
	std::unique_ptr<CGrepAgent>		m_pcGrepAgent;

	// The authoritative process-local editor model. CEditDoc may remain allocated
	// while this service legitimately contains zero open inputs.
	std::unique_ptr<workbench::editor::EditorCoreService> m_editorCoreService;
	// Composition-root owned migration seam. The window borrows these objects;
	// the adapter borrows both the core and the explicitly prepared legacy backend.
	std::unique_ptr<workbench::editor::CEditDocLegacyEditorBackend> m_legacyEditorBackend;
	std::unique_ptr<workbench::editor::CEditorServiceLegacyAdapter> m_editorServiceLegacyAdapter;
	// Imperative working-copy effects are separate from the read-only adoption
	// backend.  The coordinator borrows the core and this backend, and both
	// outlive the native window that dispatches the stable workbench commands.
	std::unique_ptr<workbench::editor::CEditDocWorkingCopyBackend> m_workingCopyBackend;
	std::unique_ptr<workbench::editor::EditorWorkingCopyCoordinator> m_workingCopyCoordinator;
	// Hot Exit composition. The bridge and lifecycle are destroyed before every
	// borrowed adapter, store, document, core service, and control runtime.
	std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> m_workingCopyPersistenceStore;
	std::unique_ptr<workbench::editor::persistence::EditorCoreWorkingCopyCaptureContextSource> m_workingCopyContextSource;
	std::unique_ptr<workbench::editor::persistence::CEditDocWorkingCopyPersistenceAdapter> m_workingCopyPersistenceAdapter;
	std::unique_ptr<workbench::editor::persistence::EditorCoreRecoveredInputAdopter> m_workingCopyRecoveredInputAdopter;
	std::unique_ptr<workbench::editor::persistence::EditorWorkingCopyLifecycle> m_workingCopyLifecycle;
	std::unique_ptr<workbench::editor::persistence::EditorWorkingCopyLifecycleBridge> m_workingCopyLifecycleBridge;
	// Owns profile/configuration/workspace truth for this editor window. The
	// native window borrows it and is destroyed before this runtime is stopped.
	std::unique_ptr<workbench::CWorkbenchRuntime> m_workbenchRuntime;
	//! The composition owner. It is destroyed before the resources declared above,
	//! so its callbacks can finalize each owner while all members are still valid.
	std::unique_ptr<editor::lifecycle::EditorAppLifecycle> m_editorLifecycle;

	[[nodiscard]] bool FinalizeWorkbenchResources();
	[[nodiscard]] bool FinalizePlatformServices();
};

//WM_QUIT検出例外
class CAppExitException : public std::exception{
public:
	const char* what() const throw() override{ return "CAppExitException"; }
};

#endif /* SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_ */
